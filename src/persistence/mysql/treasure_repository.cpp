#include "persistence/mysql/treasure_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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

using Json = nlohmann::json;

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

application::ServiceError MakeSqlError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

application::ServiceError CorruptDraw(std::string message) {
  return MakeError(application::ErrorCode::kInternal,
                   "泡泡抽取持久化快照损坏：" + std::move(message));
}

std::int64_t UnixMicroseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             value.time_since_epoch())
      .count();
}

std::int64_t UnixSeconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

std::optional<std::uint32_t> CheckedU32(std::int64_t value) {
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       static_cast<std::uint64_t>(
                           std::numeric_limits<std::uint32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

Json RewardJson(const application::ServiceRewardRule& reward) {
  Json value{{"stock_id", reward.stock_id.Value()},
             {"item_id", reward.item_id.Value()},
             {"quantity", reward.quantity}};
  if (reward.duration.has_value()) {
    value["duration_seconds"] = reward.duration->count();
  } else {
    value["duration_seconds"] = nullptr;
  }
  return value;
}

application::ServiceResult<application::ServiceRewardRule> ParseReward(
    const Json& value) {
  try {
    if (!value.is_object() || !value.at("stock_id").is_number_unsigned() ||
        !value.at("item_id").is_number_unsigned() ||
        !value.at("quantity").is_number_unsigned()) {
      return CorruptDraw("奖励字段类型无效");
    }
    const auto stock = value.at("stock_id").get<std::uint64_t>();
    const auto item = value.at("item_id").get<std::uint64_t>();
    const auto quantity = value.at("quantity").get<std::uint64_t>();
    if (stock == 0U || item == 0U || quantity == 0U ||
        quantity > std::numeric_limits<std::uint32_t>::max()) {
      return CorruptDraw("奖励字段范围无效");
    }
    std::optional<std::chrono::seconds> duration;
    const auto& duration_json = value.at("duration_seconds");
    if (!duration_json.is_null()) {
      if (!duration_json.is_number_unsigned() ||
          duration_json.get<std::uint64_t>() == 0U ||
          duration_json.get<std::uint64_t>() >
              static_cast<std::uint64_t>(std::chrono::seconds::max().count())) {
        return CorruptDraw("奖励期限范围无效");
      }
      duration =
          std::chrono::seconds{duration_json.get<std::chrono::seconds::rep>()};
    }
    return application::ServiceRewardRule{
        .stock_id = domain::StockId{stock},
        .item_id = domain::ItemId{item},
        .quantity = static_cast<std::uint32_t>(quantity),
        .duration = duration};
  } catch (const Json::exception&) {
    return CorruptDraw("奖励字段缺失");
  }
}

application::ServiceResult<application::TreasureDrawSnapshot> ParseDrawRow(
    const bf::infrastructure::mysql::Row& row) {
  try {
    const auto positions_json =
        Json::parse(row[3].As<std::string>(), nullptr, false, true);
    const auto rewards_json =
        Json::parse(row[4].As<std::string>(), nullptr, false, true);
    const auto needle_quantity = CheckedU32(row[7].As<std::int64_t>());
    const auto bubble_balance = CheckedU32(row[8].As<std::int64_t>());
    const auto selection_seconds = CheckedU32(row[6].As<std::int64_t>());
    if (!positions_json.is_array() || positions_json.size() != 2U ||
        !rewards_json.is_array() || rewards_json.size() != 5U ||
        !needle_quantity.has_value() || !bubble_balance.has_value() ||
        !selection_seconds.has_value() || *selection_seconds == 0U) {
      return CorruptDraw("格位、奖励或当前资产字段无效");
    }
    application::TreasureDrawSnapshot draw{
        .draw_id = row[0].As<std::uint64_t>(),
        .player_id = domain::PlayerId{row[1].As<std::uint64_t>()},
        .needle_item_id = domain::ItemId{row[2].As<std::uint64_t>()},
        .needle_quantity_after = *needle_quantity,
        .bubble_balance_after = *bubble_balance,
        .positions = {},
        .rewards = {},
        .selection_duration = std::chrono::seconds{*selection_seconds},
        .rules_version_id = row[5].As<std::uint64_t>()};
    for (std::size_t index = 0U; index < draw.positions.size(); ++index) {
      if (!positions_json[index].is_number_unsigned() ||
          positions_json[index].get<std::uint64_t>() > 4U) {
        return CorruptDraw("格位范围无效");
      }
      draw.positions[index] = positions_json[index].get<std::uint32_t>();
    }
    for (std::size_t index = 0U; index < draw.rewards.size(); ++index) {
      auto reward = ParseReward(rewards_json[index]);
      if (const auto* error = std::get_if<application::ServiceError>(&reward)) {
        return *error;
      }
      draw.rewards[index] =
          std::get<application::ServiceRewardRule>(std::move(reward));
    }
    return draw;
  } catch (const Json::exception&) {
    return CorruptDraw("JSON 无法解析");
  } catch (const bf::infrastructure::mysql::ConversionError&) {
    return CorruptDraw("关系字段无法转换");
  }
}

bf::infrastructure::mysql::QueryResult PendingDraw(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, std::int64_t now_microseconds, bool lock) {
  std::string query =
      "SELECT "
      "draw.`DrawId`,draw.`PlayerId`,draw.`NeedleItemId`,draw.`Positions`,draw.`RewardSnapshot`,draw.`RulesVersionId`,(JSON_UNQUOTE(JSON_EXTRACT(configuration.`Payload`,'$.policy.bubbles.selection_seconds'))),COALESCE((SELECT"
      " item.`Quantity` FROM `PlayerItems` item WHERE item.`PlayerId`=draw.`PlayerId` AND "
      "item.`ItemId`=draw.`NeedleItemId` AND (item.`ExpiresAt` IS NULL OR "
      "item.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0))),0),wallet.`Balance` FROM `BubbleDraws` draw"
      " JOIN `ConfigurationVersions` configuration ON "
      "configuration.`ConfigurationVersionId`=draw.`RulesVersionId` JOIN `PlayerWallets` wallet "
      "ON wallet.`PlayerId`=draw.`PlayerId` AND wallet.`CurrencyCode`='treasure_bubble' WHERE "
      "draw.`PlayerId`=$1 AND draw.`DrawStatus`='pending'";
  if (lock) {
    query += " FOR UPDATE OF draw";
  }
  return transaction.Exec(
      query, bf::infrastructure::mysql::Parameters{
                 transaction, player_id.Value(), now_microseconds});
}

application::ServiceResult<std::optional<application::TreasureDrawSnapshot>>
ReadPending(bf::infrastructure::mysql::TransactionBase& transaction,
            domain::PlayerId player_id, std::int64_t now_microseconds,
            bool lock) {
  const auto rows = PendingDraw(transaction, player_id, now_microseconds, lock);
  if (rows.empty()) {
    return std::optional<application::TreasureDrawSnapshot>{};
  }
  if (rows.size() != 1U) {
    return CorruptDraw("同一玩家存在多个 pending 结果");
  }
  auto parsed = ParseDrawRow(rows[0]);
  if (const auto* error = std::get_if<application::ServiceError>(&parsed)) {
    return *error;
  }
  return std::optional<application::TreasureDrawSnapshot>{
      std::get<application::TreasureDrawSnapshot>(std::move(parsed))};
}

Json RewardArray(
    const std::array<application::ServiceRewardRule, 5U>& rewards) {
  Json output = Json::array();
  for (const auto& reward : rewards) {
    output.push_back(RewardJson(reward));
  }
  return output;
}

Json PositionArray(const std::array<std::uint32_t, 2U>& positions) {
  return Json::array({positions[0], positions[1]});
}

struct StoredItem final {
  domain::ItemInstanceId instance_id;
  std::int64_t quantity{0};
  std::optional<std::int64_t> expires_at;
  bool active{false};
  bool equipped{false};
};

std::optional<StoredItem> LockItem(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id,
    std::int64_t now_seconds) {
  const auto rows = transaction.Exec(
      "SELECT item.`ItemInstanceId`,item.`Quantity`,CASE WHEN item.`ExpiresAt` IS NULL THEN NULL "
      "ELSE ROUND(UNIX_TIMESTAMP(item.`ExpiresAt`)) END,(item.`ExpiresAt` IS NULL OR "
      "item.`ExpiresAt`>FROM_UNIXTIME($3)),EXISTS(SELECT 1 FROM `PlayerEquipment` equipment WHERE"
      " equipment.`PlayerId`=item.`PlayerId` AND "
      "equipment.`ItemInstanceId`=item.`ItemInstanceId`) FROM `PlayerItems` item WHERE "
      "item.`PlayerId`=$1 AND item.`ItemId`=$2 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            item_id.Value(), now_seconds});
  if (rows.empty()) {
    return std::nullopt;
  }
  return StoredItem{
      .instance_id = domain::ItemInstanceId{rows[0][0].As<std::uint64_t>()},
      .quantity = rows[0][1].As<std::int64_t>(),
      .expires_at = rows[0][2].IsNull()
                        ? std::nullopt
                        : std::optional{rows[0][2].As<std::int64_t>()},
      .active = rows[0][3].As<bool>(),
      .equipped = rows[0][4].As<bool>()};
}

struct GrantResult final {
  domain::ItemInstanceId instance_id;
  std::int64_t quantity_delta{0};
  std::optional<std::int64_t> expires_before;
  std::optional<std::int64_t> expires_after;
  bool changed{false};
};

application::ServiceResult<GrantResult> GrantReward(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, const application::ServiceRewardRule& reward,
    std::int64_t now_seconds, std::string_view operation_id) {
  const auto template_rows = transaction.Exec(
      "SELECT template.`StackPolicy`,template.`MaxQuantity`,template.`MaxDurationSeconds` FROM "
      "`ItemTemplates` template JOIN `ConfigurationVersions` configuration ON "
      "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
      "template.`ItemId`=$1 AND template.`IsGrantable` AND "
      "configuration.`ReleaseStatus`='published'",
      bf::infrastructure::mysql::Parameters{transaction,
                                            reward.item_id.Value()});
  if (template_rows.empty()) {
    return MakeError(application::ErrorCode::kConflict,
                     "泡泡奖励物品当前不可发放");
  }
  const auto stack_policy = template_rows[0][0].As<std::string>();
  const auto maximum_quantity = template_rows[0][1].As<std::int64_t>();
  const auto maximum_duration =
      template_rows[0][2].IsNull()
          ? std::nullopt
          : std::optional{template_rows[0][2].As<std::int64_t>()};
  auto old = LockItem(transaction, player_id, reward.item_id, now_seconds);
  if (old.has_value() && !old->active) {
    transaction.Exec(
        "DELETE FROM `PlayerEquipment` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              old->instance_id.Value()});
  }

  std::int64_t quantity_after = 1;
  std::int64_t quantity_delta = 0;
  std::optional<std::int64_t> expires_after;
  const auto expires_before = old.has_value() ? old->expires_at : std::nullopt;
  if (stack_policy == "merge_quantity") {
    if (reward.duration.has_value()) {
      return CorruptDraw("数量合并奖励却保存了期限");
    }
    const auto active_quantity =
        old.has_value() && old->active ? old->quantity : 0;
    if (reward.quantity > static_cast<std::uint64_t>(maximum_quantity) ||
        active_quantity > maximum_quantity - reward.quantity) {
      return MakeError(application::ErrorCode::kConflict,
                       "奖励道具数量已达到上限");
    }
    quantity_after = active_quantity + reward.quantity;
    quantity_delta = reward.quantity;
  } else if (stack_policy == "single_instance") {
    if (reward.quantity != 1U) {
      return CorruptDraw("单实例奖励数量不是一");
    }
    if (reward.duration.has_value()) {
      if (!maximum_duration.has_value()) {
        return CorruptDraw("限时奖励模板没有期限上限");
      }
      if (old.has_value() && old->active && !old->expires_at.has_value()) {
        expires_after = std::nullopt;
      } else {
        const auto base =
            old.has_value() && old->active && old->expires_at.has_value()
                ? std::max(now_seconds, *old->expires_at)
                : now_seconds;
        if (reward.duration->count() > *maximum_duration ||
            base - now_seconds > *maximum_duration - reward.duration->count()) {
          return MakeError(application::ErrorCode::kConflict,
                           "奖励道具期限已达到上限");
        }
        expires_after = base + reward.duration->count();
      }
    }
    quantity_delta = old.has_value() && old->active ? 0 : 1;
  } else {
    return CorruptDraw("奖励模板使用未知库存合并策略");
  }

  const auto item_rows = transaction.Exec(
      "INSERT INTO "
      "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
      "VALUES($1,$2,$3,FROM_UNIXTIME($4),CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) "
      "END,'bubble_reward') ON CONFLICT(`PlayerId`,`ItemId`) DO UPDATE SET "
      "`Quantity`=VALUES(`Quantity`),`AcquiredAt`=CASE WHEN `PlayerItems`.`ExpiresAt` IS NOT NULL"
      " AND `PlayerItems`.`ExpiresAt`<=FROM_UNIXTIME($4) THEN VALUES(`AcquiredAt`) ELSE "
      "`PlayerItems`.`AcquiredAt` "
      "END,`ExpiresAt`=VALUES(`ExpiresAt`),`SourceCode`='bubble_reward' RETURNING "
      "`ItemInstanceId`",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), reward.item_id.Value(),
          quantity_after, now_seconds, expires_after});
  const auto instance_id =
      domain::ItemInstanceId{item_rows[0][0].As<std::uint64_t>()};
  const bool changed = quantity_delta != 0 || expires_before != expires_after;
  if (changed) {
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $6"
        " IS NULL THEN NULL ELSE FROM_UNIXTIME($6) END,'bubble_reward',$7,FROM_UNIXTIME($8))",
        bf::infrastructure::mysql::Parameters{
            transaction, player_id.Value(), reward.item_id.Value(),
            instance_id.Value(), quantity_delta, expires_before, expires_after,
            operation_id, now_seconds});
  }
  return GrantResult{.instance_id = instance_id,
                     .quantity_delta = quantity_delta,
                     .expires_before = expires_before,
                     .expires_after = expires_after,
                     .changed = changed};
}

}  // namespace

