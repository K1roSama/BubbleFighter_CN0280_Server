#include "persistence/mysql/world_access_repository.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <utility>
#include <variant>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

}  // namespace

MySqlWorldAccessReader::MySqlWorldAccessReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::optional<application::WorldAccessSnapshot>>
MySqlWorldAccessReader::Load(domain::PlayerId player_id) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto identity = transaction.Exec(
        "SELECT EXISTS(SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1),EXISTS(SELECT 1 FROM "
        "`ClanMemberships` WHERE `PlayerId`=$1),EXISTS(SELECT 1 FROM `ChatPenaltyHistory` WHERE "
        "`PlayerId`=$1 AND `RevokedAt` IS NULL)",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (!identity[0][0].As<bool>()) {
      transaction.Commit();
      return std::optional<application::WorldAccessSnapshot>{};
    }
    application::WorldAccessSnapshot result{
        .player_id = player_id,
        .has_clan_membership = identity[0][1].As<bool>(),
        .has_chat_penalty_history = identity[0][2].As<bool>()};
    const auto blocked = transaction.Exec(
        "SELECT peer_id FROM (SELECT `BlockedPlayerId` AS peer_id FROM `PlayerBlocks` WHERE "
        "`OwnerPlayerId`=$1 UNION SELECT `OwnerPlayerId` AS peer_id FROM `PlayerBlocks` WHERE "
        "`BlockedPlayerId`=$1) AS peers ORDER BY peer_id",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    result.chat_blocked_peers.reserve(blocked.size());
    for (const auto& row : blocked) {
      const auto value = row[0].As<std::int64_t>();
      if (value <= 0) {
        return MakeError(application::ErrorCode::kInternal,
                         "MySQL 世界访问快照包含无效屏蔽玩家");
      }
      result.chat_blocked_peers.emplace_back(static_cast<std::uint64_t>(value));
    }
    const auto moderation = transaction.Exec(
        "SELECT `Muted`,`Banned` FROM `PlayerModerationState` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (moderation.empty())
      return MakeError(application::ErrorCode::kUnauthenticated,
                       "账号状态无效");
    result.muted = moderation[0][0].As<bool>();
    result.banned = moderation[0][1].As<bool>();
    transaction.Commit();
    return std::optional<application::WorldAccessSnapshot>{std::move(result)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取世界访问快照时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取世界访问快照 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 拒绝读取世界访问快照");
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取世界访问快照时发生未分类错误");
  }
}

std::optional<application::ServiceError> MySqlWorldAccessReader::ActivateDevice(
    domain::PlayerId player, std::string_view code) {
  // Clear historical device bindings; access checks use account sanctions only.
  code = {};
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    tx.Exec(
        "SELECT LockKey FROM TransactionLocks WHERE LockKey='moderation' FOR SHARE");
    const auto changed = tx.Exec(
        "UPDATE `Accounts` a, `PlayerProfiles` p SET a.`ActiveMachineCode`=NULLIF($2,'') WHERE "
        "p.`PlayerId`=$1 AND a.`AccountId`=p.`AccountId` AND NOT "
        "`AccountBanActive`(a.`AccountId`,NULLIF($2,'')) RETURNING a.`AccountId`",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), code});
    if (changed.empty())
      return MakeError(application::ErrorCode::kUnauthenticated,
                       "账号已被封禁");
    tx.Commit();
    return std::nullopt;
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法刷新账号状态", true);
  }
}
}  // namespace bf::infrastructure::mysql
