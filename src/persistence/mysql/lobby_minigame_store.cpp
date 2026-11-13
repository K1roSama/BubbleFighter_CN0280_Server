#include "persistence/mysql/lobby_minigame_store.h"

#include <exception>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

// The server day for "today's" fishing count and fortune paper; clan periods
// use the same zone.
constexpr const char* kDay =
    "DATE(CONVERT_TZ(CURRENT_TIMESTAMP(6),'+00:00','+08:00'))";

application::ServiceError Error(application::ErrorCode code, const char* text) {
  return {.code = code, .safe_message = text};
}

application::ServiceError Unavailable() {
  return Error(application::ErrorCode::kStorageUnavailable,
               "广场小游戏记录暂时不可用");
}

std::string Today(bf::infrastructure::mysql::TransactionBase& tx) {
  return tx.Exec(std::string{"SELECT "} + kDay)[0][0].As<std::string>();
}

bool LockPlayer(bf::infrastructure::mysql::TransactionBase& tx,
                domain::PlayerId player) {
  return !tx.Exec(
                "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR UPDATE",
                bf::infrastructure::mysql::Parameters{tx, player.Value()})
              .empty();
}

std::int64_t LockedLucci(bf::infrastructure::mysql::TransactionBase& tx,
                         domain::PlayerId player) {
  const auto rows = tx.Exec(
      "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`='lucci' FOR "
      "UPDATE",
      bf::infrastructure::mysql::Parameters{tx, player.Value()});
  return rows.empty() ? 0 : rows[0][0].As<std::int64_t>();
}

std::uint16_t FishingCount(bf::infrastructure::mysql::TransactionBase& tx,
                           domain::PlayerId player, const std::string& day) {
  const auto count =
      tx.Exec(
            "SELECT count(*) FROM `OperationRequests` WHERE `PlayerId`=$1 AND "
            "`OperationType`='lmg_fishing' AND `IdempotencyKey` LIKE CONCAT($2,':%')",
            bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                  day})[0][0]
          .As<std::int64_t>();
  return static_cast<std::uint16_t>(
      std::min<std::int64_t>(count, std::numeric_limits<std::uint16_t>::max()));
}

std::string InsertOperation(bf::infrastructure::mysql::TransactionBase& tx,
                            domain::PlayerId player, const char* type,
                            const std::string& key,
                            const std::string& reference) {
  const auto operation = tx.Exec("SELECT UUID()").OneField().As<std::string>();
  tx.Exec(
      "INSERT INTO "
      "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`,`OperationStatus`,`ResultReference`,`CompletedAt`) "
      "VALUES($1,$2,$3,$4,UNHEX(MD5(CONCAT($3,':',$4,':',$5))),'succeeded',$5,CURRENT_TIMESTAMP(6))",
      Parameters{tx, operation, player.Value(), type, key, reference});
  return operation;
}

/** @brief Debit lucci for `cost`; the caller already checked the balance under
 * lock. */
void Charge(bf::infrastructure::mysql::TransactionBase& tx,
            domain::PlayerId player, std::int64_t before, std::int64_t cost,
            const char* reason, const std::string& operation) {
  if (cost == 0) return;
  tx.Exec(
      "UPDATE `PlayerWallets` SET `Balance`=$2,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
      "`PlayerId`=$1 AND `CurrencyCode`='lucci'",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), before - cost});
  tx.Exec(
      "INSERT INTO "
      "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
      " VALUES($1,'lucci',$2,$3,$4,$5,$6)",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), -cost, before,
                                            before - cost, reason, operation});
}

/**
 * @brief Grant one unit of a published grantable template: merge_quantity adds
 * one, a permanent single_instance is granted once. Timed templates are
 * refused.
 */