MySqlTreasureRepository::MySqlTreasureRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::TreasureStateSnapshot>
MySqlTreasureRepository::LoadState(
    domain::PlayerId player_id, const application::BubbleServiceRules& rules,
    std::chrono::system_clock::time_point observed_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto now = UnixMicroseconds(observed_at);
    const auto wallet = transaction.Exec(
        "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND "
        "`CurrencyCode`='treasure_bubble'",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (wallet.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "玩家泡泡钱包不存在");
    }
    const auto balance = CheckedU32(wallet[0][0].As<std::int64_t>());
    if (!balance.has_value()) {
      return CorruptDraw("泡泡钱包超出协议范围");
    }
    Json needle_ids = Json::array();
    for (const auto& needle : rules.needles) {
      needle_ids.push_back(needle.item_id.Value());
    }
    const auto rows = transaction.Exec(
        "SELECT requested.value,COALESCE(item.`Quantity`,0) FROM JSON_TABLE($2,'$[*]' COLUMNS "
        "(value BIGINT PATH '$',ordering FOR ORDINALITY)) requested LEFT JOIN `PlayerItems` item ON"
        " item.`PlayerId`=$1 AND item.`ItemId`=requested.value AND (item.`ExpiresAt` IS NULL OR "
        "item.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) ORDER BY requested.ordering",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              needle_ids.dump(), now});
    application::TreasureStateSnapshot snapshot{.player_id = player_id,
                                                .bubble_balance = *balance,
                                                .needle_counts = {},
                                                .pending_draw = std::nullopt};
    snapshot.needle_counts.reserve(rows.size());
    for (const auto& row : rows) {
      const auto quantity = CheckedU32(row[1].As<std::int64_t>());
      if (!quantity.has_value()) {
        return CorruptDraw("魔法针数量超出协议范围");
      }
      snapshot.needle_counts.push_back(
          {.item_id = domain::ItemId{row[0].As<std::uint64_t>()},
           .quantity = *quantity});
    }
    auto pending = ReadPending(transaction, player_id, now, false);
    if (const auto* error = std::get_if<application::ServiceError>(&pending)) {
      return *error;
    }
    snapshot.pending_draw =
        std::get<std::optional<application::TreasureDrawSnapshot>>(
            std::move(pending));
    transaction.Commit();
    return snapshot;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取泡泡抽取状态时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("读取泡泡抽取状态", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取泡泡抽取状态时发生未分类错误");
  }
}

