#include "persistence/mysql/equipment_repository.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/battle/sniper_weapon.h"
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

std::int64_t UnixMicroseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             value.time_since_epoch())
      .count();
}

application::ServiceError MakeSqlError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

class MySqlChangeEquipmentTransaction final
    : public application::ChangeEquipmentTransaction {
 public:
  explicit MySqlChangeEquipmentTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  std::optional<application::ServiceError> SelectPreset(
      domain::PlayerId player_id, std::uint8_t preset,
      std::chrono::system_clock::time_point now,
      std::uint32_t combat_mode) override {
    try {
      const auto profile = transaction_.Exec(
          "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                player_id.Value()});
      if (profile.empty() || preset > 2U) {
        return MakeError(application::ErrorCode::kInvalidInput,
                         "武器预设页不存在");
      }
      if (preset != 0U && !HasExpansion(player_id, now)) {
        return MakeError(application::ErrorCode::kConflict,
                         "需要有效的武器切换扩张卡才能使用设置页");
      }
      if (domain::ModeRequiresSniperPrimary(combat_mode)) {
        // The client disconnects at 7E73A9 if its respawn primary is not
        // SniperRifle. Validate ownership, expiry and category before changing
        // the selected page.
        const auto primary = transaction_.Exec(
            "SELECT item.`ItemId` FROM `PlayerEquipment` AS equipment JOIN `PlayerItems` AS item ON "
            "item.`ItemInstanceId`=equipment.`ItemInstanceId` AND item.`PlayerId`=equipment.`PlayerId` "
            "JOIN `ItemTemplates` AS template ON template.`ItemId`=item.`ItemId` JOIN "
            "`ConfigurationVersions` AS configuration ON "
            "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
            "equipment.`PlayerId`=$1 AND equipment.`LoadoutId`=$2 AND equipment.`SlotCode`='primary' "
            "AND template.`SlotCode`='primary' AND item.`Quantity`>0 AND template.`IsGrantable` AND "
            "configuration.`ReleaseStatus`='published' AND (item.`ExpiresAt` IS NULL OR "
            "item.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE OF item",
            bf::infrastructure::mysql::Parameters{
                transaction_, player_id.Value(),
                static_cast<std::uint32_t>(preset), UnixMicroseconds(now)});
        const auto item =
            primary.size() == 1U ? primary[0][0].As<std::uint64_t>() : 0U;
        if (item <= 200000U || item >= 300000U ||
            !domain::IsNativeSniperWeapon(
                static_cast<std::uint32_t>(item - 200000U))) {
          return MakeError(application::ErrorCode::kConflict,
                           "狙击模式需要装备有效的狙击主武器");
        }
      }
      const auto bit = 1U << preset;
      // 切页只选择对应背包，不能把基本页装备复制到尚未使用的设置页。
      // 保留旧初始化位图的写入以兼容已有库；装备内容只由显式换装修改。
      transaction_.Exec(
          "UPDATE `PlayerProfiles` SET "
          "`ActiveWeaponPreset`=$2,`InitializedWeaponPresets`=`InitializedWeaponPresets` | $3 WHERE "
          "`PlayerId`=$1",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id.Value(),
              static_cast<std::uint32_t>(preset), bit});
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "切换武器预设时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("切换武器预设", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "切换武器预设失败");
    }
  }

  application::ServiceResult<application::ActiveEquipableItem> LockActiveItem(
      domain::PlayerId player_id, domain::ItemId item_id,
      std::chrono::system_clock::time_point now) override {
    try {
      // 不同宠物实例也必须串行换装，避免同时写入两个历史 usable 槽位。
      const auto profile = transaction_.Exec(
          "SELECT `ActiveWeaponPreset` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                player_id.Value()});
      active_preset_ = !profile.empty() && HasExpansion(player_id, now)
                           ? profile[0][0].As<std::uint32_t>()
                           : 0U;
      now_ = now;
      const auto rows = transaction_.Exec(
          "SELECT "
          "item.`ItemInstanceId`,item.`ItemId`,`EquipmentSlot`(template.`ItemId`,template.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type')))"
          " FROM `PlayerItems` AS item JOIN `ItemTemplates` AS template ON "
          "template.`ItemId`=item.`ItemId` JOIN `ConfigurationVersions` AS configuration ON "
          "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
          "item.`PlayerId`=$1 AND item.`ItemId`=$2 AND template.`SlotCode` IS NOT NULL AND "
          "item.`Quantity`>0 AND template.`IsGrantable` AND configuration.`ReleaseStatus`='published'"
          " AND (item.`ExpiresAt` IS NULL OR item.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE OF item",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_id.Value(),
                                                UnixMicroseconds(now)});
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kConflict,
                         "物品不存在、已过期或不可装备");
      }
      return application::ActiveEquipableItem{
          .item_instance_id =
              domain::ItemInstanceId{rows[0][0].As<std::uint64_t>()},
          .item_id = domain::ItemId{rows[0][1].As<std::uint64_t>()},
          .slot_code = rows[0][2].As<std::string>()};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定换装库存时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("锁定换装库存", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定换装库存时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Equip(
      domain::PlayerId player_id, std::uint32_t loadout_id,
      std::string_view slot_code,
      domain::ItemInstanceId item_instance_id) override {
    try {
      if (slot_code == "secondary" || slot_code == "melee") {
        loadout_id = 0U;
      } else if (loadout_id == 0U &&
                 (slot_code == "primary" || slot_code == "grenade" ||
                  slot_code == "crystal")) {
        loadout_id = active_preset_;
      }
      if (slot_code == "pet") {
        // 兼容旧发布目录按 item_id 分出的槽位；不修改不可变目录或删除库存。
        transaction_.Exec(
            "DELETE equipment FROM `PlayerEquipment` AS equipment, `PlayerItems` AS "
            "item,`ItemTemplates` AS template WHERE equipment.`PlayerId`=$1 AND "
            "equipment.`LoadoutId`=$2 AND item.`ItemInstanceId`=equipment.`ItemInstanceId` AND "
            "template.`ItemId`=item.`ItemId` AND (equipment.`SlotCode`='pet' OR "
            "JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type'))='10001')",
            bf::infrastructure::mysql::Parameters{
                transaction_, player_id.Value(), loadout_id});
      }
      const auto compatibility = transaction_.Exec(
          "SELECT "
          "`ClothingCharacterCompatible`(`ItemId`,`EquippedCharacter`($1,FROM_UNIXTIME($3/1000000.0)))"
          " FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_instance_id.Value(),
                                                UnixMicroseconds(now_)});
      if (compatibility.size() != 1U || !compatibility[0][0].As<bool>()) {
        return MakeError(application::ErrorCode::kConflict,
                         "该服饰不支持当前角色");
      }
      // 原版 D11A10 按主占用部位替换整件服饰，不能只按商城分类替换。
      // 例如连体衣与上衣分属不同分类，但都占据身体和手臂；次级外观不互斥。
      // 饰品也按原槽位掩码交集替换；围巾占背部与腰后，不能同时保留腰后辅助推进器。
      transaction_.Exec(
          "DELETE equipment FROM `PlayerEquipment` AS equipment, `PlayerItems` AS "
          "current,`PlayerItems` AS incoming WHERE equipment.`PlayerId`=$1 AND "
          "equipment.`LoadoutId`=$2 AND current.`ItemInstanceId`=equipment.`ItemInstanceId` AND "
          "incoming.`PlayerId`=$1 AND incoming.`ItemInstanceId`=$3 AND "
          "((`ClothingMask`(current.`ItemId`) & `ClothingMask`(incoming.`ItemId`))<>0 OR "
          "(`AccessoryMask`(current.`ItemId`) & `AccessoryMask`(incoming.`ItemId`))<>0)",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                loadout_id,
                                                item_instance_id.Value()});
      // 先移除同一实例在该配装页上的旧槽位，再替换目标槽；两步同事务，外部不可观察中间态。
      transaction_.Exec(
          "DELETE FROM `PlayerEquipment` WHERE `PlayerId`=$1 AND `LoadoutId`=$2 AND "
          "`ItemInstanceId`=$3",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                loadout_id,
                                                item_instance_id.Value()});
      transaction_.Exec(
          "INSERT INTO "
          "`PlayerEquipment`(`PlayerId`,`LoadoutId`,`SlotCode`,`ItemInstanceId`,`EquippedAt`) "
          "VALUES($1,$2,$3,$4,CURRENT_TIMESTAMP(6)) ON CONFLICT(`PlayerId`,`LoadoutId`,`SlotCode`) DO"
          " UPDATE SET `ItemInstanceId`=VALUES(`ItemInstanceId`),`EquippedAt`=VALUES(`EquippedAt`)",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                loadout_id, slot_code,
                                                item_instance_id.Value()});
      if (slot_code == "character") {
        RemoveIncompatibleClothes(player_id);
      }
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "保存换装时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("保存换装", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "保存换装时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> UnequipIfCurrent(
      domain::PlayerId player_id, std::uint32_t loadout_id,
      std::string_view slot_code,
      domain::ItemInstanceId item_instance_id) override {
    try {
      if (slot_code == "secondary" || slot_code == "melee") {
        loadout_id = 0U;
      } else if (loadout_id == 0U &&
                 (slot_code == "primary" || slot_code == "grenade" ||
                  slot_code == "crystal")) {
        loadout_id = active_preset_;
      }
      transaction_.Exec(
          "DELETE FROM `PlayerEquipment` WHERE `PlayerId`=$1 AND `LoadoutId`=$2 AND (`SlotCode`=$3 OR"
          " $3='pet') AND `ItemInstanceId`=$4",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                loadout_id, slot_code,
                                                item_instance_id.Value()});
      if (slot_code == "character") {
        RemoveIncompatibleClothes(player_id);
      }
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "卸下装备时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("卸下装备", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "卸下装备时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState, "换装事务已结束");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交换装时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      completed_ = true;
      return MakeSqlError("提交换装", error);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交换装时发生未分类错误");
    }
  }

 private:
  void RemoveIncompatibleClothes(domain::PlayerId player_id) {
    transaction_.Exec(
        "DELETE e FROM `PlayerEquipment` e, `PlayerItems` i WHERE e.`PlayerId`=$1 AND "
        "i.`PlayerId`=e.`PlayerId` AND i.`ItemInstanceId`=e.`ItemInstanceId` AND NOT "
        "`ClothingCharacterCompatible`(i.`ItemId`,`EquippedCharacter`($1,FROM_UNIXTIME($2/1000000.0)))",
        bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                              UnixMicroseconds(now_)});
  }

  bool HasExpansion(domain::PlayerId player_id,
                    std::chrono::system_clock::time_point now) {
    const auto owned = transaction_.Exec(
        "SELECT 1 FROM `PlayerItems` item JOIN `ItemTemplates` template ON "
        "template.`ItemId`=item.`ItemId` WHERE item.`PlayerId`=$1 AND item.`ItemId`=500001 AND "
        "item.`Quantity`>0 AND template.`IsGrantable` AND (item.`ExpiresAt` IS NULL OR "
        "item.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0))",
        bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                              UnixMicroseconds(now)});
    return !owned.empty();
  }

  std::uint32_t active_preset_{0U};
  std::chrono::system_clock::time_point now_{};
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool completed_{false};
};

}  // namespace

MySqlChangeEquipmentUnitOfWork::MySqlChangeEquipmentUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<
    std::unique_ptr<application::ChangeEquipmentTransaction>>
MySqlChangeEquipmentUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::ChangeEquipmentTransaction>{
        std::make_unique<MySqlChangeEquipmentTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始换装事务", true);
  }
}

}  // namespace bf::infrastructure::mysql