std::optional<application::ServiceError> Grant(
    bf::infrastructure::mysql::TransactionBase& tx, domain::PlayerId player,
    std::uint32_t item, const char* reason, const std::string& operation,
    std::int64_t amount = 1) {
  const auto templates = tx.Exec(
      "SELECT template.`StackPolicy`,template.`MaxQuantity`,template.`MaxDurationSeconds` FROM "
      "`ItemTemplates` template JOIN `ConfigurationVersions` configuration ON "
      "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
      "template.`ItemId`=$1 AND template.`IsGrantable` AND "
      "configuration.`ReleaseStatus`='published'",
      bf::infrastructure::mysql::Parameters{tx, item});
  if (templates.empty())
    return Error(application::ErrorCode::kConflict,
                 "广场小游戏奖励物品当前不可发放");
  const auto policy = templates[0][0].As<std::string>();
  const auto maximum = templates[0][1].As<std::int64_t>();
  if (!templates[0][2].IsNull())
    return Error(application::ErrorCode::kConflict,
                 "广场小游戏奖励不支持限时物品");
  const auto held = tx.Exec(
      "SELECT `ItemInstanceId`,`Quantity`,(`ExpiresAt` IS NULL OR "
      "`ExpiresAt`>CURRENT_TIMESTAMP(6)) FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 "
      "FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item});
  const bool active = !held.empty() && held[0][2].As<bool>();
  const std::int64_t quantity = active ? held[0][1].As<std::int64_t>() : 0;
  if (policy == "single_instance" && active)
    return std::nullopt;  // Already owned permanently; nothing changes.
  if (policy == "single_instance") amount = 1;
  if (policy == "merge_quantity" && quantity > maximum - amount)
    return Error(application::ErrorCode::kConflict,
                 "广场小游戏奖励数量已达到上限");
  const auto instance =
      tx.Exec(
            "INSERT INTO "
            "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
            "VALUES($1,$2,$3,CURRENT_TIMESTAMP(6),NULL,$4) ON CONFLICT(`PlayerId`,`ItemId`) DO UPDATE "
            "SET `Quantity`=VALUES(`Quantity`),`ExpiresAt`=NULL,`SourceCode`=VALUES(`SourceCode`) "
            "RETURNING `ItemInstanceId`",
            bf::infrastructure::mysql::Parameters{
                tx, player.Value(), item, quantity + amount, reason})[0][0]
          .As<std::int64_t>();
  tx.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
      " VALUES($1,$2,$3,$4,$5,$6)",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item, instance,
                                            amount, reason, operation});
  return std::nullopt;
}

void FishingInventory(bf::infrastructure::mysql::TransactionBase& tx,
                      domain::PlayerId player,
                      const protocol::LobbyMinigameConfig::Fishing& config,
                      runtime::LobbyMinigameRecord& record, bool lock) {
  const auto rows = tx.Exec(
      std::string{
          "SELECT item.`ItemId`,item.`Quantity` FROM `PlayerItems` item JOIN `ItemTemplates` template"
          " ON template.`ItemId`=item.`ItemId` WHERE item.`PlayerId`=$1 AND item.`ItemId` IN ($2,$3) "
          "AND item.`Quantity`>0 AND (item.`ExpiresAt` IS NULL OR "
          "item.`ExpiresAt`>CURRENT_TIMESTAMP(6)) AND template.`StackPolicy`='merge_quantity' ORDER "
          "BY item.`ItemId`"} +
          (lock ? " FOR UPDATE OF item" : ""),
      bf::infrastructure::mysql::Parameters{
          tx, player.Value(), config.premium_rod_item, config.free_pass_item});
  for (const auto& row : rows) {
    const auto item = row[0].As<std::uint32_t>();
    if (item == config.premium_rod_item)
      record.premium_rod_quantity = row[1].As<std::int64_t>();
    if (item == config.free_pass_item)
      record.free_pass_quantity = row[1].As<std::int64_t>();
  }
}

void ConsumeFishingItem(bf::infrastructure::mysql::TransactionBase& tx,
                        domain::PlayerId player, std::uint32_t item,
                        std::int64_t quantity, const std::string& operation) {
  const auto held = tx.Exec(
      "SELECT `ItemInstanceId`,`Quantity` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 "
      "AND `Quantity`>=$3 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item,
                                            quantity});
  if (held.size() != 1U)
    throw std::runtime_error("fishing consumption lost locked inventory");
  const auto instance = held[0][0].As<std::int64_t>();
  tx.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
      " VALUES($1,$2,$3,$4,'lmg_fishing_cost',$5)",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item, instance,
                                            -quantity, operation});
  if (held[0][1].As<std::int64_t>() == quantity)
    tx.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{tx, instance});
  else
    tx.Exec(
        "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-$2 WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{tx, instance, quantity});
}

}  // namespace