application::ServiceResult<std::pair<application::TreasureDrawSnapshot, bool>>
MySqlTreasureRepository::Open(
    const application::TreasureOpenPersistenceCommand& command) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now_us = UnixMicroseconds(command.now);
    const auto now_seconds_value = UnixSeconds(command.now);
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState, "玩家不存在");
    }
    auto pending = ReadPending(transaction, command.player_id, now_us, true);
    if (const auto* error = std::get_if<application::ServiceError>(&pending)) {
      return *error;
    }
    auto existing = std::get<std::optional<application::TreasureDrawSnapshot>>(
        std::move(pending));
    if (existing.has_value()) {
      transaction.Commit();
      return std::pair{*existing, false};
    }
    const auto rules = transaction.Exec(
        "SELECT 1 FROM `ConfigurationVersions` WHERE `ConfigurationVersionId`=$1 AND "
        "`ConfigurationKind`='service_rules' AND `ReleaseStatus`='published'",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.rules_version_id});
    if (rules.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "泡泡规则版本已不可用");
    }
    const auto operation_id = command.operation_id.ToUuidString();
    const auto canonical =
        "bubble_open:" + std::to_string(command.player_id.Value()) + ":" +
        std::to_string(command.needle.item_id.Value()) + ":" +
        PositionArray(command.positions).dump() + ":" +
        RewardArray(command.rewards).dump();
    transaction.Exec(
        "INSERT INTO "
        "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
        " VALUES($1,$2,'bubble_open',$1,UNHEX(SHA2($3,256)))",
        bf::infrastructure::mysql::Parameters{
            transaction, operation_id, command.player_id.Value(), canonical});

    const auto wallet = transaction.Exec(
        "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND "
        "`CurrencyCode`='treasure_bubble' FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.player_id.Value()});
    if (wallet.empty() || wallet[0][0].As<std::int64_t>() < 1) {
      return MakeError(application::ErrorCode::kInsufficientBalance,
                       "魔法泡泡不足");
    }
    const auto balance_before = wallet[0][0].As<std::int64_t>();
    const auto balance_after = balance_before - 1;

    auto needle = LockItem(transaction, command.player_id,
                           command.needle.item_id, now_seconds_value);
    if (!needle.has_value() || !needle->active ||
        needle->quantity < command.needle.cost) {
      return MakeError(application::ErrorCode::kConflict,
                       "魔法针数量不足或已过期");
    }
    if (needle->equipped) {
      return MakeError(application::ErrorCode::kConflict,
                       "请先卸下要消耗的魔法针");
    }
    const auto needle_after = needle->quantity - command.needle.cost;
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $5"
        " IS NULL OR $6=0 THEN NULL ELSE FROM_UNIXTIME($5) "
        "END,'bubble_needle_consume',$7,FROM_UNIXTIME($8))",
        bf::infrastructure::mysql::Parameters{
            transaction, command.player_id.Value(),
            command.needle.item_id.Value(), needle->instance_id.Value(),
            -static_cast<std::int64_t>(command.needle.cost), needle->expires_at,
            needle_after, operation_id, now_seconds_value});
    if (needle_after == 0) {
      transaction.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
                       bf::infrastructure::mysql::Parameters{
                           transaction, needle->instance_id.Value()});
    } else {
      transaction.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=$2 WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{
              transaction, needle->instance_id.Value(), needle_after});
    }
    transaction.Exec(
        "UPDATE `PlayerWallets` SET `Balance`=$2,`UpdatedAt`=FROM_UNIXTIME($3) WHERE `PlayerId`=$1 "
        "AND `CurrencyCode`='treasure_bubble'",
        bf::infrastructure::mysql::Parameters{
            transaction, command.player_id.Value(), balance_after,
            now_seconds_value});
    transaction.Exec(
        "INSERT INTO "
        "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,'treasure_bubble',-1,$2,$3,'bubble_draw',$4,FROM_UNIXTIME($5))",
        bf::infrastructure::mysql::Parameters{
            transaction, command.player_id.Value(), balance_before,
            balance_after, operation_id, now_seconds_value});
    const auto inserted = transaction.Exec(
        "INSERT INTO "
        "`BubbleDraws`(`PlayerId`,`OperationRequestId`,`NeedleItemId`,`Positions`,`RewardSnapshot`,`DrawStatus`,`RulesVersionId`,`CreatedAt`)"
        " VALUES($1,$2,$3,$4,$5,'pending',$6,FROM_UNIXTIME($7)) RETURNING `DrawId`",
        bf::infrastructure::mysql::Parameters{
            transaction, command.player_id.Value(), operation_id,
            command.needle.item_id.Value(),
            PositionArray(command.positions).dump(),
            RewardArray(command.rewards).dump(), command.rules_version_id,
            now_seconds_value});
    const auto draw_id = inserted[0][0].As<std::uint64_t>();
    transaction.Exec(
        "UPDATE `OperationRequests` SET "
        "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=FROM_UNIXTIME($3) WHERE "
        "`OperationRequestId`=$1",
        bf::infrastructure::mysql::Parameters{
            transaction, operation_id, "bubble_draw:" + std::to_string(draw_id),
            now_seconds_value});
    transaction.Commit();
    return std::pair{
        application::TreasureDrawSnapshot{
            .draw_id = draw_id,
            .player_id = command.player_id,
            .needle_item_id = command.needle.item_id,
            .needle_quantity_after = static_cast<std::uint32_t>(needle_after),
            .bubble_balance_after = static_cast<std::uint32_t>(balance_after),
            .positions = command.positions,
            .rewards = command.rewards,
            .selection_duration = command.selection_duration,
            .rules_version_id = command.rules_version_id},
        true};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(application::ErrorCode::kConflict,
                     "泡泡抽取操作重复或已有待领取结果");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "提交泡泡抽取时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("提交泡泡抽取", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "提交泡泡抽取时发生未分类错误");
  }
}

