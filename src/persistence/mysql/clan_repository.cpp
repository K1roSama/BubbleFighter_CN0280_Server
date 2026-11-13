#include "persistence/mysql/clan_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

using application::ClanApplicantView;
using application::ClanChatResult;
using application::ClanHomeState;
using application::ClanMemberChangeResult;
using application::ClanMemberView;
using application::ClanPostView;
using application::ClanSearchPage;
using application::ClanSummary;
using application::ErrorCode;
using application::ServiceError;
using application::ServiceResult;

ServiceError MakeError(ErrorCode code, std::string message,
                       bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

ServiceError StorageError(std::string message, bool retryable = false) {
  return MakeError(ErrorCode::kStorageUnavailable, std::move(message),
                   retryable);
}

std::chrono::system_clock::time_point FromUnixSeconds(std::int64_t value) {
  return std::chrono::system_clock::time_point{std::chrono::seconds{value}};
}

std::int64_t UnixSeconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

template <typename Field>
std::optional<domain::PlayerId> PlayerId(const Field& field) {
  if (field.IsNull()) {
    return std::nullopt;
  }
  const auto value = field.As<std::int64_t>();
  if (value <= 0) {
    return std::nullopt;
  }
  return domain::PlayerId{static_cast<std::uint64_t>(value)};
}

template <typename Field>
std::optional<std::uint32_t> UnsignedValue(const Field& field) {
  if (field.IsNull()) {
    return std::nullopt;
  }
  const auto value = field.As<std::int64_t>();
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       static_cast<std::uint64_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> ProtocolRole(std::string_view role) {
  if (role == "member") {
    return application::kClanMemberRole;
  }
  if (role == "master") {
    return application::kClanMasterRole;
  }
  if (role == "deputy") {
    return application::kClanDeputyRole;
  }
  return std::nullopt;
}

std::optional<std::string_view> StoredRole(std::int32_t role) {
  if (role == static_cast<std::int32_t>(application::kClanMemberRole)) {
    return "member";
  }
  if (role == static_cast<std::int32_t>(application::kClanMasterRole)) {
    return "master";
  }
  if (role == static_cast<std::int32_t>(application::kClanDeputyRole)) {
    return "deputy";
  }
  return std::nullopt;
}

std::string ClanNameKey(std::string_view name) {
  std::string key{name};
  std::ranges::transform(key, key.begin(), [](unsigned char character) {
    return static_cast<char>(character >= 'A' && character <= 'Z'
                                 ? character + ('a' - 'A')
                                 : character);
  });
  return key;
}

template <typename Row>
ServiceResult<ClanSummary> SummaryFromRow(const Row& row) {
  if (row.size() != 15U) {
    return MakeError(ErrorCode::kInternal, "战队资料查询列数不符合契约");
  }
  const auto mark = UnsignedValue(row[6]);
  const auto capacity = UnsignedValue(row[7]);
  const auto members = UnsignedValue(row[8]);
  if (!mark.has_value() || !capacity.has_value() || !members.has_value()) {
    return MakeError(ErrorCode::kInternal, "战队资料包含越界数值");
  }
  std::optional<std::chrono::system_clock::time_point> disband_requested_at;
  if (!row[11].IsNull()) {
    disband_requested_at = FromUnixSeconds(row[11].As<std::int64_t>());
  }
  return ClanSummary{
      .clan_id = row[0].As<std::uint64_t>(),
      .name = row[1].As<std::string>(),
      .leader_player_id = PlayerId(row[2]),
      .leader_nickname = row[3].As<std::string>(),
      .introduction = row[4].As<std::string>(),
      .notice = row[5].As<std::string>(),
      .mark_code = *mark,
      .capacity = *capacity,
      .member_count = *members,
      .pending_disband = row[9].As<bool>(),
      .created_at = FromUnixSeconds(row[10].As<std::int64_t>()),
      .disband_requested_at = disband_requested_at,
      .recruitment_mode = row[12].As<std::uint32_t>(),
      .level = row[13].As<std::uint32_t>(),
      .level_points = row[14].As<std::uint32_t>(),
  };
}

bf::infrastructure::mysql::QueryResult SummaryRows(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::uint64_t clan_id) {
  return transaction.Exec(
      "SELECT "
      "clan.`ClanId`,clan.`ClanName`,clan.`LeaderPlayerId`,coalesce(leader.`Nickname`,''),clan.`Introduction`,clan.`Notice`,clan.`MarkCode`,clan.`Capacity`,count(member.`PlayerId`),clan.`ClanStatus`='pending_disband',floor(UNIX_TIMESTAMP(clan.`CreatedAt`)),floor(UNIX_TIMESTAMP(disband.`CreatedAt`)),clan.`RecruitmentMode`,coalesce((SELECT"
      " ClanLevel FROM `ClanActivityTotals` WHERE `ClanId`=clan.`ClanId`),1),coalesce((SELECT "
      "LevelPoints FROM `ClanActivityTotals` WHERE `ClanId`=clan.`ClanId`),0) FROM `Clans` AS "
      "clan LEFT JOIN `PlayerProfiles` AS leader ON leader.`PlayerId`=clan.`LeaderPlayerId` LEFT "
      "JOIN `ClanMemberships` AS member ON member.`ClanId`=clan.`ClanId` LEFT JOIN "
      "`ClanDisbandRequests` AS disband ON disband.`ClanId`=clan.`ClanId` AND "
      "disband.`RequestStatus`='pending' WHERE clan.`ClanId`=$1 GROUP BY "
      "clan.`ClanId`,leader.`Nickname`,disband.`CreatedAt`",
      bf::infrastructure::mysql::Parameters{transaction, clan_id});
}

ServiceResult<ClanSummary> LoadSummary(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::uint64_t clan_id, bool lock_parent = false) {
  if (lock_parent) {
    transaction.Exec(
        "SELECT 1 FROM `Clans` WHERE `ClanId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
  }
  const auto rows = SummaryRows(transaction, clan_id);
  if (rows.empty()) {
    return MakeError(ErrorCode::kInvalidState, "战队不存在或已解散");
  }
  return SummaryFromRow(rows[0]);
}

ServiceResult<std::pair<std::uint64_t, std::string>> ManagerClan(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player, bool master_only, bool lock_parent) {
  const auto rows = transaction.Exec(
      "SELECT clan.`ClanId`,membership.`RoleCode` FROM `ClanMemberships` AS membership JOIN "
      "`Clans` AS clan ON clan.`ClanId`=membership.`ClanId` WHERE membership.`PlayerId`=$1 AND "
      "clan.`ClanKind`='standard'" +
          std::string{lock_parent ? " FOR UPDATE OF clan" : ""},
      bf::infrastructure::mysql::Parameters{transaction, player.Value()});
  if (rows.empty()) {
    return MakeError(ErrorCode::kPermissionDenied,
                     master_only ? "仅会长可以操作" : "仅会长或副会长可以操作");
  }
  const auto role = rows[0][1].As<std::string>();
  if ((master_only && role != "master") ||
      (!master_only && role != "master" && role != "deputy")) {
    return MakeError(ErrorCode::kPermissionDenied,
                     master_only ? "仅会长可以操作" : "仅会长或副会长可以操作");
  }
  return std::pair{rows[0][0].As<std::uint64_t>(), role};
}

ServiceResult<std::uint32_t> DisplayLevel(std::int64_t experience,
                                          std::int64_t step,
                                          std::int64_t maximum) {
  if (experience < 0 || step <= 0 || maximum <= 0 || maximum > 1000) {
    return MakeError(ErrorCode::kInternal, "战队成员等级规则或经验无效");
  }
  std::int64_t level = 1;
  while (level < maximum && experience >= step * level * (level + 1) / 2) {
    ++level;
  }
  return static_cast<std::uint32_t>(level);
}

std::pair<std::int64_t, std::int64_t> RewardLevelPolicy(
    bf::infrastructure::mysql::TransactionBase& transaction) {
  const auto rows = transaction.Exec(
      "SELECT "
      "(JSON_UNQUOTE(JSON_EXTRACT(JSON_EXTRACT(`Payload`,'$.policy'),'$.level_step'))),(JSON_UNQUOTE(JSON_EXTRACT(JSON_EXTRACT(`Payload`,'$.policy'),'$.maximum_level')))"
      " FROM `ConfigurationVersions` WHERE `ConfigurationKind`='reward_rules' AND "
      "`ReleaseStatus`='published'");
  if (rows.size() != 1U || rows[0][0].IsNull() || rows[0][1].IsNull()) {
    throw std::runtime_error{"战队查询要求唯一完整的已发布奖励规则"};
  }
  return {rows[0][0].As<std::int64_t>(), rows[0][1].As<std::int64_t>()};
}

void WriteAudit(bf::infrastructure::mysql::TransactionBase& transaction,
                std::uint64_t clan_id, domain::PlayerId actor,
                std::string_view action,
                std::optional<domain::PlayerId> target = std::nullopt,
                std::string_view detail = {}) {
  transaction.Exec(
      "INSERT INTO "
      "`ClanAuditEvents`(`ClanId`,`ActorPlayerId`,`ActorSnapshot`,`ActionCode`,`TargetPlayerId`,`TargetSnapshot`,`Detail`)"
      " SELECT $1,$2,`Actor`.`Nickname`,$3,$4,JSON_OBJECT('player_id',$4),JSON_OBJECT('value',$5)"
      " FROM `PlayerProfiles` AS `Actor` WHERE `Actor`.`PlayerId`=$2",
      bf::infrastructure::mysql::Parameters{
          transaction, clan_id, actor.Value(), action,
          target.has_value() ? std::optional<std::uint64_t>{target->Value()}
                             : std::nullopt,
          detail});
}

template <typename Value>
ServiceResult<Value> CaughtDatabaseError(
    std::string_view operation,
    const bf::infrastructure::mysql::BrokenConnection&) {
  return StorageError(std::string{operation} + "时 MySQL 连接中断", true);
}

template <typename Value>
ServiceResult<Value> CaughtDatabaseError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return StorageError("MySQL 拒绝" + std::string{operation});
}

template <typename Value>
ServiceResult<Value> CaughtDatabaseError(std::string_view operation,
                                         const std::exception&) {
  return StorageError(std::string{operation} + "时发生未分类错误");
}

}  // namespace

MySqlClanRepository::MySqlClanRepository(MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

ServiceResult<application::ClanMarkResult> MySqlClanRepository::ChangeMark(
    domain::PlayerId player, std::uint32_t mark_code) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto membership = transaction.Exec(
        "SELECT clan.`ClanId`,clan.`MarkCode` FROM `Clans` clan JOIN `ClanMemberships` member ON "
        "member.`ClanId`=clan.`ClanId` WHERE member.`PlayerId`=$1 AND member.`RoleCode`='master' "
        "AND clan.`LeaderPlayerId`=$1 AND clan.`ClanKind`='standard' AND clan.`ClanStatus`='active'"
        " FOR UPDATE OF clan,member",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (membership.empty()) {
      return MakeError(ErrorCode::kPermissionDenied,
                       "只有正常家族的会长可以更换徽章");
    }
    const auto card = transaction.Exec(
        "SELECT `ItemInstanceId`,`Quantity` FROM `PlayerItems` WHERE `PlayerId`=$1 AND "
        "`ItemId`=101050 AND (`ExpiresAt` IS NULL OR `ExpiresAt`>CURRENT_TIMESTAMP(6)) FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    const auto quantity = card.empty() ? 0U : card[0][1].As<std::uint32_t>();
    if (membership[0][1].As<std::uint32_t>() == mark_code) {
      transaction.Commit();
      return application::ClanMarkResult{.mark_code = mark_code,
                                         .remaining_cards = quantity};
    }
    if (card.empty() || quantity == 0U) {
      return MakeError(ErrorCode::kConflict, "缺少家族徽章兑换券");
    }
    const auto instance = card[0][0].As<std::uint64_t>();
    if (quantity == 1U) {
      transaction.Exec(
          "DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, instance});
      transaction.Exec(
          "DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, instance});
    } else {
      transaction.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, instance});
    }
    const auto clan_id = membership[0][0].As<std::uint64_t>();
    transaction.Exec(
        "UPDATE `Clans` SET `MarkCode`=$2 WHERE `ClanId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, clan_id, mark_code});
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,101050,NULL,-1,'clan_mark_consume',UUID())",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    WriteAudit(transaction, clan_id, player, "change_mark", player,
               std::to_string(mark_code));
    transaction.Commit();
    return application::ClanMarkResult{.mark_code = mark_code,
                                       .remaining_cards = quantity - 1U,
                                       .consumed_cards = 1U};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<application::ClanMarkResult>("更换家族徽章",
                                                            error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<application::ClanMarkResult>("更换家族徽章",
                                                            error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<application::ClanMarkResult>("更换家族徽章",
                                                            error);
  }
}

ServiceResult<ClanHomeState> MySqlClanRepository::LoadHome(
    domain::PlayerId player) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    ClanHomeState home;
    const auto membership = transaction.Exec(
        "SELECT `ClanId`,`RoleCode` FROM `ClanMemberships` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!membership.empty()) {
      auto summary =
          LoadSummary(transaction, membership[0][0].As<std::uint64_t>());
      if (const auto* error = std::get_if<ServiceError>(&summary)) {
        return *error;
      }
      const auto role = ProtocolRole(membership[0][1].As<std::string>());
      if (!role.has_value()) {
        return MakeError(ErrorCode::kInternal, "战队成员包含未知角色");
      }
      home.clan = std::get<ClanSummary>(std::move(summary));
      home.role_code = *role;
    } else {
      const auto pending = transaction.Exec(
          "SELECT application.`ClanId`,clan.`ClanName` FROM `ClanApplications` AS application JOIN "
          "`Clans` AS clan ON clan.`ClanId`=application.`ClanId` WHERE "
          "application.`ApplicantPlayerId`=$1 AND application.`ApplicationStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      if (!pending.empty()) {
        home.pending_clan_id = pending[0][0].As<std::uint64_t>();
        home.pending_clan_name = pending[0][1].As<std::string>();
      }
    }
    transaction.Commit();
    return home;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanHomeState>("读取战队首页", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanHomeState>("读取战队首页", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanHomeState>("读取战队首页", error);
  }
}

ServiceResult<std::optional<ClanSummary>> MySqlClanRepository::LoadProfile(
    domain::PlayerId player) {
  auto home = LoadHome(player);
  if (const auto* error = std::get_if<ServiceError>(&home)) {
    return *error;
  }
  return std::get<ClanHomeState>(std::move(home)).clan;
}

ServiceResult<std::vector<ClanMemberView>> MySqlClanRepository::LoadRoster(
    domain::PlayerId player) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto [level_step, maximum_level] = RewardLevelPolicy(transaction);
    const auto rows = transaction.Exec(
        "SELECT "
        "member.`PlayerId`,profile.`Nickname`,progress.`Experience`,member.`RoleCode`,floor(UNIX_TIMESTAMP(member.`JoinedAt`))"
        " FROM `ClanMemberships` AS owner JOIN `ClanMemberships` AS member ON "
        "member.`ClanId`=owner.`ClanId` JOIN `PlayerProfiles` AS profile ON "
        "profile.`PlayerId`=member.`PlayerId` JOIN `PlayerProgressions` AS progress ON "
        "progress.`PlayerId`=member.`PlayerId` WHERE owner.`PlayerId`=$1 ORDER BY CASE "
        "member.`RoleCode` WHEN 'master' THEN 0 WHEN 'member' THEN 1 ELSE 2 END,member.`PlayerId`",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    std::vector<ClanMemberView> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      const auto id = PlayerId(row[0]);
      const auto role = ProtocolRole(row[3].As<std::string>());
      auto level =
          DisplayLevel(row[2].As<std::int64_t>(), level_step, maximum_level);
      if (!id.has_value() || !role.has_value() ||
          std::holds_alternative<ServiceError>(level)) {
        return MakeError(ErrorCode::kInternal, "战队成员资料不满足应用契约");
      }
      result.push_back(
          {.player_id = *id,
           .nickname = row[1].As<std::string>(),
           .level = std::get<std::uint32_t>(level),
           .role_code = *role,
           .joined_at = FromUnixSeconds(row[4].As<std::int64_t>())});
    }
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<std::vector<ClanMemberView>>("读取战队成员",
                                                            error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<std::vector<ClanMemberView>>("读取战队成员",
                                                            error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<ClanMemberView>>("读取战队成员",
                                                            error);
  }
}

ServiceResult<ClanSearchPage> MySqlClanRepository::Search(
    std::uint32_t page, const std::string& query) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto total_rows = transaction.Exec(
        "SELECT count(*) FROM `Clans` AS clan WHERE clan.`ClanKind`='standard' AND "
        "clan.`ClanStatus`<>'disbanded' AND position(lower($1) in lower(clan.`ClanName`))>0",
        bf::infrastructure::mysql::Parameters{transaction, query});
    const auto total = total_rows[0][0].As<std::uint64_t>();
    if (total > std::numeric_limits<std::uint32_t>::max()) {
      return MakeError(ErrorCode::kInternal, "战队搜索总数超出协议范围");
    }
    const auto rows = transaction.Exec(
        "SELECT "
        "clan.`ClanId`,clan.`ClanName`,clan.`LeaderPlayerId`,leader.`Nickname`,clan.`Introduction`,clan.`Notice`,clan.`MarkCode`,clan.`Capacity`,count(member.`PlayerId`),clan.`ClanStatus`='pending_disband',floor(UNIX_TIMESTAMP(clan.`CreatedAt`)),floor(UNIX_TIMESTAMP(disband.`CreatedAt`)),clan.`RecruitmentMode`,coalesce((SELECT"
        " ClanLevel FROM `ClanActivityTotals` WHERE `ClanId`=clan.`ClanId`),1),coalesce((SELECT "
        "LevelPoints FROM `ClanActivityTotals` WHERE `ClanId`=clan.`ClanId`),0) FROM `Clans` AS "
        "clan JOIN `PlayerProfiles` AS leader ON leader.`PlayerId`=clan.`LeaderPlayerId` LEFT JOIN "
        "`ClanMemberships` AS member ON member.`ClanId`=clan.`ClanId` LEFT JOIN "
        "`ClanDisbandRequests` AS disband ON disband.`ClanId`=clan.`ClanId` AND "
        "disband.`RequestStatus`='pending' WHERE clan.`ClanKind`='standard' AND "
        "clan.`ClanStatus`<>'disbanded' AND position(lower($1) in lower(clan.`ClanName`))>0 GROUP "
        "BY clan.`ClanId`,leader.`Nickname`,disband.`CreatedAt` ORDER BY clan.`ClanId` LIMIT 5 "
        "OFFSET $2",
        bf::infrastructure::mysql::Parameters{
            transaction, query, static_cast<std::uint64_t>(page - 1U) * 5U});
    std::vector<ClanSummary> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      auto summary = SummaryFromRow(row);
      if (const auto* error = std::get_if<ServiceError>(&summary)) {
        return *error;
      }
      result.push_back(std::get<ClanSummary>(std::move(summary)));
    }
    transaction.Commit();
    return ClanSearchPage{.entries = std::move(result),
                          .total_count = static_cast<std::uint32_t>(total)};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSearchPage>("搜索战队", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSearchPage>("搜索战队", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSearchPage>("搜索战队", error);
  }
}

ServiceResult<std::vector<ClanApplicantView>>
MySqlClanRepository::LoadApplicants(domain::PlayerId manager) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, manager, false, false);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto [level_step, maximum_level] = RewardLevelPolicy(transaction);
    const auto clan_id =
        std::get<std::pair<std::uint64_t, std::string>>(managed).first;
    const auto rows = transaction.Exec(
        "SELECT "
        "application.`ApplicantPlayerId`,profile.`Nickname`,progress.`Experience`,floor(UNIX_TIMESTAMP(application.`CreatedAt`))"
        " FROM `ClanApplications` AS application JOIN `PlayerProfiles` AS profile ON "
        "profile.`PlayerId`=application.`ApplicantPlayerId` JOIN `PlayerProgressions` AS progress "
        "ON progress.`PlayerId`=profile.`PlayerId` WHERE application.`ClanId`=$1 AND "
        "application.`ApplicationStatus`='pending' ORDER BY "
        "application.`CreatedAt`,application.`ApplicantPlayerId`",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    std::vector<ClanApplicantView> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      const auto id = PlayerId(row[0]);
      auto level =
          DisplayLevel(row[2].As<std::int64_t>(), level_step, maximum_level);
      if (!id.has_value() || std::holds_alternative<ServiceError>(level)) {
        return MakeError(ErrorCode::kInternal, "战队申请资料不满足应用契约");
      }
      result.push_back(
          {.player_id = *id,
           .nickname = row[1].As<std::string>(),
           .level = std::get<std::uint32_t>(level),
           .created_at = FromUnixSeconds(row[3].As<std::int64_t>())});
    }
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<std::vector<ClanApplicantView>>("读取入队申请",
                                                               error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<std::vector<ClanApplicantView>>("读取入队申请",
                                                               error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<ClanApplicantView>>("读取入队申请",
                                                               error);
  }
}

ServiceResult<std::vector<ClanPostView>> MySqlClanRepository::LoadPosts(
    domain::PlayerId player, std::uint32_t kind, std::uint64_t cursor) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto membership = transaction.Exec(
        "SELECT `ClanId` FROM `ClanMemberships` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (membership.empty()) {
      return MakeError(ErrorCode::kInvalidState, "请先加入战队");
    }
    const auto [level_step, maximum_level] = RewardLevelPolicy(transaction);
    const auto rows = transaction.Exec(
        "SELECT "
        "post.`PostId`,post.`AuthorPlayerId`,post.`AuthorSnapshot`,coalesce(progress.`Experience`,0),post.`PostBody`,floor(UNIX_TIMESTAMP(post.`CreatedAt`))"
        " FROM `ClanPosts` AS post LEFT JOIN `PlayerProgressions` AS progress ON "
        "progress.`PlayerId`=post.`AuthorPlayerId` WHERE post.`ClanId`=$1 AND ($2=1 OR ($2=2 AND "
        "post.`PostId`<$3) OR ($2=3 AND post.`PostId`>$3)) ORDER BY post.`PostId` DESC LIMIT 10",
        bf::infrastructure::mysql::Parameters{
            transaction, membership[0][0].As<std::uint64_t>(), kind, cursor});
    std::vector<ClanPostView> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      const auto author = PlayerId(row[1]);
      auto level =
          DisplayLevel(row[3].As<std::int64_t>(), level_step, maximum_level);
      if (!author.has_value() || std::holds_alternative<ServiceError>(level)) {
        return MakeError(ErrorCode::kInternal, "战队留言资料不满足应用契约");
      }
      result.push_back(
          {.post_id = row[0].As<std::uint64_t>(),
           .author_player_id = *author,
           .author_nickname = row[2].As<std::string>(),
           .author_level = std::get<std::uint32_t>(level),
           .body = row[4].As<std::string>(),
           .created_at = FromUnixSeconds(row[5].As<std::int64_t>())});
    }
    if (kind == 3U) {
      std::ranges::reverse(result);
    }
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<std::vector<ClanPostView>>("读取战队留言",
                                                          error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<std::vector<ClanPostView>>("读取战队留言",
                                                          error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<ClanPostView>>("读取战队留言",
                                                          error);
  }
}