application::ServiceResult<runtime::LobbyMinigameRecord>
MySqlLobbyMinigameStore::Load(
    domain::PlayerId player,
    const protocol::LobbyMinigameConfig::Fishing& config) {
  auto acquired = pool_.Acquire();
  if (const auto* failed = std::get_if<application::ServiceError>(&acquired))
    return *failed;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::SnapshotTransaction tx{lease.Connection()};
    const auto day = Today(tx);
    runtime::LobbyMinigameRecord record;
    record.fishing_count = FishingCount(tx, player, day);
    const auto paper = tx.Exec(
        "SELECT `ResultReference` FROM `OperationRequests` WHERE `PlayerId`=$1 AND "
        "`OperationType`='lmg_fortune' AND `IdempotencyKey`=$2",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), day});
    if (!paper.empty())
      record.fortune_paper = std::stoull(paper[0][0].As<std::string>());
    const auto wallet = tx.Exec(
        "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`='lucci'",
        bf::infrastructure::mysql::Parameters{tx, player.Value()});
    record.lucci = wallet.empty() ? 0 : wallet[0][0].As<std::int64_t>();
    FishingInventory(tx, player, config, record, false);
    tx.Commit();
    return record;
  } catch (const std::exception&) {
    return Unavailable();
  }
}

application::ServiceResult<runtime::LobbyMinigameCharge>
MySqlLobbyMinigameStore::CompleteFishing(
    domain::PlayerId player,
    const protocol::LobbyMinigameConfig::Fishing& config,
    const protocol::LobbyFishingTerms& terms, std::uint16_t daily_limit,
    const std::vector<std::uint32_t>& rewards) {
  auto acquired = pool_.Acquire();
  if (const auto* failed = std::get_if<application::ServiceError>(&acquired))
    return *failed;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (!LockPlayer(tx, player))
      return Error(application::ErrorCode::kInvalidState, "玩家尚未完成建角");
    const auto day = Today(tx);
    const auto count = FishingCount(tx, player, day);
    if (count == std::numeric_limits<std::uint16_t>::max() ||
        (daily_limit != 0U && count >= daily_limit))
      return Error(application::ErrorCode::kConflict, "今日钓鱼次数已用完");
    const auto before = LockedLucci(tx, player);
    runtime::LobbyMinigameRecord record{.fishing_count = count,
                                        .lucci = before};
    FishingInventory(tx, player, config, record, true);
    if (terms != runtime::SelectFishingTerms(config, record) ||
        (config.premium_rod_item != 0U &&
         config.premium_rod_item == config.free_pass_item) ||
        rewards.size() > (terms.premium_rod ? 2U : 1U))
      return Error(application::ErrorCode::kInvalidState,
                   "钓鱼库存或计费条件已变化，请重新抛竿");
    const auto price =
        terms.use_free_pass
            ? 0
            : static_cast<std::int64_t>(terms.base_cost) * (count + 1);
    if (before < price || price > std::numeric_limits<std::int32_t>::max())
      return Error(application::ErrorCode::kInsufficientBalance, "卢奇不足");
    const auto coupons = terms.use_free_pass ? (terms.premium_rod ? 2 : 1) : 0;
    const nlohmann::json receipt{{"version", 1},
                                 {"premium_rod", terms.premium_rod},
                                 {"rod_item", config.premium_rod_item},
                                 {"coupon_item", config.free_pass_item},
                                 {"coupons", coupons},
                                 {"base_cost", terms.base_cost},
                                 {"count_before", count},
                                 {"paid", price},
                                 {"lucci_after", before - price},
                                 {"rewards", rewards}};
    const auto operation =
        InsertOperation(tx, player, "lmg_fishing",
                        day + ":" + std::to_string(count + 1), receipt.dump());
    Charge(tx, player, before, price, "lmg_fishing", operation);
    if (terms.premium_rod)
      ConsumeFishingItem(tx, player, config.premium_rod_item, 1, operation);
    if (coupons != 0)
      ConsumeFishingItem(tx, player, config.free_pass_item, coupons, operation);
    std::map<std::uint32_t, std::int64_t> reward_quantities;
    for (const auto item : rewards) ++reward_quantities[item];
    for (const auto& [item, amount] : reward_quantities)
      if (auto refused =
              Grant(tx, player, item, "lmg_fishing", operation, amount))
        return *refused;
    tx.Commit();
    return runtime::LobbyMinigameCharge{
        .fishing_count = static_cast<std::uint16_t>(count + 1U),
        .lucci_after = before - price,
        .paid = price};
  } catch (const std::exception&) {
    return Unavailable();
  }
}

