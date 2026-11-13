/** @file life_item_repository.cpp
 * @brief 原版无目标生命注射器：锁定本人全部 Life，逐项补一并原子扣一支。
 */
#include <spdlog/spdlog.h>

#include <exception>
#include <string>
#include <vector>

#include "features/inventory/life_item.h"
#include "persistence/mysql/item_use_repository.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError LifeError(application::ErrorCode code,
                                    std::string message) {
  return {
      .code = code, .safe_message = std::move(message), .is_retryable = false};
}

std::string LifeInventoryKeys() {
  std::string keys = "[" + std::to_string(domain::kLifeSyringe);
  for (const auto id : domain::kLifeItemIds) keys += "," + std::to_string(id);
  return keys + "]";
}
}  // namespace

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::ReplenishLife(
    domain::PlayerId player, std::chrono::system_clock::time_point now) {
  if (!player.IsValid())
    return LifeError(application::ErrorCode::kInvalidInput,
                     "生命注射器身份无效");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (tx.Exec(
              "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
              "UPDATE",
              bf::infrastructure::mysql::Parameters{tx, player.Value()})
            .empty())
      return LifeError(application::ErrorCode::kPermissionDenied,
                       "角色尚未创建");
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
    // Freeze both owned instances and their published definitions. Checking the
    // immutable payload prevents a changed ordinary stack type or modified cap
    // from granting free items.
    const auto rows = tx.Exec(
        "SELECT i.ItemInstanceId,i.ItemId,i.Quantity,i.ExpiresAt IS "
        "NULL,JSON_UNQUOTE(JSON_EXTRACT(t.ClientFields,'$.merge_type')), t.MaxQuantity, "
        "v.ConfigurationKind='catalog' AND v.ReleaseStatus='published' AND t.IsGrantable AND "
        "t.StackPolicy='merge_quantity' AND t.DurationPolicy='permanent' AND t.MaxDurationSeconds "
        "IS NULL AND JSON_UNQUOTE(JSON_EXTRACT(t.ClientFields,'$.merge_limit'))=t.MaxQuantity AND "
        "JSON_CONTAINS(v.Payload,JSON_OBJECT('item_id',t.ItemId,'client_fields',t.ClientFields,'max_quantity',t.MaxQuantity,'duration_policy',t.DurationPolicy,'stack_policy',t.StackPolicy,'is_grantable',JSON_EXTRACT('true','$')),'$.item_templates'),"
        " JSON_UNQUOTE(JSON_EXTRACT(t.ClientFields,'$.use_type')), EXISTS(SELECT 1 FROM "
        "PlayerEquipment e WHERE e.PlayerId=i.PlayerId AND e.ItemInstanceId=i.ItemInstanceId) FROM "
        "PlayerItems i JOIN ItemTemplates t USING(ItemId) JOIN ConfigurationVersions v "
        "USING(ConfigurationVersionId) WHERE i.PlayerId=$1 AND i.ItemId IN (SELECT ids.ItemId FROM "
        "JSON_TABLE($2,'$[*]' COLUMNS (ItemId BIGINT PATH '$')) ids) ORDER BY i.ItemId FOR UPDATE OF i FOR SHARE OF t,v",
        bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                              LifeInventoryKeys()});
    std::uint64_t syringe_instance = 0U;
    std::int64_t syringe_quantity = 0;
    bool equipment_restored = false;

    struct Target {
      std::uint64_t instance;
      std::uint64_t item;
    };

    std::vector<Target> targets;
    for (const auto& row : rows) {
      const auto item = row[1].As<std::uint64_t>();
      const auto quantity = row[2].As<std::int64_t>();
      const bool syringe = item == domain::kLifeSyringe;
      const auto expected_cap =
          syringe ? 30000 : static_cast<int>(domain::kLifeMaximum);
      if (!row[3].As<bool>() || !row[6].As<bool>(false) ||
          row[4].As<std::string>("") != (syringe ? "1" : "3") ||
          row[5].As<std::int64_t>() != expected_cap ||
          quantity < (syringe ? 1 : 0) || quantity > expected_cap ||
          (syringe && row[7].As<std::string>("") != "18"))
        return LifeError(application::ErrorCode::kConflict,
                         "Life库存或发布目录不一致");
      if (syringe) {
        syringe_instance = row[0].As<std::uint64_t>();
        syringe_quantity = quantity;
      } else if (quantity < expected_cap) {
        targets.push_back({row[0].As<std::uint64_t>(), item});
        equipment_restored =
            equipment_restored || (quantity == 0 && row[8].As<bool>());
      }
    }
    if (syringe_instance == 0U)
      return LifeError(application::ErrorCode::kInsufficientBalance,
                       "生命注射器不足");
    if (targets.empty())
      return LifeError(application::ErrorCode::kInvalidState,
                       "没有可以补充的Life道具");
    const auto business = tx.Exec("SELECT UUID()")[0][0].As<std::string>();
    for (const auto& target : targets) {
      const auto updated = tx.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=`Quantity`+1 WHERE `PlayerId`=$1 AND "
          "`ItemInstanceId`=$2 AND `ItemId`=$3 AND `Quantity` BETWEEN 0 AND 14",
          bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                target.instance, target.item});
      if (updated.AffectedRows() != 1U)
        return LifeError(application::ErrorCode::kConflict,
                         "Life实例发生变化，事务已回滚");
      static_cast<void>(tx.Exec(
          "INSERT INTO "
          "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
          " VALUES($1,$2,$3,1,'life_replenish',$4,FROM_UNIXTIME($5/1000000.0))",
          bf::infrastructure::mysql::Parameters{tx, player.Value(), target.item,
                                                target.instance, business,
                                                now_us}));
    }
    // NULL deliberately: deleting the last syringe must not need to rewrite
    // immutable history.
    static_cast<void>(tx.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,190144,NULL,-1,'life_syringe',$2,FROM_UNIXTIME($3/1000000.0))",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), business,
                                              now_us}));
    const auto consumed =
        syringe_quantity == 1
            ? tx.Exec(
                  "DELETE FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2 AND `Quantity`=$3",
                  bf::infrastructure::mysql::Parameters{
                      tx, player.Value(), syringe_instance, syringe_quantity})
            : tx.Exec(
                  "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `PlayerId`=$1 AND "
                  "`ItemInstanceId`=$2 AND `Quantity`=$3",
                  bf::infrastructure::mysql::Parameters{
                      tx, player.Value(), syringe_instance, syringe_quantity});
    if (consumed.AffectedRows() != 1U)
      return LifeError(application::ErrorCode::kConflict,
                       "生命注射器未完成消耗，事务已回滚");
    tx.Commit();
    return application::UseItemResult{.consumed = true,
                                      .equipment_restored = equipment_restored};
  } catch (const std::exception& error) {
    spdlog::warn("life syringe transaction unconfirmed: {}", error.what());
    // C2S806 has no stable request id: never auto-retry an unknown COMMIT
    // outcome.
    return LifeError(application::ErrorCode::kStorageUnavailable,
                     "Life事务未确认，请刷新仓库，不要重复使用注射器");
  }
}
}  // namespace bf::infrastructure::mysql