ServiceResult<bool> MySqlClanRepository::EnsureStarterCard(
    domain::PlayerId player) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto owner = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (owner.empty()) {
      return MakeError(ErrorCode::kInvalidState, "当前玩家资料不存在");
    }
    if (!transaction
             .Exec("SELECT 1 FROM `ClanStarterGrants` WHERE `PlayerId`=$1",
                   bf::infrastructure::mysql::Parameters{transaction,
                                                         player.Value()})
             .empty()) {
      transaction.Commit();
      return false;
    }
    const auto existing = transaction.Exec(
        "SELECT `ItemInstanceId`,`ItemId` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId` "
        "IN($2,$3) AND `Quantity`>0 AND (`ExpiresAt` IS NULL OR `ExpiresAt`>CURRENT_TIMESTAMP(6)) "
        "ORDER BY `ItemInstanceId` LIMIT 1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{
            transaction, player.Value(), application::kClanCreationCard30,
            application::kClanCreationCard10});
    if (!existing.empty()) {
      transaction.Exec(
          "INSERT INTO `ClanStarterGrants`(`PlayerId`,`ItemId`) VALUES($1,$2)",
          bf::infrastructure::mysql::Parameters{
              transaction, player.Value(), existing[0][1].As<std::uint64_t>()});
      transaction.Commit();
      return false;
    }
    const auto granted = transaction.Exec(
        "INSERT INTO `PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`SourceCode`) "
        "VALUES($1,$2,1,CURRENT_TIMESTAMP(6),'clan_starter') RETURNING `ItemInstanceId`",
        bf::infrastructure::mysql::Parameters{
            transaction, player.Value(), application::kClanCreationCard30});
    const auto ledger = transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,$2,$3,1,'clan_starter',UUID()) RETURNING `InventoryTransactionId`",
        bf::infrastructure::mysql::Parameters{
            transaction, player.Value(), application::kClanCreationCard30,
            granted[0][0].As<std::uint64_t>()});
    transaction.Exec(
        "INSERT INTO `ClanStarterGrants`(`PlayerId`,`ItemId`,`InventoryTransactionId`) "
        "VALUES($1,$2,$3)",
        bf::infrastructure::mysql::Parameters{
            transaction, player.Value(), application::kClanCreationCard30,
            ledger[0][0].As<std::uint64_t>()});
    transaction.Commit();
    return true;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<bool>("发放战队创建卡", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<bool>("发放战队创建卡", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<bool>("发放战队创建卡", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::JoinTraining(
    domain::PlayerId player) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    if (transaction
            .Exec(
                "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
                bf::infrastructure::mysql::Parameters{transaction,
                                                      player.Value()})
            .empty()) {
      return MakeError(ErrorCode::kInvalidState, "当前玩家资料不存在");
    }
    transaction.Exec(
        "INSERT INTO "
        "`Clans`(`ClanId`,`ClanName`,`ClanNameKey`,`LeaderPlayerId`,`ClanKind`,`Introduction`,`MarkCode`,`Capacity`)"
        " VALUES($1,'训练所家族','训练所家族',NULL,'training','新战士的本地训练家族',49,50) ON CONFLICT(`ClanId`) DO "
        "NOTHING",
        bf::infrastructure::mysql::Parameters{transaction,
                                              application::kTrainingClanId});
    auto training =
        LoadSummary(transaction, application::kTrainingClanId, true);
    if (const auto* error = std::get_if<ServiceError>(&training)) {
      return *error;
    }
    const auto existing = transaction.Exec(
        "SELECT membership.`ClanId`,clan.`ClanKind` FROM `ClanMemberships` AS membership JOIN "
        "`Clans` AS clan ON clan.`ClanId`=membership.`ClanId` WHERE membership.`PlayerId`=$1 FOR "
        "UPDATE OF membership",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!existing.empty() && existing[0][1].As<std::string>() != "training") {
      return MakeError(ErrorCode::kConflict, "已经加入其他战队");
    }
    if (existing.empty()) {
      transaction.Exec(
          "UPDATE `ClanApplications` SET "
          "`ApplicationStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
          "`ApplicantPlayerId`=$1 AND `ApplicationStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      transaction.Exec(
          "INSERT INTO `ClanMemberships`(`PlayerId`,`ClanId`,`RoleCode`) VALUES($1,$2,'member')",
          bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                                application::kTrainingClanId});
    }
    training = LoadSummary(transaction, application::kTrainingClanId);
    transaction.Commit();
    return training;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("加入训练营", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("加入训练营", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("加入训练营", error);
  }
}

ServiceResult<std::optional<ClanSummary>> MySqlClanRepository::Leave(
    domain::PlayerId player, bool training_only) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto membership = transaction.Exec(
        "SELECT clan.`ClanId`,clan.`ClanKind`,membership.`RoleCode` FROM `ClanMemberships` AS "
        "membership JOIN `Clans` AS clan ON clan.`ClanId`=membership.`ClanId` WHERE "
        "membership.`PlayerId`=$1 FOR UPDATE OF clan,membership",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (membership.empty()) {
      transaction.Commit();
      return std::optional<ClanSummary>{};
    }
    const auto clan_id = membership[0][0].As<std::uint64_t>();
    const auto kind = membership[0][1].As<std::string>();
    const auto role = membership[0][2].As<std::string>();
    if (training_only && kind != "training") {
      return MakeError(ErrorCode::kInvalidState, "该请求仅用于退出训练营");
    }
    if (kind == "standard" && role == "master") {
      return MakeError(ErrorCode::kConflict, "会长请先转移战队或申请解散");
    }
    auto summary = LoadSummary(transaction, clan_id);
    if (const auto* error = std::get_if<ServiceError>(&summary)) {
      return *error;
    }
    transaction.Exec(
        "DELETE FROM `ClanMemberships` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    transaction.Exec(
        "UPDATE `ClanLeadershipTransfers` SET "
        "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
        "(`SourcePlayerId`=$2 OR `TargetPlayerId`=$2) AND `TransferStatus` IN "
        "('pending','accepted')",
        bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                              player.Value()});
    if (kind == "standard") {
      WriteAudit(transaction, clan_id, player, "leave", player);
    }
    transaction.Commit();
    return std::optional<ClanSummary>{
        std::get<ClanSummary>(std::move(summary))};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<std::optional<ClanSummary>>("退出战队", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<std::optional<ClanSummary>>("退出战队", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::optional<ClanSummary>>("退出战队", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::Create(
    domain::PlayerId player, const std::string& name,
    const std::string& introduction, domain::ItemId card_item_id,
    std::uint32_t capacity) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    if (transaction
            .Exec(
                "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
                bf::infrastructure::mysql::Parameters{transaction,
                                                      player.Value()})
            .empty()) {
      return MakeError(ErrorCode::kInvalidState, "当前玩家资料不存在");
    }
    const auto membership = transaction.Exec(
        "SELECT membership.`ClanId`,clan.`ClanKind` FROM `ClanMemberships` AS membership JOIN "
        "`Clans` AS clan ON clan.`ClanId`=membership.`ClanId` WHERE membership.`PlayerId`=$1 FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!membership.empty() &&
        membership[0][1].As<std::string>() != "training") {
      return MakeError(ErrorCode::kConflict, "已经加入战队");
    }
    const auto card = transaction.Exec(
        "SELECT `ItemInstanceId`,`Quantity` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 "
        "AND `Quantity`>0 AND (`ExpiresAt` IS NULL OR `ExpiresAt`>CURRENT_TIMESTAMP(6)) ORDER BY "
        "`ItemInstanceId` LIMIT 1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              card_item_id.Value()});
    if (card.empty()) {
      return MakeError(ErrorCode::kConflict, "缺少对应的战队创建卡");
    }
    const auto item_instance_id = card[0][0].As<std::uint64_t>();
    const auto quantity = card[0][1].As<std::int64_t>();
    if (quantity == 1) {
      transaction.Exec(
          "DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, item_instance_id});
      transaction.Exec(
          "DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, item_instance_id});
    } else {
      transaction.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, item_instance_id});
    }
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,$2,NULL,-1,'clan_create_consume',UUID())",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              card_item_id.Value()});
    const auto clan = transaction.Exec(
        "INSERT INTO "
        "`Clans`(`ClanName`,`ClanNameKey`,`LeaderPlayerId`,`ClanKind`,`Introduction`,`Capacity`) "
        "VALUES($1,$2,$3,'standard',$4,$5) RETURNING `ClanId`",
        bf::infrastructure::mysql::Parameters{transaction, name,
                                              ClanNameKey(name), player.Value(),
                                              introduction, capacity});
    const auto clan_id = clan[0][0].As<std::uint64_t>();
    if (!membership.empty()) {
      transaction.Exec(
          "DELETE FROM `ClanMemberships` WHERE `PlayerId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    }
    transaction.Exec(
        "UPDATE `ClanApplications` SET "
        "`ApplicationStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
        "`ApplicantPlayerId`=$1 AND `ApplicationStatus`='pending'",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    transaction.Exec(
        "INSERT INTO `ClanMemberships`(`PlayerId`,`ClanId`,`RoleCode`) VALUES($1,$2,'master')",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              clan_id});
    WriteAudit(transaction, clan_id, player, "create", player, name);
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(ErrorCode::kConflict, "战队名称已被使用");
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("创建战队", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("创建战队", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("创建战队", error);
  }
}

ServiceResult<ClanHomeState> MySqlClanRepository::Apply(
    domain::PlayerId player, std::uint64_t clan_id,
    const std::string& clan_name) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto clan = transaction.Exec(
        "SELECT `ClanName`,`Capacity`,`ClanStatus`,`RecruitmentMode` FROM `Clans` WHERE `ClanId`=$1"
        " AND `ClanKind`='standard' FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    if (clan.empty() || clan[0][0].As<std::string>() != clan_name) {
      return MakeError(ErrorCode::kInvalidInput, "战队不存在，请刷新列表");
    }
    if (clan[0][2].As<std::string>() != "active") {
      return MakeError(ErrorCode::kConflict, "该战队正在等待解散");
    }
    if (clan[0][3].As<std::uint32_t>() == 0U) {
      return MakeError(ErrorCode::kConflict, "该战队已停止招募");
    }
    // 必须在父行锁取得后重新查询人数；同一个 SELECT
    // 的子查询可能在等待锁前执行。
    const auto members = transaction.Exec(
        "SELECT count(*) FROM `ClanMemberships` WHERE `ClanId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    if (members[0][0].As<std::uint64_t>() >= clan[0][1].As<std::uint64_t>()) {
      return MakeError(ErrorCode::kConflict, "战队人数已满");
    }
    // 即使尚无成员行，也通过玩家行串行化两个不同战队的并发加入请求。
    transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    const auto membership = transaction.Exec(
        "SELECT membership.`ClanId`,clan.`ClanKind` FROM `ClanMemberships` AS membership JOIN "
        "`Clans` AS clan ON clan.`ClanId`=membership.`ClanId` WHERE membership.`PlayerId`=$1 FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!membership.empty() &&
        membership[0][1].As<std::string>() != "training") {
      return MakeError(ErrorCode::kConflict, "请先退出当前战队");
    }
    const auto pending = transaction.Exec(
        "SELECT `ClanId` FROM `ClanApplications` WHERE `ApplicantPlayerId`=$1 AND "
        "`ApplicationStatus`='pending' FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!pending.empty() && pending[0][0].As<std::uint64_t>() != clan_id) {
      return MakeError(ErrorCode::kConflict, "请先取消已有的入队申请");
    }
    if (clan[0][3].As<std::uint32_t>() == 2U) {
      if (!membership.empty()) {
        transaction.Exec(
            "DELETE FROM `ClanMemberships` WHERE `PlayerId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      }
      transaction.Exec(
          "INSERT INTO `ClanMemberships`(`ClanId`,`PlayerId`,`RoleCode`) VALUES($1,$2,'member')",
          bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                                player.Value()});
      transaction.Exec(
          "UPDATE `ClanApplications` SET "
          "`ApplicationStatus`='accepted',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
          "`ApplicantPlayerId`=$1 AND `ClanId`=$2 AND `ApplicationStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                                clan_id});
      WriteAudit(transaction, clan_id, player, "direct_join", player);
      auto summary = LoadSummary(transaction, clan_id);
      if (const auto* error = std::get_if<ServiceError>(&summary)) {
        return *error;
      }
      transaction.Commit();
      return ClanHomeState{.clan = std::get<ClanSummary>(std::move(summary)),
                           .role_code = application::kClanMemberRole};
    }
    if (pending.empty()) {
      if (!membership.empty()) {
        transaction.Exec(
            "DELETE FROM `ClanMemberships` WHERE `PlayerId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      }
      transaction.Exec(
          "INSERT INTO `ClanApplications`(`ApplicantPlayerId`,`ClanId`) VALUES($1,$2)",
          bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                                clan_id});
      WriteAudit(transaction, clan_id, player, "apply", player);
    }
    transaction.Commit();
    ClanHomeState result;
    result.pending_clan_id = clan_id;
    result.pending_clan_name = clan_name;
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanHomeState>("申请加入战队", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanHomeState>("申请加入战队", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanHomeState>("申请加入战队", error);
  }
}

ServiceResult<ClanHomeState> MySqlClanRepository::CancelApplication(
    domain::PlayerId player, std::uint64_t clan_id) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto pending = transaction.Exec(
        "SELECT application.`ClanId` FROM `ClanApplications` AS application JOIN `Clans` AS clan ON"
        " clan.`ClanId`=application.`ClanId` WHERE application.`ApplicantPlayerId`=$1 AND "
        "application.`ApplicationStatus`='pending' FOR UPDATE OF clan,application",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (!pending.empty() && pending[0][0].As<std::uint64_t>() != clan_id) {
      return MakeError(ErrorCode::kConflict, "入队申请已改变，请刷新");
    }
    if (!pending.empty()) {
      transaction.Exec(
          "UPDATE `ClanApplications` SET "
          "`ApplicationStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
          "`ApplicantPlayerId`=$1 AND `ApplicationStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      WriteAudit(transaction, clan_id, player, "cancel_apply", player);
    }
    transaction.Commit();
    return ClanHomeState{};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanHomeState>("取消入队申请", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanHomeState>("取消入队申请", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanHomeState>("取消入队申请", error);
  }
}

ServiceResult<ClanMemberChangeResult> MySqlClanRepository::ChangeMember(
    domain::PlayerId manager, domain::PlayerId target,
    const std::string& target_nickname, std::int32_t old_role,
    std::int32_t new_role) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, manager, false, true);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto [clan_id, manager_role] =
        std::get<std::pair<std::uint64_t, std::string>>(managed);
    const auto profile = transaction.Exec(
        "SELECT `Nickname` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, target.Value()});
    if (profile.empty() || profile[0][0].As<std::string>() != target_nickname) {
      return MakeError(ErrorCode::kConflict, "成员身份已改变，请刷新");
    }
    const auto application = transaction.Exec(
        "SELECT `ClanId` FROM `ClanApplications` WHERE `ApplicantPlayerId`=$1 AND "
        "`ApplicationStatus`='pending' FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, target.Value()});
    const bool joining = !application.empty() &&
                         application[0][0].As<std::uint64_t>() == clan_id;
    const auto membership = transaction.Exec(
        "SELECT `ClanId`,`RoleCode` FROM `ClanMemberships` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, target.Value()});
    if (joining) {
      if (old_role !=
              static_cast<std::int32_t>(application::kClanApplicantRole) ||
          (new_role !=
               static_cast<std::int32_t>(application::kClanMemberRole) &&
           new_role != -10)) {
        return MakeError(ErrorCode::kInvalidInput, "入队审批参数不正确");
      }
      if (new_role == static_cast<std::int32_t>(application::kClanMemberRole)) {
        const auto capacity = transaction.Exec(
            "SELECT `Capacity`,(SELECT count(*) FROM `ClanMemberships` WHERE `ClanId`=$1),`ClanStatus` "
            "FROM `Clans` WHERE `ClanId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, clan_id});
        if (!membership.empty() || capacity[0][1].As<std::uint64_t>() >=
                                       capacity[0][0].As<std::uint64_t>()) {
          return MakeError(ErrorCode::kConflict,
                           "玩家已加入战队或战队人数已满");
        }
        if (capacity[0][2].As<std::string>() != "active") {
          return MakeError(ErrorCode::kConflict, "该战队正在等待解散");
        }
        transaction.Exec(
            "INSERT INTO `ClanMemberships`(`PlayerId`,`ClanId`,`RoleCode`) VALUES($1,$2,'member')",
            bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                                  clan_id});
        transaction.Exec(
            "UPDATE `ClanApplications` SET "
            "`ApplicationStatus`='accepted',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
            "`ApplicantPlayerId`=$1 AND `ApplicationStatus`='pending'",
            bf::infrastructure::mysql::Parameters{transaction, target.Value()});
      } else {
        transaction.Exec(
            "UPDATE `ClanApplications` SET "
            "`ApplicationStatus`='rejected',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE "
            "`ApplicantPlayerId`=$1 AND `ApplicationStatus`='pending'",
            bf::infrastructure::mysql::Parameters{transaction, target.Value()});
      }
    } else {
      if (membership.empty() ||
          membership[0][0].As<std::uint64_t>() != clan_id ||
          target == manager || membership[0][1].As<std::string>() == "master") {
        return MakeError(ErrorCode::kPermissionDenied, "不能管理该成员");
      }
      const auto current_role =
          ProtocolRole(membership[0][1].As<std::string>());
      const auto next_role = StoredRole(new_role);
      if (!current_role.has_value() ||
          *current_role != static_cast<std::uint32_t>(old_role) ||
          (new_role != -30 && !next_role.has_value())) {
        return MakeError(ErrorCode::kConflict,
                         "成员职位已经改变或动作非法，请刷新");
      }
      if (manager_role != "master" &&
          (new_role != -30 || membership[0][1].As<std::string>() != "member")) {
        return MakeError(ErrorCode::kPermissionDenied,
                         "该成员操作需要会长权限");
      }
      if (new_role == static_cast<std::int32_t>(application::kClanDeputyRole) &&
          !transaction
               .Exec(
                   "SELECT 1 FROM `ClanMemberships` WHERE `ClanId`=$1 AND `RoleCode`='deputy' AND "
                   "`PlayerId`<>$2",
                   bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                                         target.Value()})
               .empty()) {
        return MakeError(ErrorCode::kConflict, "战队只能任命一位副会长");
      }
      if (new_role == -30) {
        transaction.Exec(
            "DELETE FROM `ClanMemberships` WHERE `PlayerId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, target.Value()});
      } else {
        transaction.Exec(
            "UPDATE `ClanMemberships` SET `RoleCode`=$2 WHERE `PlayerId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                                  *next_role});
      }
      transaction.Exec(
          "UPDATE `ClanLeadershipTransfers` SET "
          "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
          "`TransferStatus` IN ('pending','accepted')",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
    }
    WriteAudit(transaction, clan_id, manager, "member_change", target,
               std::to_string(new_role));
    auto summary = LoadSummary(transaction, clan_id);
    if (const auto* error = std::get_if<ServiceError>(&summary)) {
      return *error;
    }
    transaction.Commit();
    return ClanMemberChangeResult{
        .clan = std::get<ClanSummary>(std::move(summary)),
        .target_player_id = target,
        .target_nickname = target_nickname,
        .new_role = new_role,
        .joined_from_application = joining};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(ErrorCode::kConflict, "成员归属或副会长职位发生并发冲突");
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("变更战队成员", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("变更战队成员", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("变更战队成员", error);
  }
}

ServiceResult<application::ClanActivityView> MySqlClanRepository::Activity(
    domain::PlayerId player) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto rows = tx.Exec(
        "WITH period AS (SELECT "
        "CONVERT_TZ(DATE_SUB(DATE(CONVERT_TZ(CURRENT_TIMESTAMP(6),'+00:00','+08:00')),INTERVAL "
        "WEEKDAY(DATE(CONVERT_TZ(CURRENT_TIMESTAMP(6),'+00:00','+08:00'))) DAY),'+08:00','+00:00') "
        "beginning) SELECT "
        "c.`ClanId`,floor(UNIX_TIMESTAMP(p.beginning)),floor(UNIX_TIMESTAMP(p.beginning+INTERVAL 7 "
        "DAY-INTERVAL 1 SECOND)),least(2147483647,coalesce(sum(CASE WHEN g.`EarnedAt`>=p.beginning "
        "AND g.`EarnedAt`<p.beginning+INTERVAL 7 DAY THEN g.`Points` "
        "END),0)),least(2147483647,coalesce(sum(CASE WHEN g.`PlayerId`=$1 THEN g.`Points` "
        "END),0)),coalesce(t.ClanLevel,1) FROM `ClanMemberships` cm JOIN `Clans` c USING(`ClanId`) "
        "CROSS JOIN period p LEFT JOIN `ClanActivityGrants` g ON g.`ClanId`=c.`ClanId` LEFT JOIN "
        "`ClanActivityTotals` t ON t.`ClanId`=c.`ClanId` WHERE cm.`PlayerId`=$1 AND "
        "c.`ClanKind`='standard' AND c.`ClanStatus` IN('active','pending_disband') GROUP BY "
        "c.`ClanId`,p.beginning,t.ClanLevel",
        bf::infrastructure::mysql::Parameters{tx, player.Value()});
    if (rows.empty())
      return MakeError(ErrorCode::kPermissionDenied,
                       "只有普通战队成员可以查询活跃度");
    const auto& row = rows[0];
    return application::ClanActivityView{
        .clan_id = row[0].As<std::uint64_t>(),
        .period_start = FromUnixSeconds(row[1].As<std::int64_t>()),
        .period_end = FromUnixSeconds(row[2].As<std::int64_t>()),
        .weekly_points = row[3].As<std::uint32_t>(),
        .personal_points = row[4].As<std::uint32_t>(),
        .level = row[5].As<std::uint32_t>()};
  } catch (const std::exception& error) {
    return CaughtDatabaseError<application::ClanActivityView>("查询战队活跃度",
                                                              error);
  }
}

ServiceResult<std::vector<application::ClanRankingEntry>>
MySqlClanRepository::Rankings(domain::PlayerId player,
                              const application::ClanRankingQuery& query) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    if (query.clan_id &&
        tx.Exec(
              "SELECT 1 FROM `ClanMemberships` cm JOIN `Clans` c USING(`ClanId`) WHERE cm.`PlayerId`=$1 "
              "AND cm.`ClanId`=$2 AND c.`ClanKind`='standard' AND c.`ClanStatus` "
              "IN('active','pending_disband')",
              bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                    *query.clan_id})
            .empty())
      return MakeError(ErrorCode::kPermissionDenied,
                       "单队排名只能查询本人当前普通战队");
    std::optional<std::string> month;
    if (query.month) {
      const std::chrono::year_month_day date{*query.month};
      if (date.day() != std::chrono::day{1} ||
          date.year() < std::chrono::year{2000} ||
          date.year() > std::chrono::year{2100})
        return MakeError(ErrorCode::kInvalidInput, "排行榜月份无效");
      month = std::to_string(static_cast<int>(date.year())) + "-" +
              std::to_string(static_cast<unsigned>(date.month())) + "-01";
    }
    // 先计算完整排名，再应用前100名/本队过滤，避免把榜外本队错误编号为第1名。
    const auto rows = tx.Exec(
        "WITH games AS (SELECT "
        "cs.`ClanId`,cs.`MatchId`,min(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome'))) outcome "
        "FROM `ClanMatchSides` cs JOIN `Matches` m USING(`MatchId`) JOIN `MatchSettlements` s "
        "USING(`MatchId`) CROSS JOIN "
        "JSON_TABLE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.participants'), '$[*]' COLUMNS (Element "
        "JSON PATH '$')) p WHERE s.`SettlementStatus`='succeeded' AND "
        "JSON_UNQUOTE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.completion_kind'))='completed' AND "
        "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.team_code')))=cs.`TeamCode` AND ($1 IS NULL OR "
        "(m.`EndedAt`>=CONVERT_TZ(CAST($1 AS DATE),'+08:00','+00:00') AND "
        "m.`EndedAt`<CONVERT_TZ(DATE_ADD(CAST($1 AS DATE),INTERVAL 1 MONTH),'+08:00','+00:00'))) "
        "GROUP BY cs.`ClanId`,cs.`MatchId` HAVING count(DISTINCT "
        "JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome')))=1), counts AS (SELECT "
        "`ClanId`,COUNT(CASE WHEN outcome='win' THEN 1 END) `Wins`,COUNT(CASE WHEN outcome='draw' "
        "THEN 1 END) `Draws`,COUNT(CASE WHEN outcome='loss' THEN 1 END) `Losses` FROM games JOIN "
        "`Clans` USING(`ClanId`) WHERE `ClanStatus` IN('active','pending_disband') GROUP BY "
        "`ClanId`), ranking AS (SELECT *,row_number() OVER(ORDER BY `Wins` DESC,`Draws` "
        "DESC,`Losses`,`ClanId`) position FROM counts) SELECT "
        "c.`ClanId`,c.`ClanName`,coalesce(r.position,0),coalesce(t.ClanLevel,1),coalesce(r.`Wins`,0),coalesce(r.`Draws`,0),coalesce(r.`Losses`,0)"
        " FROM `Clans` c LEFT JOIN ranking r USING(`ClanId`) LEFT JOIN `ClanActivityTotals` t "
        "USING(`ClanId`) WHERE ($2 IS NULL AND r.position<=100) OR c.`ClanId`=$2 ORDER BY "
        "(r.position IS NULL), r.position,c.`ClanId`",
        bf::infrastructure::mysql::Parameters{tx, month, query.clan_id});
    std::vector<application::ClanRankingEntry> result;
    result.reserve(rows.size());
    for (const auto& row : rows)
      result.push_back(
          {.clan_id = row[0].As<std::uint32_t>(),
           .name = row[1].As<std::string>(),
           .rank = row[2].As<std::uint32_t>(),
           .level = row[3].As<std::uint32_t>(),
           .results = {row[4].As<std::uint32_t>(), row[5].As<std::uint32_t>(),
                       row[6].As<std::uint32_t>()}});
    return result;
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<application::ClanRankingEntry>>(
        "查询战队排行榜", error);
  }
}

ServiceResult<std::vector<application::ClanRecentMatch>>
MySqlClanRepository::RecentMatches(domain::PlayerId player,
                                   std::uint64_t clan) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto authorized = tx.Exec(
        "SELECT 1 FROM `ClanMemberships` cm JOIN `Clans` c USING(`ClanId`) WHERE cm.`PlayerId`=$1 "
        "AND c.`ClanId`=$2 AND c.`ClanKind`='standard' AND c.`ClanStatus` "
        "IN('active','pending_disband')",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), clan});
    if (authorized.empty())
      return MakeError(ErrorCode::kPermissionDenied,
                       "只能查询本人当前普通战队的最近比赛");
    const auto rows = tx.Exec(
        "SELECT "
        "other.`ClanId`,c.`MarkCode`,c.`ClanName`,r.outcome,floor(UNIX_TIMESTAMP(m.`EndedAt`)),own.`TeamCode`,n.names"
        " FROM `ClanMatchSides` own JOIN `Matches` m USING(`MatchId`) JOIN `MatchSettlements` s "
        "USING(`MatchId`) JOIN `ClanMatchSides` other ON other.`MatchId`=own.`MatchId` AND "
        "other.`TeamCode`<>own.`TeamCode` JOIN `Clans` c ON c.`ClanId`=other.`ClanId` JOIN LATERAL "
        "(SELECT MIN(p.Outcome) outcome FROM JSON_TABLE(s.TerminalSnapshot,'$.participants[*]' "
        "COLUMNS (Outcome VARCHAR(8) PATH '$.outcome',TeamCode INT PATH '$.team_code')) p WHERE "
        "p.TeamCode=own.TeamCode HAVING COUNT(DISTINCT p.Outcome)=1) r ON TRUE JOIN LATERAL (SELECT"
        " CONCAT('[',GROUP_CONCAT(JSON_OBJECT('team',p.TeamCode,'name',COALESCE(pp.Nickname,'')) "
        "ORDER BY p.TeamCode,p.SeatIndex SEPARATOR ','),']') names FROM "
        "JSON_TABLE(s.TerminalSnapshot,'$.participants[*]' COLUMNS (PlayerId BIGINT PATH "
        "'$.player_id',TeamCode INT PATH '$.team_code',SeatIndex INT PATH '$.seat_index')) p LEFT "
        "JOIN PlayerProfiles pp ON pp.PlayerId=p.PlayerId) n ON true WHERE own.`ClanId`=$1 AND "
        "s.`SettlementStatus`='succeeded' AND "
        "JSON_UNQUOTE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.completion_kind'))='completed' ORDER BY "
        "m.`EndedAt` DESC,m.`MatchId` DESC LIMIT 50",
        bf::infrastructure::mysql::Parameters{tx, clan});
    std::vector<application::ClanRecentMatch> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      const auto outcome = row[3].As<std::string>();
      if (outcome != "win" && outcome != "draw" && outcome != "loss")
        return MakeError(ErrorCode::kInternal, "最近比赛的赛果无效");
      application::ClanRecentMatch entry{
          .opponent_id = row[0].As<std::uint32_t>(),
          .opponent_mark = row[1].As<std::uint32_t>(),
          .opponent_name = row[2].As<std::string>(),
          .result = {outcome == "win" ? 1U : 0U, outcome == "draw" ? 1U : 0U,
                     outcome == "loss" ? 1U : 0U},
          .ended_at = FromUnixSeconds(row[4].As<std::int64_t>())};
      const auto own_team = row[5].As<std::uint32_t>();
      std::array<std::size_t, 2U> positions{};
      for (const auto& participant :
           nlohmann::json::parse(row[6].As<std::string>())) {
        const auto team = participant.at("team").get<std::uint32_t>();
        if (team != 1U && team != 2U)
          return MakeError(ErrorCode::kInternal, "最近比赛的队伍无效");
        const auto side = team == own_team ? 0U : 1U;
        if (positions[side] < 6U)
          entry.participant_names[side * 6U + positions[side]++] =
              participant.at("name").get<std::string>();
      }
      result.push_back(std::move(entry));
    }
    return result;
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<application::ClanRecentMatch>>(
        "查询战队最近比赛", error);
  }
}

ServiceResult<std::vector<application::ClanRecordEntry>>
MySqlClanRepository::Records(domain::PlayerId player,
                             const application::ClanRecordsQuery& query) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto authorized = tx.Exec(
        "SELECT 1 FROM `ClanMemberships` cm JOIN `Clans` c USING(`ClanId`) WHERE cm.`PlayerId`=$1 "
        "AND c.`ClanId`=$2 AND c.`ClanKind`='standard' AND c.`ClanStatus` "
        "IN('active','pending_disband')",
        bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                              query.clan_id});
    if (authorized.empty())
      return MakeError(ErrorCode::kPermissionDenied,
                       "只能查询本人当前普通战队的战绩");
    const std::chrono::year_month_day date{query.month};
    if (date.day() != std::chrono::day{1} ||
        date.year() < std::chrono::year{2000} ||
        date.year() > std::chrono::year{2100})
      return MakeError(ErrorCode::kInvalidInput, "战绩月份无效");
    const auto month = std::to_string(static_cast<int>(date.year())) + "-" +
                       std::to_string(static_cast<unsigned>(date.month())) +
                       "-01";
    // 归属取开局快照；只有成功完成结算的比赛进入统计，重放不会新增比赛行。
    const std::string games =
        "WITH games AS (SELECT "
        "cs.`MatchId`,cs.`TeamCode`,m.`EndedAt`,s.`TerminalSnapshot`,(m.`EndedAt` >= "
        "CONVERT_TZ(CAST($2 AS DATE),'+08:00','+00:00') AND m.`EndedAt` < "
        "CONVERT_TZ(DATE_ADD(CAST($2 AS DATE),INTERVAL 1 MONTH),'+08:00','+00:00')) selected FROM "
        "`ClanMatchSides` cs JOIN `Matches` m USING(`MatchId`) JOIN `MatchSettlements` s "
        "USING(`MatchId`) WHERE cs.`ClanId`=$1 AND s.`SettlementStatus`='succeeded' AND "
        "JSON_UNQUOTE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.completion_kind'))='completed'), ";
    const std::string counts =
        "COUNT(CASE WHEN selected AND outcome='win' THEN 1 END),COUNT(CASE WHEN selected AND "
        "outcome='draw' THEN 1 END),COUNT(CASE WHEN selected AND outcome='loss' THEN 1 "
        "END),COUNT(CASE WHEN outcome='win' THEN 1 END),COUNT(CASE WHEN outcome='draw' THEN 1 "
        "END),COUNT(CASE WHEN outcome='loss' THEN 1 END) ";
    std::string sql;
    if (query.opponents) {
      sql =
          games +
          "results AS (SELECT "
          "g.`MatchId`,g.selected,min(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome'))) outcome FROM"
          " games g CROSS JOIN JSON_TABLE(JSON_EXTRACT(g.`TerminalSnapshot`,'$.participants'), '$[*]'"
          " COLUMNS (Element JSON PATH '$')) p WHERE "
          "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.team_code')))=g.`TeamCode` GROUP BY "
          "g.`MatchId`,g.selected HAVING count(DISTINCT "
          "JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome')))=1), rivals AS (SELECT "
          "cs.`ClanId`,r.selected,r.outcome FROM results r JOIN `ClanMatchSides` cs USING(`MatchId`) "
          "WHERE cs.`ClanId`<>$1) SELECT c.`ClanId`,c.`ClanName`,c.`MarkCode`," +
          counts +
          "FROM rivals r JOIN `Clans` c USING(`ClanId`) GROUP BY c.`ClanId`,c.`ClanName`,c.`MarkCode`"
          " HAVING MAX(selected)=1 ORDER BY COUNT(CASE WHEN selected THEN 1 END) DESC,c.`ClanId` "
          "LIMIT 513";
    } else {
      sql =
          games +
          "results AS (SELECT (JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.player_id'))) "
          "`PlayerId`,g.selected,JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome')) outcome FROM games"
          " g CROSS JOIN JSON_TABLE(JSON_EXTRACT(g.`TerminalSnapshot`,'$.participants'), '$[*]' "
          "COLUMNS (Element JSON PATH '$')) p WHERE "
          "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.team_code')))=g.`TeamCode`) SELECT "
          "cm.`PlayerId`,pp.`Nickname`,pg.`Experience`," +
          counts +
          "FROM `ClanMemberships` cm JOIN `PlayerProfiles` pp USING(`PlayerId`) JOIN "
          "`PlayerProgressions` pg USING(`PlayerId`) LEFT JOIN results r USING(`PlayerId`) WHERE "
          "cm.`ClanId`=$1 GROUP BY cm.`PlayerId`,pp.`Nickname`,pg.`Experience` ORDER BY COUNT(CASE "
          "WHEN selected AND outcome='win' THEN 1 END) DESC,COUNT(CASE WHEN selected AND "
          "outcome='loss' THEN 1 END),cm.`PlayerId` LIMIT 513";
    }
    const auto rows = tx.Exec(
        sql, bf::infrastructure::mysql::Parameters{tx, query.clan_id, month});
    if (rows.size() > 512U)
      return MakeError(ErrorCode::kInvalidState, "战绩条目超过原生单页容量");
    const auto [step, maximum] = RewardLevelPolicy(tx);
    std::vector<application::ClanRecordEntry> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      auto code = query.opponents
                      ? ServiceResult<std::uint32_t>{row[2].As<std::uint32_t>()}
                      : DisplayLevel(row[2].As<std::int64_t>(), step, maximum);
      if (const auto* error = std::get_if<ServiceError>(&code)) return *error;
      result.push_back(
          {.id = row[0].As<std::uint32_t>(),
           .name = row[1].As<std::string>(),
           .display_code = std::get<std::uint32_t>(code),
           .monthly = {row[3].As<std::uint32_t>(), row[4].As<std::uint32_t>(),
                       row[5].As<std::uint32_t>()},
           .lifetime = {row[6].As<std::uint32_t>(), row[7].As<std::uint32_t>(),
                        row[8].As<std::uint32_t>()}});
    }
    return result;
  } catch (const std::exception& error) {
    return CaughtDatabaseError<std::vector<application::ClanRecordEntry>>(
        "查询战队战绩", error);
  }
}

ServiceResult<application::ClanTodayResults> MySqlClanRepository::TodayResults(
    domain::PlayerId player, std::uint64_t clan) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto rows = tx.Exec(
        "WITH authorized AS (SELECT c.`ClanId` FROM `ClanMemberships` cm JOIN `Clans` c "
        "USING(`ClanId`) WHERE cm.`PlayerId`=$1 AND c.`ClanId`=$2 AND c.`ClanId`<>99 AND "
        "c.`ClanStatus` IN ('active','pending_disband')), games AS (SELECT "
        "cs.`MatchId`,min(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome'))) outcome FROM "
        "`ClanMatchSides` cs JOIN `Matches` m USING(`MatchId`) JOIN `MatchSettlements` s "
        "USING(`MatchId`) CROSS JOIN "
        "JSON_TABLE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.participants'), '$[*]' COLUMNS (Element "
        "JSON PATH '$')) p WHERE cs.`ClanId` IN (SELECT `ClanId` FROM authorized) AND "
        "s.`SettlementStatus`='succeeded' AND "
        "JSON_UNQUOTE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.completion_kind'))='completed' AND "
        "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.team_code')))=cs.`TeamCode` AND m.`EndedAt` >= "
        "(CONVERT_TZ(DATE(CONVERT_TZ(CURRENT_TIMESTAMP(6),'+00:00','+08:00')),'+08:00','+00:00')) "
        "AND m.`EndedAt` < "
        "DATE_ADD(CONVERT_TZ(DATE(CONVERT_TZ(CURRENT_TIMESTAMP(6),'+00:00','+08:00')),'+08:00','+00:00'),INTERVAL"
        " 1 DAY) GROUP BY cs.`MatchId` HAVING count(DISTINCT "
        "JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome')))=1) SELECT (SELECT count(*) FROM "
        "authorized),COUNT(CASE WHEN outcome='win' THEN 1 END),COUNT(CASE WHEN outcome='draw' THEN "
        "1 END),COUNT(CASE WHEN outcome='loss' THEN 1 END) FROM games",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), clan});
    if (rows[0][0].As<int>() != 1)
      return MakeError(ErrorCode::kPermissionDenied,
                       "只能查询本人当前普通战队的比赛");
    return application::ClanTodayResults{rows[0][1].As<std::uint32_t>(),
                                         rows[0][2].As<std::uint32_t>(),
                                         rows[0][3].As<std::uint32_t>()};
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("战队当日赛果查询失败，sqlstate={}", error.Sqlstate());
  } catch (const std::exception&) {
  }
  return StorageError("战队当日赛果暂时不可用");
}