application::ServiceResult<application::TreasureClaimResult>
MySqlTreasureRepository::Claim(
    const application::TreasureClaimPersistenceCommand& command) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now_us = UnixMicroseconds(command.now);
    const auto now_seconds_value = UnixSeconds(command.now);
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState, "玩家不存在");
    }
    auto pending = ReadPending(transaction, command.player_id, now_us, true);
    if (const auto* error = std::get_if<application::ServiceError>(&pending)) {
      return *error;
    }
    auto draw = std::get<std::optional<application::TreasureDrawSnapshot>>(
        std::move(pending));
    if (!draw.has_value()) {
      // The native claim has no request ID. When its acknowledgement is lost,
      // acknowledge only the latest draw and the exact persisted choice; never
      // run grant_reward again or search older matching draws.
      const auto previous = transaction.Exec(
          "SELECT `DrawId`,`RewardSnapshot`,`ClaimedChoice`,`ClaimedPosition`,`DrawStatus` FROM "
          "`BubbleDraws` WHERE `PlayerId`=$1 ORDER BY `DrawId` DESC LIMIT 1",
          bf::infrastructure::mysql::Parameters{transaction,
                                                command.player_id.Value()});
      if (!previous.empty() &&
          previous[0][4].As<std::string_view>() == "claimed" &&
          !previous[0][2].IsNull() && !previous[0][3].IsNull() &&
          previous[0][2].As<std::uint32_t>() == command.choice &&
          previous[0][3].As<std::uint32_t>() == command.position &&
          command.choice <= 1U) {
        const auto rewards =
            Json::parse(previous[0][1].As<std::string>(), nullptr, false, true);
        if (!rewards.is_array() || rewards.size() != 5U) {
          return CorruptDraw("已领取奖励快照无效");
        }
        application::TreasureClaimResult replay{
            .player_id = command.player_id,
            .draw_id = previous[0][0].As<std::uint64_t>(),
            .position = command.position};
        for (std::size_t index = 0U; index < rewards.size(); ++index) {
          auto parsed = ParseReward(rewards[index]);
          if (const auto* error =
                  std::get_if<application::ServiceError>(&parsed)) {
            return *error;
          }
          const auto& reward = std::get<application::ServiceRewardRule>(parsed);
          replay.stocks[index] = reward.stock_id;
          if (index == command.choice) replay.granted_item_id = reward.item_id;
        }
        transaction.Commit();
        return replay;
      }
      return MakeError(application::ErrorCode::kConflict,
                       "没有等待领取的泡泡奖励");
    }
    if (command.choice > 1U ||
        draw->positions[command.choice] != command.position) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "只能领取刚刚选中的两个泡泡之一");
    }
    const auto operation_id = command.operation_id.ToUuidString();
    const auto canonical =
        "bubble_claim:" + std::to_string(command.player_id.Value()) + ":" +
        std::to_string(draw->draw_id) + ":" + std::to_string(command.choice) +
        ":" + std::to_string(command.position);
    transaction.Exec(
        "INSERT INTO "
        "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
        " VALUES($1,$2,'bubble_claim',$1,UNHEX(SHA2($3,256)))",
        bf::infrastructure::mysql::Parameters{
            transaction, operation_id, command.player_id.Value(), canonical});
    const auto& reward = draw->rewards[command.choice];
    auto grant = GrantReward(transaction, command.player_id, reward,
                             now_seconds_value, operation_id);
    if (const auto* error = std::get_if<application::ServiceError>(&grant)) {
      return *error;
    }
    const auto updated = transaction.Exec(
        "UPDATE `BubbleDraws` SET "
        "`DrawStatus`='claimed',`ClaimedAt`=FROM_UNIXTIME($2),`ClaimedChoice`=$3,`ClaimedPosition`=$4"
        " WHERE `DrawId`=$1 AND `DrawStatus`='pending' RETURNING `DrawId`",
        bf::infrastructure::mysql::Parameters{transaction, draw->draw_id,
                                              now_seconds_value, command.choice,
                                              command.position});
    if (updated.size() != 1U) {
      return MakeError(application::ErrorCode::kConflict,
                       "泡泡奖励已被其他请求领取");
    }
    transaction.Exec(
        "UPDATE `OperationRequests` SET "
        "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=FROM_UNIXTIME($3) WHERE "
        "`OperationRequestId`=$1",
        bf::infrastructure::mysql::Parameters{
            transaction, operation_id,
            "bubble_claim:" + std::to_string(draw->draw_id),
            now_seconds_value});
    transaction.Commit();
    std::array<domain::StockId, 5U> stocks{};
    for (std::size_t index = 0U; index < stocks.size(); ++index) {
      stocks[index] = draw->rewards[index].stock_id;
    }
    return application::TreasureClaimResult{.player_id = command.player_id,
                                            .draw_id = draw->draw_id,
                                            .position = command.position,
                                            .stocks = stocks,
                                            .granted_item_id = reward.item_id};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(application::ErrorCode::kConflict, "泡泡领取操作重复");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "提交泡泡领取时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("提交泡泡领取", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "提交泡泡领取时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
