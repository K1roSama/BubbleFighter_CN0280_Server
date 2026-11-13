#include "persistence/mysql/item_use_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

#include "features/characters/name_policy.h"
#include "persistence/mysql/room_lease_repository.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError Failure(application::ErrorCode code,
                                  std::string message) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = false};
}
}  // namespace

MySqlItemUseRepository::MySqlItemUseRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::Rename(domain::PlayerId player,
                               const domain::Nickname& nickname,
                               std::chrono::system_clock::time_point now) {
  if (!player.IsValid() || domain::IsRestrictedName(nickname.DisplayName())) {
    return Failure(application::ErrorCode::kInvalidInput,
                   "昵称包含禁用词或保留名称");
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto profile = transaction.Exec(
        "SELECT `Nickname` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (profile.empty()) {
      return Failure(application::ErrorCode::kPermissionDenied, "角色尚未创建");
    }
    // Requests have no operation UUID. An immediate replay, including after the
    // last card was deleted, is an idempotent success under the player's
    // profile lock.
    if (profile[0][0].As<std::string>() == nickname.DisplayName()) {
      transaction.Commit();
      return application::UseItemResult{.consumed = false};
    }
    if (!transaction
             .Exec(
                 "SELECT 1 FROM `PlayerProfiles` WHERE `NicknameKey`=$1 AND `PlayerId`<>$2",
                 bf::infrastructure::mysql::Parameters{
                     transaction, nickname.Key(), player.Value()})
             .empty()) {
      return Failure(application::ErrorCode::kConflict,
                     "该昵称已被使用，请换一个昵称");
    }
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
    const auto inventory = transaction.Exec(
        "SELECT owned.`ItemInstanceId`,owned.`Quantity` FROM `PlayerItems` owned JOIN "
        "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` JOIN `ConfigurationVersions` "
        "version ON version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` WHERE "
        "owned.`PlayerId`=$1 AND owned.`ItemId`=100104 AND owned.`Quantity`>0 AND "
        "`Kind`.`IsGrantable` AND "
        "JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))='3' AND "
        "version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS NULL OR "
        "owned.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0)) ORDER BY (owned.`ExpiresAt` IS NULL), "
        "owned.`ExpiresAt`,owned.`ItemInstanceId` LIMIT 1 FOR UPDATE OF owned",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              now_us});
    if (inventory.empty()) {
      return Failure(application::ErrorCode::kInsufficientBalance,
                     "改名卡不足或已过期");
    }
    // The database unique constraint arbitrates two players racing for the same
    // name. Any failure rolls back both the profile and inventory changes.
    transaction.Exec(
        "UPDATE `PlayerProfiles` SET `Nickname`=$2,`NicknameKey`=$3 WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              nickname.DisplayName(),
                                              nickname.Key()});
    const auto instance = inventory[0][0].As<std::uint64_t>();
    if (inventory[0][1].As<std::uint32_t>() == 1U) {
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
    transaction.Exec(
        "INSERT INTO `InventoryTransactions` "
        "(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`) "
        "VALUES($1,100104,NULL,-1,'character_rename',UUID())",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    transaction.Commit();
    return application::UseItemResult{.consumed = true};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return Failure(application::ErrorCode::kConflict,
                   "该昵称已被使用，请换一个昵称");
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("角色改名 SQL 失败，sqlstate={}", error.Sqlstate());
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "角色改名事务失败");
  } catch (const std::exception&) {
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "角色改名数据库暂不可用");
  }
}

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::ExpandClan(domain::PlayerId player,
                                   std::chrono::system_clock::time_point now) {
  if (!player.IsValid())
    return Failure(application::ErrorCode::kInvalidInput, "家族扩容身份无效");
  // create() takes profile before membership; older clan mutations take
  // clan/member first. Retry only a MySQL-confirmed pre-commit deadlock, after
  // work/lease destruction has rolled back the entire attempt. Never retry a
  // broken connection or unknown COMMIT result.
  for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
    auto acquired = pool_.Acquire();
    if (const auto* error = std::get_if<application::ServiceError>(&acquired))
      return *error;
    bool commit_started = false;
    try {
      auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
      bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
      // Match the existing clan-management lock order. Leadership/status cannot
      // change midway.
      const auto clan = tx.Exec(
          "SELECT c.`ClanId`,c.`Capacity` FROM `ClanMemberships` m JOIN `Clans` c USING(`ClanId`) "
          "WHERE m.`PlayerId`=$1 AND m.`RoleCode`='master' AND c.`LeaderPlayerId`=$1 AND "
          "c.`ClanKind`='standard' AND c.`ClanStatus`='active' FOR UPDATE OF c,m",
          bf::infrastructure::mysql::Parameters{tx, player.Value()});
      if (clan.empty())
        return Failure(application::ErrorCode::kPermissionDenied,
                       "仅正常家族的会长可以使用扩张券");
      if (clan[0][1].As<std::uint32_t>() > 90U)
        return Failure(application::ErrorCode::kConflict,
                       "家族人数已达到原版扩张上限一百人");
      if (tx.Exec(
                "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
                "UPDATE",
                bf::infrastructure::mysql::Parameters{tx, player.Value()})
              .empty())
        return Failure(application::ErrorCode::kPermissionDenied,
                       "角色尚未创建");
      const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              now.time_since_epoch())
                              .count();
      const auto card = tx.Exec(
          "SELECT i.`ItemInstanceId`,i.`Quantity` FROM `PlayerItems` i JOIN `ItemTemplates` t "
          "USING(`ItemId`) JOIN `ConfigurationVersions` v ON "
          "v.`ConfigurationVersionId`=t.`ConfigurationVersionId` WHERE i.`PlayerId`=$1 AND "
          "i.`ItemId`=101062 AND i.`Quantity`>0 AND t.`IsGrantable` AND "
          "JSON_UNQUOTE(JSON_EXTRACT(t.`ClientFields`,'$.use_type'))='24' AND "
          "t.`StackPolicy`='merge_quantity' AND v.`ReleaseStatus`='published' AND (i.`ExpiresAt` IS "
          "NULL OR i.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0)) FOR UPDATE OF i FOR SHARE OF t,v",
          bf::infrastructure::mysql::Parameters{tx, player.Value(), now_us});
      if (card.empty())
        return Failure(application::ErrorCode::kConflict,
                       "家族扩张券不足、已过期或目录已停用");
      const auto instance = card[0][0].As<std::uint64_t>();
      const bool remove = card[0][1].As<std::int64_t>() == 1;
      if (remove) {
        tx.Exec("DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
                bf::infrastructure::mysql::Parameters{tx, instance});
        tx.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
                bf::infrastructure::mysql::Parameters{tx, instance});
      } else {
        tx.Exec(
            "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{tx, instance});
      }
      tx.Exec("UPDATE `Clans` SET `Capacity`=`Capacity`+10 WHERE `ClanId`=$1",
              bf::infrastructure::mysql::Parameters{
                  tx, clan[0][0].As<std::uint64_t>()});
      tx.Exec(
          "INSERT INTO "
          "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
          " VALUES($1,101062,$2,-1,'clan_expand',UUID())",
          bf::infrastructure::mysql::Parameters{
              tx, player.Value(),
              remove ? std::nullopt : std::optional{instance}});
      commit_started = true;
      tx.Commit();
      // Native 806 has no request ID: each valid request uses a card, not
      // cross-connection replay.
      return application::UseItemResult{.consumed = true};
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      if (!commit_started && (error.Code() == 1213U || error.Code() == 1205U) &&
          attempt + 1U < 3U) {
        spdlog::warn("家族扩容死锁已整笔回滚，重试第{}次", attempt + 1U);
        std::this_thread::sleep_for(
            std::chrono::milliseconds{5U * (attempt + 1U)});
        continue;
      }
      spdlog::error("家族扩容 SQL 失败，sqlstate={}，detail={}",
                    error.Sqlstate(), error.what());
    } catch (const std::exception&) {
      spdlog::error("家族扩容事务无法完成");
    }
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "家族扩容事务暂不可用");
  }
  return Failure(application::ErrorCode::kStorageUnavailable,
                 "家族扩容死锁重试已达到上限");
}

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::InstantRespawn(
    const application::UseItemCommand& command,
    std::chrono::system_clock::time_point now) {
  if (!command.match_id || command.item_id.Value() != 100443U ||
      command.quantity != 1U || command.life_generation == 0U ||
      command.life_generation > static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max())) {
    return Failure(application::ErrorCode::kInvalidInput, "即时复活凭据无效");
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto player = command.player_id.Value();
    const auto match = command.match_id->ToUuidString();
    const auto life = command.life_generation;
    const bool replay =
        !transaction
             .Exec(
                 "SELECT 1 FROM `InstantRespawnUses` WHERE `MatchId`=$1 AND `PlayerId`=$2 AND "
                 "`LifeGeneration`=$3",
                 bf::infrastructure::mysql::Parameters{transaction, match,
                                                       player, life})
             .empty();
    if (!LockMatchOwnership(transaction, *command.match_id, command.room_owner,
                            !replay))
      return Failure(application::ErrorCode::kConflict,
                     "instant respawn room ownership expired or mismatched");
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
    if (transaction
            .Exec(
                "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
                "UPDATE",
                bf::infrastructure::mysql::Parameters{transaction, player})
            .empty()) {
      return Failure(application::ErrorCode::kPermissionDenied, "角色尚未创建");
    }
    if (!transaction
             .Exec(
                 "SELECT 1 FROM `InstantRespawnUses` WHERE `MatchId`=$1 AND `PlayerId`=$2 AND "
                 "`LifeGeneration`=$3",
                 bf::infrastructure::mysql::Parameters{transaction, match,
                                                       player, life})
             .empty()) {
      transaction.Commit();
      return application::UseItemResult{.consumed = false};
    }
    if (transaction
            .Exec(
                "SELECT 1 FROM `Matches` m JOIN `MatchParticipants` p USING(`MatchId`) WHERE m.`MatchId`=$1"
                " AND m.`MatchStatus`='playing' AND p.`PlayerId`=$2 AND p.`ParticipantStatus`='joined' FOR "
                "SHARE OF m,p",
                bf::infrastructure::mysql::Parameters{transaction, match,
                                                      player})
            .empty()) {
      return Failure(application::ErrorCode::kInvalidState,
                     "即时复活对局已经结束");
    }
    const auto inventory = transaction.Exec(
        "SELECT owned.`ItemInstanceId`,owned.`Quantity` FROM `PlayerItems` owned JOIN "
        "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` JOIN `ConfigurationVersions` "
        "version ON version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` WHERE "
        "owned.`PlayerId`=$1 AND owned.`ItemId`=100443 AND owned.`Quantity`>0 AND "
        "`Kind`.`IsGrantable` AND version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS "
        "NULL OR owned.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0)) ORDER BY (owned.`ExpiresAt` IS "
        "NULL), owned.`ExpiresAt`,owned.`ItemInstanceId` LIMIT 1 FOR UPDATE OF owned",
        bf::infrastructure::mysql::Parameters{transaction, player, now_us});
    if (inventory.empty()) {
      return Failure(application::ErrorCode::kConflict,
                     "即时复活道具不足或已过期");
    }
    const auto instance = inventory[0][0].As<std::uint64_t>();
    if (inventory[0][1].As<std::uint32_t>() == 1U) {
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
    const auto receipt = transaction.Exec(
        "INSERT INTO "
        "`InstantRespawnUses`(`MatchId`,`PlayerId`,`LifeGeneration`,`BusinessId`,`ConsumedAt`) "
        "VALUES($1,$2,$3,UUID(),FROM_UNIXTIME($4/1000000.0)) RETURNING `BusinessId`",
        bf::infrastructure::mysql::Parameters{transaction, match, player, life,
                                              now_us});
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,100443,NULL,-1,'instant_respawn',$2)",
        bf::infrastructure::mysql::Parameters{transaction, player,
                                              receipt[0][0].As<std::string>()});
    transaction.Commit();
    return application::UseItemResult{.consumed = true};
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("即时复活 SQL 失败，sqlstate={}，detail={}", error.Sqlstate(),
                  error.what());
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "即时复活事务失败");
  } catch (const std::exception&) {
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "即时复活数据库暂不可用");
  }
}

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::Dye(domain::PlayerId player, domain::ItemId item,
                            std::uint16_t color,
                            std::chrono::system_clock::time_point now) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto profile = transaction.Exec(
        "SELECT `ColorCode` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (profile.empty()) {
      return Failure(application::ErrorCode::kPermissionDenied, "角色尚未创建");
    }
    if (profile[0][0].As<std::uint16_t>() == color) {
      transaction.Commit();
      return application::UseItemResult{.color_code = color, .consumed = false};
    }
    const auto inventory = transaction.Exec(
        "SELECT owned.`ItemInstanceId`,owned.`Quantity` FROM `PlayerItems` owned JOIN "
        "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` JOIN `ConfigurationVersions` "
        "version ON version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` WHERE "
        "owned.`PlayerId`=$1 AND owned.`ItemId`=$2 AND owned.`Quantity`>0 AND `Kind`.`IsGrantable` "
        "AND JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))='1' AND "
        "version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS NULL OR "
        "owned.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE OF owned",
        bf::infrastructure::mysql::Parameters{
            transaction, player.Value(), item.Value(),
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count()});
    if (inventory.empty()) {
      return Failure(application::ErrorCode::kConflict, "喷剂不存在或已过期");
    }
    const auto instance = inventory[0][0].As<std::uint64_t>();
    if (inventory[0][1].As<std::uint32_t>() == 1U) {
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
    transaction.Exec(
        "UPDATE `PlayerProfiles` SET `ColorCode`=$2 WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              color});
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,$2,NULL,-1,'character_dye',UUID())",
        bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                              item.Value()});
    transaction.Commit();
    return application::UseItemResult{.color_code = color, .consumed = true};
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("角色染色 SQL 失败，sqlstate={}，detail={}", error.Sqlstate(),
                  error.what());
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "角色染色事务失败");
  } catch (const std::exception&) {
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "角色染色数据库暂不可用");
  }
}

