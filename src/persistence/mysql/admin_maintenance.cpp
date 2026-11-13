#include "persistence/mysql/admin_maintenance.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

constexpr std::string_view kActionCode = "execute_clan_disbands";

application::ServiceError Error(application::ErrorCode code,
                                std::string message, bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

bool CanonicalUuid(std::string_view value) noexcept {
  if (value.size() != 36U) {
    return false;
  }
  for (std::size_t index = 0U; index < value.size(); ++index) {
    if (index == 8U || index == 13U || index == 18U || index == 23U) {
      if (value[index] != '-') {
        return false;
      }
      continue;
    }
    const auto character = static_cast<unsigned char>(value[index]);
    if (!std::isdigit(character) && (character < 'a' || character > 'f')) {
      return false;
    }
  }
  return true;
}

std::optional<application::ServiceError> Validate(
    const ExecuteClanDisbandsCommand& command) {
  if (command.actor.empty() || command.actor.size() > 128U) {
    return Error(application::ErrorCode::kInvalidInput,
                 "维护命令操作者长度必须为 1 至 128 字节");
  }
  if (command.reason.empty() || command.reason.size() > 1024U) {
    return Error(application::ErrorCode::kInvalidInput,
                 "维护命令原因长度必须为 1 至 1024 字节");
  }
  if (!CanonicalUuid(command.business_id)) {
    return Error(application::ErrorCode::kInvalidInput,
                 "维护命令业务 ID 必须是小写规范 UUID");
  }
  if (command.limit == 0U || command.limit > 1000U) {
    return Error(application::ErrorCode::kInvalidInput,
                 "维护命令批量上限必须为 1 至 1000");
  }
  return std::nullopt;
}

template <typename Row>
ExecuteClanDisbandsReport PriorReport(const Row& row) {
  const auto snapshot = nlohmann::json::parse(row[1].As<std::string>());
  ExecuteClanDisbandsReport report{.already_applied = true};
  if (snapshot.contains("clan_ids")) {
    report.clan_ids = snapshot.at("clan_ids").get<std::vector<std::uint64_t>>();
  }
  return report;
}

}  // namespace

application::ServiceResult<ExecuteClanDisbandsReport> ExecuteDueClanDisbands(
    MySqlConnectionPool& pool, const ExecuteClanDisbandsCommand& command) {
  if (const auto validation_error = Validate(command);
      validation_error.has_value()) {
    return *validation_error;
  }
  auto lease_result = pool.Acquire();
  if (const auto* acquire_error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *acquire_error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto previous = transaction.Exec(
        "SELECT `ActionCode`,`TargetSnapshot` FROM `AdminAuditEvents` WHERE `BusinessId`=$1 FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.business_id});
    if (!previous.empty()) {
      if (previous[0][0].As<std::string>() != kActionCode) {
        return Error(application::ErrorCode::kConflict,
                     "维护命令业务 ID 已用于其他管理操作");
      }
      const auto snapshot =
          nlohmann::json::parse(previous[0][1].As<std::string>());
      if (snapshot.value("native_requests_only", false) !=
          command.native_requests_only) {
        return Error(application::ErrorCode::kConflict,
                     "维护命令业务 ID 已用于另一执行范围");
      }
      auto report = PriorReport(previous[0]);
      transaction.Commit();
      return report;
    }

    const auto requests = transaction.Exec(
        "SELECT request.`DisbandRequestId`,request.`ClanId` FROM `ClanDisbandRequests` AS request "
        "JOIN `Clans` AS clan ON clan.`ClanId`=request.`ClanId` WHERE "
        "request.`RequestStatus`='pending' AND request.`ExecuteAfter`<=CURRENT_TIMESTAMP(6) AND "
        "clan.`ClanStatus`='pending_disband' AND (NOT $2 OR (request.`LegacyImportBatchId` IS NULL "
        "AND clan.`ClanKind`='standard' AND clan.`LeaderPlayerId`=request.`RequestedByPlayerId` AND"
        " EXISTS(SELECT 1 FROM `ClanMemberships` membership WHERE membership.`ClanId`=clan.`ClanId`"
        " AND membership.`PlayerId`=request.`RequestedByPlayerId` AND "
        "membership.`RoleCode`='master'))) ORDER BY request.`DisbandRequestId` LIMIT $1 FOR UPDATE "
        "OF request,clan SKIP LOCKED",
        bf::infrastructure::mysql::Parameters{transaction, command.limit,
                                              command.native_requests_only});

    ExecuteClanDisbandsReport report;
    std::vector<std::uint64_t> request_ids;
    report.clan_ids.reserve(requests.size());
    request_ids.reserve(requests.size());
    for (const auto& request : requests) {
      const auto request_id = request[0].As<std::uint64_t>();
      const auto clan_id = request[1].As<std::uint64_t>();
      transaction.Exec(
          "UPDATE `ClanApplications` SET "
          "`ApplicationStatus`='rejected',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
          "`ApplicationStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "UPDATE `ClanLeadershipTransfers` SET "
          "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
          "`TransferStatus` IN ('pending','accepted')",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "DELETE FROM `ClanMemberships` WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "UPDATE `ClanDisbandRequests` SET "
          "`RequestStatus`='executed',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `DisbandRequestId`=$1 "
          "AND `RequestStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, request_id});
      transaction.Exec(
          "UPDATE `Clans` SET `ClanStatus`='disbanded' WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "INSERT INTO "
          "`ClanAuditEvents`(`ClanId`,`ActorSnapshot`,`ActionCode`,`TargetSnapshot`,`Detail`) "
          "VALUES($1,$2,'disband','{}',JSON_OBJECT('business_id',$3,'reason',$4))",
          bf::infrastructure::mysql::Parameters{
              transaction, clan_id, "maintenance:" + command.actor,
              command.business_id, command.reason});
      request_ids.push_back(request_id);
      report.clan_ids.push_back(clan_id);
    }

    const nlohmann::json snapshot{
        {"clan_ids", report.clan_ids},
        {"disband_request_ids", request_ids},
        {"native_requests_only", command.native_requests_only}};
    transaction.Exec(
        "INSERT INTO "
        "`AdminAuditEvents`(`AdminAuditEventId`,`Actor`,`ActionCode`,`BusinessId`,`TargetSnapshot`,`Reason`)"
        " VALUES($1,$2,$3,$1,$4,$5)",
        bf::infrastructure::mysql::Parameters{transaction, command.business_id,
                                              command.actor, kActionCode,
                                              snapshot.dump(), command.reason});
    transaction.Commit();
    return report;
  } catch (const nlohmann::json::exception&) {
    return Error(application::ErrorCode::kInternal, "既有维护审计结果格式无效");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return Error(application::ErrorCode::kStorageUnavailable,
                 "执行战队解散维护时数据库不可用", true);
  } catch (const bf::infrastructure::mysql::SqlError&) {
    return Error(application::ErrorCode::kInternal, "执行战队解散维护事务失败");
  } catch (const std::exception&) {
    return Error(application::ErrorCode::kInternal,
                 "执行战队解散维护时发生内部错误");
  }
}

}  // namespace bf::infrastructure::mysql