ServiceResult<ClanSummary> MySqlClanRepository::SetRecruitment(
    domain::PlayerId manager, std::uint64_t clan_id, std::uint32_t mode) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto parent = transaction.Exec(
        "SELECT `RecruitmentMode` FROM `Clans` WHERE `ClanId`=$1 AND `ClanKind`='standard' AND "
        "`ClanStatus`='active' FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    if (parent.empty() || mode > 2U) {
      return MakeError(ErrorCode::kInvalidInput,
                       "战队不存在、正在解散或招募模式无效");
    }
    // 先取得父行锁再读取角色，避免与会长转让/降职并发时使用旧权限。
    auto managed = ManagerClan(transaction, manager, false, false);
    const auto* authorized =
        std::get_if<std::pair<std::uint64_t, std::string>>(&managed);
    if (!authorized || authorized->first != clan_id) {
      return MakeError(ErrorCode::kPermissionDenied,
                       "只有本战队管理者可以修改招募模式");
    }
    if (parent[0][0].As<std::uint32_t>() != mode) {
      transaction.Exec(
          "UPDATE `Clans` SET `RecruitmentMode`=$2 WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id, mode});
      WriteAudit(transaction, clan_id, manager, "recruitment", std::nullopt,
                 std::to_string(mode));
    }
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("修改战队招募模式", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::EditText(
    domain::PlayerId manager, bool introduction, const std::string& value) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, manager, false, true);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto clan_id =
        std::get<std::pair<std::uint64_t, std::string>>(managed).first;
    if (introduction) {
      transaction.Exec(
          "UPDATE `Clans` SET `Introduction`=$2 WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id, value});
      WriteAudit(transaction, clan_id, manager, "introduction", std::nullopt,
                 value);
    } else {
      transaction.Exec(
          "UPDATE `Clans` SET `Notice`=$2 WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id, value});
      WriteAudit(transaction, clan_id, manager, "notice", std::nullopt, value);
    }
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("编辑战队文本", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("编辑战队文本", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("编辑战队文本", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::ScheduleDisband(
    domain::PlayerId master, bool schedule) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, master, true, true);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto clan_id =
        std::get<std::pair<std::uint64_t, std::string>>(managed).first;
    if (schedule) {
      transaction.Exec(
          "INSERT INTO `ClanDisbandRequests`(`ClanId`,`RequestedByPlayerId`,`ExecuteAfter`) "
          "VALUES($1,$2,CURRENT_TIMESTAMP(6)) ON CONFLICT(`ClanId`) WHERE `RequestStatus`='pending' "
          "DO NOTHING",
          bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                                master.Value()});
      transaction.Exec(
          "UPDATE `Clans` SET `ClanStatus`='pending_disband' WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "UPDATE `ClanLeadershipTransfers` SET "
          "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
          "`TransferStatus` IN ('pending','accepted')",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
    } else {
      transaction.Exec(
          "UPDATE `ClanDisbandRequests` SET "
          "`RequestStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
          "`RequestStatus`='pending'",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
      transaction.Exec(
          "UPDATE `Clans` SET `ClanStatus`='active' WHERE `ClanId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, clan_id});
    }
    WriteAudit(transaction, clan_id, master,
               schedule ? "schedule_disband" : "cancel_disband");
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("变更战队解散计划", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("变更战队解散计划", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("变更战队解散计划", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::RequestTransfer(
    domain::PlayerId master, domain::PlayerId target,
    std::chrono::system_clock::time_point expires_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, master, true, true);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto clan_id =
        std::get<std::pair<std::uint64_t, std::string>>(managed).first;
    const auto member = transaction.Exec(
        "SELECT `RoleCode` FROM `ClanMemberships` WHERE `PlayerId`=$1 AND `ClanId`=$2 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                              clan_id});
    if (member.empty() || member[0][0].As<std::string>() == "master") {
      return MakeError(ErrorCode::kInvalidInput, "请选择本战队的非会长成员");
    }
    transaction.Exec(
        "UPDATE `ClanLeadershipTransfers` SET "
        "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
        "`TransferStatus` IN ('pending','accepted')",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    transaction.Exec(
        "INSERT INTO "
        "`ClanLeadershipTransfers`(`ClanId`,`SourcePlayerId`,`TargetPlayerId`,`ExpiresAt`) "
        "VALUES($1,$2,$3,FROM_UNIXTIME($4))",
        bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                              master.Value(), target.Value(),
                                              UnixSeconds(expires_at)});
    WriteAudit(transaction, clan_id, master, "transfer_request", target);
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("申请会长转让", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("申请会长转让", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("申请会长转让", error);
  }
}

ServiceResult<ClanSummary> MySqlClanRepository::RespondTransfer(
    domain::PlayerId target, domain::PlayerId source, bool accepted) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto request = transaction.Exec(
        "SELECT transfer.`ClanId`,clan.`LeaderPlayerId` FROM `ClanLeadershipTransfers` AS transfer "
        "JOIN `Clans` AS clan ON clan.`ClanId`=transfer.`ClanId` JOIN `ClanMemberships` AS member "
        "ON member.`PlayerId`=$1 AND member.`ClanId`=transfer.`ClanId` WHERE "
        "transfer.`TargetPlayerId`=$1 AND transfer.`SourcePlayerId`=$2 AND "
        "transfer.`TransferStatus`='pending' AND transfer.`ExpiresAt`>CURRENT_TIMESTAMP(6) FOR "
        "UPDATE OF clan,transfer,member",
        bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                              source.Value()});
    if (request.empty() ||
        request[0][1].As<std::uint64_t>() != source.Value()) {
      return MakeError(ErrorCode::kConflict, "战队转让请求已失效");
    }
    const auto clan_id = request[0][0].As<std::uint64_t>();
    transaction.Exec(
        "UPDATE `ClanLeadershipTransfers` SET `TransferStatus`=$2,`ResolvedAt`=CURRENT_TIMESTAMP(6)"
        " WHERE `ClanId`=$1 AND `TransferStatus`='pending'",
        bf::infrastructure::mysql::Parameters{
            transaction, clan_id, accepted ? "accepted" : "declined"});
    WriteAudit(transaction, clan_id, target,
               accepted ? "transfer_accept" : "transfer_decline", source);
    auto summary = LoadSummary(transaction, clan_id);
    transaction.Commit();
    return summary;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanSummary>("回复会长转让", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanSummary>("回复会长转让", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanSummary>("回复会长转让", error);
  }
}

ServiceResult<ClanMemberChangeResult> MySqlClanRepository::FinishTransfer(
    domain::PlayerId master, domain::PlayerId target,
    const std::string& target_nickname) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto managed = ManagerClan(transaction, master, true, true);
    if (const auto* error = std::get_if<ServiceError>(&managed)) {
      return *error;
    }
    const auto clan_id =
        std::get<std::pair<std::uint64_t, std::string>>(managed).first;
    const auto member = transaction.Exec(
        "SELECT membership.`RoleCode`,profile.`Nickname` FROM `ClanMemberships` AS membership JOIN "
        "`PlayerProfiles` AS profile ON profile.`PlayerId`=membership.`PlayerId` WHERE "
        "membership.`PlayerId`=$1 AND membership.`ClanId`=$2 FOR UPDATE OF membership,profile",
        bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                              clan_id});
    const auto request = transaction.Exec(
        "SELECT 1 FROM `ClanLeadershipTransfers` WHERE `ClanId`=$1 AND `SourcePlayerId`=$2 AND "
        "`TargetPlayerId`=$3 AND `TransferStatus`='accepted' AND `LegacyImportBatchId` IS NULL AND "
        "`TransferId`=(SELECT max(`TransferId`) FROM `ClanLeadershipTransfers` WHERE `ClanId`=$1) "
        "AND `ExpiresAt`>CURRENT_TIMESTAMP(6) FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                              master.Value(), target.Value()});
    if (member.empty() || member[0][1].As<std::string>() != target_nickname ||
        request.empty()) {
      return MakeError(ErrorCode::kConflict,
                       "战队转让尚未获同意或接收方身份已改变");
    }
    transaction.Exec(
        "UPDATE `ClanMemberships` SET `RoleCode`='member' WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, master.Value()});
    transaction.Exec(
        "UPDATE `ClanMemberships` SET `RoleCode`='master' WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, target.Value()});
    transaction.Exec("UPDATE `Clans` SET `LeaderPlayerId`=$2 WHERE `ClanId`=$1",
                     bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                                           target.Value()});
    // Consume the authorization in this transaction. The immutable
    // accept/transfer audit events retain history; the approval cannot be
    // reused after leadership returns.
    transaction.Exec(
        "UPDATE `ClanLeadershipTransfers` SET "
        "`TransferStatus`='cancelled',`ResolvedAt`=CURRENT_TIMESTAMP(6) WHERE `ClanId`=$1 AND "
        "`TransferStatus` IN ('pending','accepted')",
        bf::infrastructure::mysql::Parameters{transaction, clan_id});
    WriteAudit(transaction, clan_id, master, "transfer", target);
    auto summary = LoadSummary(transaction, clan_id);
    if (const auto* error = std::get_if<ServiceError>(&summary)) {
      return *error;
    }
    transaction.Commit();
    return ClanMemberChangeResult{
        .clan = std::get<ClanSummary>(std::move(summary)),
        .target_player_id = target,
        .target_nickname = target_nickname,
        .new_role = static_cast<std::int32_t>(application::kClanMasterRole),
        .joined_from_application = false};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("完成会长转让", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("完成会长转让", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanMemberChangeResult>("完成会长转让", error);
  }
}

ServiceResult<ClanPostView> MySqlClanRepository::AddPost(
    domain::PlayerId author, const std::string& body) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto membership = transaction.Exec(
        "SELECT membership.`ClanId`,profile.`Nickname`,progress.`Experience` FROM `ClanMemberships`"
        " AS membership JOIN `Clans` AS clan ON clan.`ClanId`=membership.`ClanId` JOIN "
        "`PlayerProfiles` AS profile ON profile.`PlayerId`=membership.`PlayerId` JOIN "
        "`PlayerProgressions` AS progress ON progress.`PlayerId`=membership.`PlayerId` WHERE "
        "membership.`PlayerId`=$1 FOR UPDATE OF clan",
        bf::infrastructure::mysql::Parameters{transaction, author.Value()});
    if (membership.empty()) {
      return MakeError(ErrorCode::kInvalidState, "请先加入战队");
    }
    const auto [step, maximum] = RewardLevelPolicy(transaction);
    auto level =
        DisplayLevel(membership[0][2].As<std::int64_t>(), step, maximum);
    if (const auto* error = std::get_if<ServiceError>(&level)) {
      return *error;
    }
    const auto post = transaction.Exec(
        "INSERT INTO `ClanPosts`(`ClanId`,`AuthorPlayerId`,`AuthorSnapshot`,`PostBody`) "
        "VALUES($1,$2,$3,$4) RETURNING `PostId`,floor(UNIX_TIMESTAMP(`CreatedAt`))",
        bf::infrastructure::mysql::Parameters{
            transaction, membership[0][0].As<std::uint64_t>(), author.Value(),
            membership[0][1].As<std::string>(), body});
    ClanPostView result{
        .post_id = post[0][0].As<std::uint64_t>(),
        .author_player_id = author,
        .author_nickname = membership[0][1].As<std::string>(),
        .author_level = std::get<std::uint32_t>(level),
        .body = body,
        .created_at = FromUnixSeconds(post[0][1].As<std::int64_t>())};
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanPostView>("新增战队留言", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanPostView>("新增战队留言", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanPostView>("新增战队留言", error);
  }
}

ServiceResult<bool> MySqlClanRepository::DeletePost(domain::PlayerId actor,
                                                    std::uint64_t post_id) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto post = transaction.Exec(
        "SELECT post.`ClanId`,post.`AuthorPlayerId`,membership.`RoleCode` FROM `ClanPosts` AS post "
        "JOIN `ClanMemberships` AS membership ON membership.`ClanId`=post.`ClanId` AND "
        "membership.`PlayerId`=$1 JOIN `Clans` AS clan ON clan.`ClanId`=post.`ClanId` WHERE "
        "post.`PostId`=$2 FOR UPDATE OF clan,post",
        bf::infrastructure::mysql::Parameters{transaction, actor.Value(),
                                              post_id});
    if (post.empty()) {
      return MakeError(ErrorCode::kPermissionDenied, "不能删除这条战队留言");
    }
    const auto author = PlayerId(post[0][1]);
    const auto role = post[0][2].As<std::string>();
    if (!author.has_value() ||
        (*author != actor && role != "master" && role != "deputy")) {
      return MakeError(ErrorCode::kPermissionDenied, "不能删除这条战队留言");
    }
    const bool removed =
        transaction
            .Exec("DELETE FROM `ClanPosts` WHERE `PostId`=$1",
                  bf::infrastructure::mysql::Parameters{transaction, post_id})
            .AffectedRows() == 1U;
    transaction.Commit();
    return removed;
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<bool>("删除战队留言", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<bool>("删除战队留言", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<bool>("删除战队留言", error);
  }
}

ServiceResult<ClanChatResult> MySqlClanRepository::SendChat(
    domain::PlayerId sender, const std::string& body) {
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    if (transaction
            .Exec("SELECT `PlayerChatDenied`($1)",
                  bf::infrastructure::mysql::Parameters{transaction,
                                                        sender.Value()})[0][0]
            .As<bool>())
      return MakeError(ErrorCode::kPermissionDenied,
                       "账号处于禁言或封禁期间，暂时无法发送消息");

    const auto membership = transaction.Exec(
        "SELECT membership.`ClanId`,profile.`Nickname` FROM `ClanMemberships` AS membership JOIN "
        "`Clans` AS clan ON clan.`ClanId`=membership.`ClanId` JOIN `PlayerProfiles` AS profile ON "
        "profile.`PlayerId`=membership.`PlayerId` WHERE membership.`PlayerId`=$1 FOR UPDATE OF clan",
        bf::infrastructure::mysql::Parameters{transaction, sender.Value()});
    if (membership.empty()) {
      return MakeError(ErrorCode::kInvalidState, "请先加入战队");
    }
    const auto clan_id = membership[0][0].As<std::uint64_t>();
    const auto nickname = membership[0][1].As<std::string>();
    transaction.Exec(
        "INSERT INTO `ClanMessages`(`ClanId`,`SenderPlayerId`,`SenderSnapshot`,`MessageBody`) "
        "VALUES($1,$2,$3,$4)",
        bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                              sender.Value(), nickname, body});
    const auto rows = transaction.Exec(
        "SELECT member.`PlayerId` FROM `ClanMemberships` AS member WHERE member.`ClanId`=$1 AND NOT"
        " EXISTS(SELECT 1 FROM `PlayerBlocks` AS block WHERE "
        "(block.`OwnerPlayerId`=member.`PlayerId` AND block.`BlockedPlayerId`=$2) OR "
        "(block.`OwnerPlayerId`=$2 AND block.`BlockedPlayerId`=member.`PlayerId`)) ORDER BY "
        "member.`PlayerId`",
        bf::infrastructure::mysql::Parameters{transaction, clan_id,
                                              sender.Value()});
    std::vector<domain::PlayerId> recipients;
    recipients.reserve(rows.size());
    for (const auto& row : rows) {
      const auto recipient = PlayerId(row[0]);
      if (!recipient.has_value()) {
        return MakeError(ErrorCode::kInternal, "战队聊天目标包含无效玩家");
      }
      recipients.push_back(*recipient);
    }
    transaction.Commit();
    return ClanChatResult{.clan_id = clan_id,
                          .recipients = std::move(recipients),
                          .sender_nickname = nickname,
                          .body = body};
  } catch (const bf::infrastructure::mysql::BrokenConnection& error) {
    return CaughtDatabaseError<ClanChatResult>("保存战队聊天", error);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return CaughtDatabaseError<ClanChatResult>("保存战队聊天", error);
  } catch (const std::exception& error) {
    return CaughtDatabaseError<ClanChatResult>("保存战队聊天", error);
  }
}

ServiceResult<application::ClanWhisperResult> MySqlClanRepository::Whisper(
    domain::PlayerId sender, const std::string& target_name,
    const std::string& body, std::uint32_t kind,
    const application::ClanOnlineLookup& online) {
  using Result = application::ClanWhisperResult;
  auto lease_result = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&lease_result))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    if (transaction
            .Exec("SELECT `PlayerChatDenied`($1)",
                  bf::infrastructure::mysql::Parameters{transaction,
                                                        sender.Value()})[0][0]
            .As<bool>())
      return MakeError(ErrorCode::kPermissionDenied,
                       "账号处于禁言或封禁期间，暂时无法发送消息");

    // The UI accepts a nickname; automatic replies carry the canonical login.
    // Prefer an exact nickname when a login happens to have the same spelling.
    const auto target_rows = transaction.Exec(
        "SELECT profile.`PlayerId` FROM `PlayerProfiles` AS profile JOIN `Accounts` AS account "
        "USING(`AccountId`) WHERE account.`AccountStatus`='active' AND profile.`IsCharacterCreated`"
        " AND (profile.`Nickname`=$1 OR account.`LoginKey`=lower($1)) ORDER BY "
        "(profile.`Nickname`=$1) DESC,profile.`PlayerId` LIMIT 1",
        bf::infrastructure::mysql::Parameters{transaction, target_name});
    if (target_rows.empty())
      return MakeError(ErrorCode::kInvalidState, "私聊目标不存在或未在线");
    const domain::PlayerId target{target_rows[0][0].As<std::uint64_t>()};
    if (target == sender)
      return MakeError(ErrorCode::kInvalidState, "不能向自己发送私聊");
    const auto people = transaction.Exec(
        "SELECT profile.`PlayerId`,account.`LoginName`,profile.`Nickname`,account.`LoginKey` FROM "
        "`PlayerProfiles` AS profile JOIN `Accounts` AS account USING(`AccountId`) WHERE "
        "profile.`PlayerId` IN ($1,$2) AND account.`AccountStatus`='active' ORDER BY "
        "profile.`PlayerId` FOR UPDATE OF profile",
        bf::infrastructure::mysql::Parameters{transaction, sender.Value(),
                                              target.Value()});
    if (people.size() != 2U || !online(target))
      return MakeError(ErrorCode::kInvalidState, "私聊目标不存在或未在线");
    Result result{.recipient = target};
    for (const auto& row : people) {
      if (row[0].As<std::uint64_t>() == sender.Value()) {
        result.sender_account = row[1].As<std::string>();
        result.sender_nickname = row[2].As<std::string>();
      } else {
        result.recipient_account = row[1].As<std::string>();
        result.recipient_nickname = row[2].As<std::string>();
      }
    }
    // Re-resolve under the profile locks, including a concurrent nickname
    // change.
    const auto current_target = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` AS profile JOIN `Accounts` AS account USING(`AccountId`) "
        "WHERE profile.`PlayerId`=$1 AND (profile.`Nickname`=$2 OR account.`LoginKey`=lower($2))",
        bf::infrastructure::mysql::Parameters{transaction, target.Value(),
                                              target_name});
    if (current_target.empty())
      return MakeError(ErrorCode::kConflict, "私聊目标资料已经改变");
    if (!transaction
             .Exec(
                 "SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR "
                 "(`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)",
                 bf::infrastructure::mysql::Parameters{
                     transaction, sender.Value(), target.Value()})
             .empty())
      return MakeError(ErrorCode::kPermissionDenied, "双方当前不能私聊");
    if (kind >= 3U &&
        transaction
            .Exec(
                "SELECT 1 FROM `PrivateMessages` WHERE `SenderPlayerId`=$1 AND `RecipientPlayerId`=$2 AND "
                "`CreatedAt`>=CURRENT_TIMESTAMP(6)-INTERVAL 60 SECOND LIMIT 1",
                bf::infrastructure::mysql::Parameters{
                    transaction, target.Value(), sender.Value()})
            .empty())
      return MakeError(ErrorCode::kPermissionDenied,
                       "自动回复缺少近期私聊请求");
    if (kind >= 3U &&
        !transaction
             .Exec(
                 "SELECT 1 FROM `PrivateMessages` WHERE `SenderPlayerId`=$1 AND `RecipientPlayerId`=$2 AND "
                 "`MessageBody`=$3 AND `CreatedAt`>=CURRENT_TIMESTAMP(6)-INTERVAL 5 SECOND LIMIT 1",
                 bf::infrastructure::mysql::Parameters{
                     transaction, sender.Value(), target.Value(), body})
             .empty())
      return MakeError(ErrorCode::kConflict, "自动回复已经发送");
    transaction.Exec(
        "INSERT INTO `PrivateMessages`(`SenderPlayerId`,`RecipientPlayerId`,`MessageBody`) "
        "VALUES($1,$2,$3)",
        bf::infrastructure::mysql::Parameters{transaction, sender.Value(),
                                              target.Value(), body});
    transaction.Commit();
    return result;
  } catch (const std::exception& error) {
    // Deliberately exclude SQL detail: INSERT diagnostics may include private
    // text.
    static_cast<void>(error);
    return StorageError("保存私聊时数据库操作失败", true);
  }
}

}  // namespace bf::infrastructure::mysql