application::ServiceResult<application::UseItemResult>
MySqlItemUseRepository::GrowPet(const application::PetGrowthCommand& command,
                                std::chrono::system_clock::time_point now) {
  const auto pet_id = command.pet_id.Value();
  const auto food_id = command.food_id.Value();
  const bool level_up = !command.food_id.IsValid();
  if (!command.player_id.IsValid() || pet_id < 301001U || pet_id > 301021U ||
      command.maximum_level != (pet_id == 301015U ? 1U : 2U) ||
      (!level_up && food_id != 100643U && food_id != 100644U &&
       food_id != 101605U) ||
      command.hunger_seconds_per_point.count() < 1 ||
      command.hunger_seconds_per_point.count() > 86'400) {
    return Failure(application::ErrorCode::kInvalidInput, "宠物成长参数无效");
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
    const auto profile = transaction.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.player_id.Value()});
    if (profile.empty()) {
      return Failure(application::ErrorCode::kPermissionDenied, "角色尚未创建");
    }
    const auto pet = transaction.Exec(
        "SELECT "
        "owned.`ItemInstanceId`,coalesce(growth.`Level`,0),coalesce(growth.`Experience`,0),(UNIX_TIMESTAMP(growth.`FedUntil`)*1000000)"
        " FROM `PlayerItems` owned JOIN `ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` "
        "JOIN `ConfigurationVersions` version ON "
        "version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` LEFT JOIN "
        "`PlayerPetGrowth` growth ON growth.`ItemInstanceId`=owned.`ItemInstanceId` WHERE "
        "owned.`PlayerId`=$1 AND owned.`ItemId`=$2 AND owned.`Quantity`>0 AND `Kind`.`IsGrantable` "
        "AND JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))='10001' AND "
        "version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS NULL OR "
        "owned.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE OF owned",
        bf::infrastructure::mysql::Parameters{
            transaction, command.player_id.Value(), pet_id, now_us});
    if (pet.empty()) {
      return Failure(application::ErrorCode::kConflict, "宠物不存在或已过期");
    }
    const auto instance = pet[0][0].As<std::uint64_t>();
    if (instance > std::numeric_limits<std::uint32_t>::max()) {
      return Failure(application::ErrorCode::kConflict,
                     "宠物实例超出原版属性标识范围");
    }
    auto level = pet[0][1].As<std::uint32_t>();
    auto experience = pet[0][2].As<std::uint32_t>();
    std::optional<std::int64_t> fed_until;
    if (!pet[0][3].IsNull()) {
      fed_until = pet[0][3].As<std::int64_t>();
    }
    if (level >= command.maximum_level) {
      return Failure(application::ErrorCode::kConflict, "宠物已达到最高阶段");
    }
    if (level_up) {
      if (experience < 100U) {
        return Failure(application::ErrorCode::kConflict,
                       "宠物经验不足，无法升阶段");
      }
      ++level;
      experience = 0U;
    } else {
      if (experience >= 100U) {
        return Failure(application::ErrorCode::kConflict,
                       "请先提升宠物阶段，再继续喂食");
      }
      if (food_id != 100644U && fed_until.has_value() && *fed_until > now_us) {
        return Failure(application::ErrorCode::kConflict,
                       "宠物尚未饥饿，请稍后再喂食");
      }
      const auto food = transaction.Exec(
          "SELECT owned.`ItemInstanceId`,owned.`Quantity` FROM `PlayerItems` owned JOIN "
          "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` JOIN `ConfigurationVersions` "
          "version ON version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` WHERE "
          "owned.`PlayerId`=$1 AND owned.`ItemId`=$2 AND owned.`Quantity`>0 AND `Kind`.`IsGrantable` "
          "AND JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))=$4 AND "
          "version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS NULL OR "
          "owned.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE OF owned",
          bf::infrastructure::mysql::Parameters{
              transaction, command.player_id.Value(), food_id, now_us,
              food_id == 100644U ? "20" : "19"});
      if (food.empty()) {
        return Failure(application::ErrorCode::kConflict, "宠物粮不足或已过期");
      }
      const auto food_instance = food[0][0].As<std::uint64_t>();
      if (food[0][1].As<std::uint32_t>() == 1U) {
        transaction.Exec(
            "DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, food_instance});
        transaction.Exec(
            "DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, food_instance});
      } else {
        transaction.Exec(
            "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{transaction, food_instance});
      }
      const auto gain = food_id == 100644U   ? 100U
                        : food_id == 100643U ? 5U
                        : level == 0U        ? 20U
                                             : 2U;
      const auto fullness = food_id == 100644U   ? 100
                            : food_id == 100643U ? 50
                                                 : 20;
      experience = std::min(experience + gain, 100U);
      fed_until = std::chrono::duration_cast<std::chrono::microseconds>(
                      (now + command.hunger_seconds_per_point * fullness)
                          .time_since_epoch())
                      .count();
      transaction.Exec(
          "INSERT INTO "
          "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
          " VALUES($1,$2,NULL,-1,'pet_feed',UUID())",
          bf::infrastructure::mysql::Parameters{
              transaction, command.player_id.Value(), food_id});
    }
    transaction.Exec(
        "INSERT INTO "
        "`PlayerPetGrowth`(`ItemInstanceId`,`Level`,`Experience`,`FedUntil`,`UpdatedAt`,`PlayerId`,`PetId`)"
        " VALUES($1,$2,$3,FROM_UNIXTIME($4/1000000.0),FROM_UNIXTIME($5/1000000.0),$6,$7) ON "
        "CONFLICT(`ItemInstanceId`) DO UPDATE SET "
        "`Level`=VALUES(`Level`),`Experience`=VALUES(`Experience`),`FedUntil`=VALUES(`FedUntil`),`UpdatedAt`=VALUES(`UpdatedAt`)",
        bf::infrastructure::mysql::Parameters{
            transaction, instance, level, experience, fed_until, now_us,
            command.player_id.Value(), pet_id});
    transaction.Commit();
    return application::UseItemResult{.consumed = !level_up};
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("宠物成长 SQL 失败，sqlstate={}，detail={}", error.Sqlstate(),
                  error.what());
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "宠物成长事务失败");
  } catch (const std::exception&) {
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "宠物成长数据库暂不可用");
  }
}
}  // namespace bf::infrastructure::mysql
