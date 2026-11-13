#include "persistence/mysql/character_creation_repository.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/characters/character.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = retryable};
}

application::ServiceError StorageError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

class MySqlCharacterCreationTransaction final
    : public application::CharacterCreationTransaction {
 public:
  explicit MySqlCharacterCreationTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::CharacterCreationProfileSnapshot>
  LockProfile(domain::PlayerId player_id) override {
    try {
      const auto rows = transaction_.Exec(
          "SELECT `PlayerId`,`IsCharacterCreated`,`Nickname`,`InitialFighterId` FROM `PlayerProfiles`"
          " WHERE `PlayerId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                player_id.Value()});
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "待建角玩家资料不存在");
      }
      std::optional<domain::InitialFighter> fighter;
      if (!rows[0][3].IsNull()) {
        fighter =
            domain::InitialFighterFromProtocol(rows[0][3].As<std::uint32_t>());
        if (!fighter.has_value()) {
          return MakeError(application::ErrorCode::kInternal,
                           "玩家资料包含未发布的初始角色");
        }
      }
      return application::CharacterCreationProfileSnapshot{
          .player_id = domain::PlayerId{rows[0][0].As<std::uint64_t>()},
          .is_character_created = rows[0][1].As<bool>(),
          .nickname = rows[0][2].As<std::string>(),
          .initial_fighter = fighter};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定玩家建角资料时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("锁定玩家建角资料", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定玩家建角资料时发生未分类错误");
    }
  }

  application::ServiceResult<bool> NicknameKeyInUse(
      std::string_view nickname_key,
      domain::PlayerId excluding_player_id) override {
    try {
      const auto rows = transaction_.Exec(
          "SELECT EXISTS(SELECT 1 FROM `PlayerProfiles` WHERE `NicknameKey`=$1 AND `PlayerId`<>$2)",
          bf::infrastructure::mysql::Parameters{transaction_, nickname_key,
                                                excluding_player_id.Value()});
      return rows[0][0].As<bool>();
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "检查昵称唯一性时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("检查昵称唯一性", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "检查昵称唯一性时发生未分类错误");
    }
  }

  application::ServiceResult<application::StarterCatalogItem>
  LockEnabledStarterItem(domain::ItemId item_id) override {
    try {
      const auto rows = transaction_.Exec(
          "SELECT template.`ItemId`,template.`SlotCode` FROM `ItemTemplates` AS template JOIN "
          "`ConfigurationVersions` AS configuration ON "
          "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
          "template.`ItemId`=$1 AND template.`IsGrantable` AND "
          "configuration.`ReleaseStatus`='published' FOR SHARE OF template",
          bf::infrastructure::mysql::Parameters{transaction_, item_id.Value()});
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "初始物品未在已发布目录中启用");
      }
      std::optional<std::string> slot_code;
      if (!rows[0][1].IsNull()) {
        slot_code = rows[0][1].As<std::string>();
      }
      return application::StarterCatalogItem{
          .item_id = domain::ItemId{rows[0][0].As<std::uint64_t>()},
          .slot_code = std::move(slot_code)};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取初始物品目录时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("读取初始物品目录", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取初始物品目录时发生未分类错误");
    }
  }

  application::ServiceResult<domain::ItemInstanceId> EnsureStarterItem(
      domain::PlayerId player_id, domain::ItemId item_id) override {
    try {
      const auto existing = transaction_.Exec(
          "SELECT `ItemInstanceId` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_id.Value()});
      if (!existing.empty()) {
        return domain::ItemInstanceId{existing[0][0].As<std::uint64_t>()};
      }
      const auto inserted = transaction_.Exec(
          "INSERT INTO "
          "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
          "VALUES($1,$2,1,CURRENT_TIMESTAMP(6),NULL,'starter') RETURNING `ItemInstanceId`",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_id.Value()});
      const auto instance_id = inserted[0][0].As<std::uint64_t>();
      transaction_.Exec(
          "INSERT INTO "
          "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`)"
          " "
          "VALUES($1,$2,$3,1,NULL,NULL,'starter',BIN_TO_UUID(UNHEX(MD5(CONCAT('starter:',$1,':',$2)))))",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_id.Value(), instance_id});
      return domain::ItemInstanceId{instance_id};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "发放初始物品时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("发放初始物品", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "发放初始物品时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> EquipItem(
      domain::PlayerId player_id, std::string_view slot_code,
      domain::ItemInstanceId item_instance_id) override {
    try {
      transaction_.Exec(
          "INSERT INTO "
          "`PlayerEquipment`(`PlayerId`,`LoadoutId`,`SlotCode`,`ItemInstanceId`,`EquippedAt`) "
          "VALUES($1,0,$2,$3,CURRENT_TIMESTAMP(6)) ON CONFLICT(`PlayerId`,`LoadoutId`,`SlotCode`) DO "
          "UPDATE SET `ItemInstanceId`=VALUES(`ItemInstanceId`),`EquippedAt`=VALUES(`EquippedAt`)",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                slot_code,
                                                item_instance_id.Value()});
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "保存初始配装时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("保存初始配装", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "保存初始配装时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> CompleteProfile(
      domain::PlayerId player_id, const domain::Nickname& nickname,
      domain::InitialFighter fighter) override {
    try {
      const auto changed = transaction_.Exec(
          "UPDATE `PlayerProfiles` SET "
          "`Nickname`=$2,`NicknameKey`=$3,`IsCharacterCreated`=true,`InitialFighterId`=$4 WHERE "
          "`PlayerId`=$1 AND NOT `IsCharacterCreated` RETURNING `PlayerId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id.Value(), nickname.DisplayName(),
              nickname.Key(), static_cast<std::uint32_t>(fighter)});
      if (changed.size() != 1U) {
        return MakeError(application::ErrorCode::kConflict,
                         "玩家角色已由另一个请求完成创建");
      }
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::UniqueViolation&) {
      return MakeError(application::ErrorCode::kConflict,
                       "该昵称已被使用，请换一个昵称");
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "完成玩家资料时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("完成玩家资料", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "完成玩家资料时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "角色创建事务已结束");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交角色创建时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      completed_ = true;
      return StorageError("提交角色创建", error);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交角色创建时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool completed_{false};
};

}  // namespace

MySqlCharacterCreationUnitOfWork::MySqlCharacterCreationUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<
    std::unique_ptr<application::CharacterCreationTransaction>>
MySqlCharacterCreationUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::CharacterCreationTransaction>{
        std::make_unique<MySqlCharacterCreationTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始角色创建事务", true);
  }
}

}  // namespace bf::infrastructure::mysql