application::ServiceResult<runtime::LobbyMinigameCharge>
MySqlLobbyMinigameStore::ShootBasketball(domain::PlayerId player,
                                         std::int64_t cost,
                                         std::optional<std::uint32_t> reward) {
  auto acquired = pool_.Acquire();
  if (const auto* failed = std::get_if<application::ServiceError>(&acquired))
    return *failed;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (!LockPlayer(tx, player))
      return Error(application::ErrorCode::kInvalidState, "玩家尚未完成建角");
    const auto before = LockedLucci(tx, player);
    if (before < cost)
      return Error(application::ErrorCode::kInsufficientBalance, "卢奇不足");
    if (cost != 0 || reward) {
      // Only charged or rewarded shots leave a durable record; free misses
      // change nothing.
      const auto key = tx.Exec("SELECT UUID()")[0][0].As<std::string>();
      const auto operation =
          InsertOperation(tx, player, "lmg_basketball", key,
                          std::to_string(reward.value_or(0U)));
      Charge(tx, player, before, cost, "lmg_basketball", operation);
      if (reward)
        if (auto refused =
                Grant(tx, player, *reward, "lmg_basketball", operation))
          return *refused;
    }
    tx.Commit();
    return runtime::LobbyMinigameCharge{.lucci_after = before - cost,
                                        .paid = cost};
  } catch (const std::exception&) {
    return Unavailable();
  }
}

application::ServiceResult<runtime::LobbyFortuneOutcome>
MySqlLobbyMinigameStore::Fortune(domain::PlayerId player, std::uint64_t paper,
                                 std::int64_t cost,
                                 std::optional<std::uint32_t> lucky_item) {
  auto acquired = pool_.Acquire();
  if (const auto* failed = std::get_if<application::ServiceError>(&acquired))
    return *failed;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (!LockPlayer(tx, player))
      return Error(application::ErrorCode::kInvalidState, "玩家尚未完成建角");
    const auto day = Today(tx);
    const auto before = LockedLucci(tx, player);
    const auto stored = tx.Exec(
        "SELECT `ResultReference` FROM `OperationRequests` WHERE `PlayerId`=$1 AND "
        "`OperationType`='lmg_fortune' AND `IdempotencyKey`=$2",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), day});
    if (!stored.empty()) {
      tx.Commit();
      return runtime::LobbyFortuneOutcome{
          .created = false,
          .paper = std::stoull(stored[0][0].As<std::string>()),
          .lucci_after = before};
    }
    if (before < cost)
      return Error(application::ErrorCode::kInsufficientBalance, "卢奇不足");
    const auto operation =
        InsertOperation(tx, player, "lmg_fortune", day, std::to_string(paper));
    Charge(tx, player, before, cost, "lmg_fortune", operation);
    if (lucky_item)
      if (auto refused =
              Grant(tx, player, *lucky_item, "lmg_fortune", operation))
        return *refused;
    tx.Commit();
    return runtime::LobbyFortuneOutcome{
        .created = true, .paper = paper, .lucci_after = before - cost};
  } catch (const std::exception&) {
    return Unavailable();
  }
}

}  // namespace bf::infrastructure::mysql
