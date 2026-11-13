#include <spdlog/spdlog.h>

#include <exception>
#include <limits>

#include "persistence/mysql/item_use_repository.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError DiscardError(application::ErrorCode code,
                                       std::string message,
                                       bool retry = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retry};
}
}  // namespace

application::ServiceResult<application::ExpiredItemDiscardResult>
MySqlItemUseRepository::DiscardExpired(
    const application::ExpiredItemDiscardCommand& c,
    std::chrono::system_clock::time_point now) {
  if (!c.player_id.IsValid() || c.native_item_key == 0U ||
      c.native_item_key > 2'147'483'647U ||
      (c.native_item_key >= 301'000U && c.native_item_key < 400'000U))
    return DiscardError(application::ErrorCode::kInvalidInput,
                        "原过期删弃键无效或是宠物");
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch())
                          .count();
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto owner = tx.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value()});
    if (owner.empty())
      return DiscardError(application::ErrorCode::kPermissionDenied,
                          "过期物品不属于已认证角色");
    const auto bound = tx.Exec(
        "SELECT `PlayerId`,`NativeItemKey` FROM `InventoryDiscards` WHERE `DiscardId`=$1",
        bf::infrastructure::mysql::Parameters{tx,
                                              c.operation_id.ToUuidString()});
    if (!bound.empty() &&
        (bound[0][0].As<std::uint64_t>() != c.player_id.Value() ||
         bound[0][1].As<std::uint32_t>() != c.native_item_key))
      return DiscardError(application::ErrorCode::kConflict,
                          "删弃UUID不能迁移到其他主人或原库存键");
    const auto row = tx.Exec(
        "SELECT `ItemInstanceId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`ExpiresAt` IS NOT NULL AND "
        "`ExpiresAt`<=FROM_UNIXTIME($3/1000000.0) FROM `PlayerItems` WHERE `PlayerId`=$1 AND "
        "`ItemId`=$2 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value(),
                                              c.native_item_key, now_us});
    // Inspect live ownership BEFORE an old receipt: repurchasing the same
    // native template key must never receive an old813 success which would
    // remove its fresh client inventory row.
    if (row.empty()) {
      const auto previous = tx.Exec(
          "SELECT `ItemInstanceId`,`RemovedQuantity` FROM `InventoryDiscards` WHERE `PlayerId`=$1 AND"
          " `NativeItemKey`=$2 ORDER BY `DiscardedAt` DESC,`DiscardId` DESC LIMIT 1",
          bf::infrastructure::mysql::Parameters{tx, c.player_id.Value(),
                                                c.native_item_key});
      if (previous.empty())
        return DiscardError(application::ErrorCode::kConflict,
                            "本人没有这个原库存键或过期删弃回执");
      tx.Commit();
      return application::ExpiredItemDiscardResult{
          .native_item_key = c.native_item_key,
          .item_instance_id =
              domain::ItemInstanceId{previous[0][0].As<std::uint64_t>()},
          .removed_quantity = previous[0][1].As<std::uint32_t>(),
          .replay = true};
    }
    if (row.size() != 1U || !row[0][4].As<bool>())
      return DiscardError(application::ErrorCode::kConflict,
                          "未过期或永久物品不能删弃");
    const auto instance = row[0][0].As<std::uint64_t>();
    const auto quantity = row[0][1].As<std::uint64_t>();
    if (quantity == 0U || quantity > 2'147'483'647U)
      return DiscardError(application::ErrorCode::kConflict,
                          "过期库存数量超出原生审计范围");
    const auto reused = tx.Exec(
        "SELECT `DiscardId` FROM `InventoryDiscards` WHERE `DiscardId`=$1",
        bf::infrastructure::mysql::Parameters{tx,
                                              c.operation_id.ToUuidString()});
    if (!reused.empty())
      return DiscardError(application::ErrorCode::kConflict,
                          "删弃操作UUID不能迁移到新实例");
    static_cast<void>(tx.Exec(
        "INSERT INTO "
        "`InventoryDiscards`(`DiscardId`,`PlayerId`,`NativeItemKey`,`ItemInstanceId`,`RemovedQuantity`,`AcquiredAt`,`ExpiresAt`,`DiscardedAt`)"
        " SELECT "
        "$1,`PlayerId`,`ItemId`,`ItemInstanceId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,FROM_UNIXTIME($3/1000000.0)"
        " FROM `PlayerItems` WHERE `ItemInstanceId`=$2 AND `PlayerId`=$4 AND `ItemId`=$5",
        bf::infrastructure::mysql::Parameters{
            tx, c.operation_id.ToUuidString(), instance, now_us,
            c.player_id.Value(), c.native_item_key}));
    static_cast<void>(tx.Exec(
        "DELETE FROM `NativeTitleEquipment` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value(),
                                              instance}));
    static_cast<void>(tx.Exec(
        "DELETE FROM `PlayerEquipment` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value(),
                                              instance}));
    static_cast<void>(tx.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ReasonCode`,`BusinessId`)"
        " SELECT `PlayerId`,`ItemId`,NULL,-`Quantity`,`ExpiresAt`,'expired_discard',$2 FROM "
        "`PlayerItems` WHERE `ItemInstanceId`=$1 AND `PlayerId`=$3",
        bf::infrastructure::mysql::Parameters{
            tx, instance, c.operation_id.ToUuidString(), c.player_id.Value()}));
    const auto erased = tx.Exec(
        "DELETE FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 AND `ItemInstanceId`=$3 AND "
        "`Quantity`=$4 AND `ExpiresAt`<=FROM_UNIXTIME($5/1000000.0)",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value(),
                                              c.native_item_key, instance,
                                              quantity, now_us});
    if (erased.AffectedRows() != 1U)
      return DiscardError(application::ErrorCode::kConflict,
                          "精确过期实例没有完成删除，事务已回滚");
    tx.Commit();
    return application::ExpiredItemDiscardResult{
        .native_item_key = c.native_item_key,
        .item_instance_id = domain::ItemInstanceId{instance},
        .removed_quantity = static_cast<std::uint32_t>(quantity),
        .replay = false};
  } catch (const std::exception& error) {
    spdlog::warn("expired discard transaction unconfirmed: {}", error.what());
    return DiscardError(application::ErrorCode::kStorageUnavailable,
                        "过期删弃事务未确认，请刷新仓库", true);
  }
}
}  // namespace bf::infrastructure::mysql
