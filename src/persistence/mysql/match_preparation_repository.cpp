#include "persistence/mysql/match_preparation_repository.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/battle/mode_weapon.h"
#include "features/battle/sniper_weapon.h"
#include "features/rooms/room_item_policy.h"
#include "persistence/mysql/item_attributes.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

constexpr std::uint64_t kWeaponItemBase = 200'000U;
constexpr std::uint64_t kWeaponItemEnd = 300'000U;
constexpr std::array<std::uint64_t, 4U> kDefaultWeaponItems{202'004U, 201'004U,
                                                            207'001U, 205'033U};
constexpr std::array<std::string_view, 4U> kWeaponSlots{"primary", "secondary",
                                                        "melee", "grenade"};

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

std::optional<std::size_t> WeaponBar(std::string_view slot) noexcept {
  for (std::size_t index = 0U; index < kWeaponSlots.size(); ++index) {
    if (kWeaponSlots[index] == slot) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> NativeTemplate(std::uint64_t item_id) noexcept {
  if (item_id < kWeaponItemBase || item_id >= kWeaponItemEnd) {
    return std::nullopt;
  }
  const auto value = item_id - kWeaponItemBase;
  if (value == 0U || value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

MySqlMatchPreparationReader::MySqlMatchPreparationReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::PreparedMatch>
MySqlMatchPreparationReader::Load(
    const application::MatchPreparationRequest& request) {
  const auto score_model = domain::GetMatchScoreModel(request.mode_id);
  if (!score_model.has_value()) {
    return MakeError(application::ErrorCode::kInvalidInput, "模式规则尚未恢复");
  }
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto versions = transaction.Exec(
        "SELECT `ConfigurationKind`,`ConfigurationVersionId` FROM `ConfigurationVersions` WHERE "
        "`ConfigurationKind` IN ('catalog','reward_rules') AND `ReleaseStatus`='published' ORDER BY"
        " `ConfigurationKind`,`ConfigurationVersionId`");
    std::optional<std::int64_t> catalog_version;
    std::optional<std::int64_t> reward_version;
    for (const auto& row : versions) {
      const auto kind = row[0].As<std::string>();
      auto& selected = kind == "catalog" ? catalog_version : reward_version;
      if (selected.has_value()) {
        return MakeError(
            application::ErrorCode::kInternal,
            "开局要求 catalog 与 reward_rules 各有唯一 published 版本");
      }
      selected = row[1].As<std::int64_t>();
    }
    if (!catalog_version.has_value() || !reward_version.has_value() ||
        *catalog_version <= 0 || *reward_version <= 0) {
      return MakeError(application::ErrorCode::kInternal,
                       "开局缺少已发布目录或奖励规则版本");
    }

    const auto default_rows = transaction.Exec(
        "SELECT `ItemId`,`SlotCode` FROM `ItemTemplates` WHERE `ConfigurationVersionId`=$1 AND "
        "`ItemId` IN ($2,$3,$4,$5) ORDER BY `ItemId`",
        bf::infrastructure::mysql::Parameters{
            transaction, *catalog_version, kDefaultWeaponItems[0],
            kDefaultWeaponItems[1], kDefaultWeaponItems[2],
            kDefaultWeaponItems[3]});
    if (default_rows.size() != kDefaultWeaponItems.size()) {
      return MakeError(application::ErrorCode::kInternal,
                       "已发布目录缺少 build 752 的原生默认武器");
    }
    std::array<std::optional<std::uint32_t>,
               application::kPreparedWeaponBarCount>
        defaults;
    for (const auto& row : default_rows) {
      const auto bar = WeaponBar(row[1].As<std::string>());
      const auto item_id = row[0].As<std::uint64_t>();
      const auto template_id = NativeTemplate(item_id);
      if (!bar.has_value() || !template_id.has_value() || *bar >= 4U ||
          kDefaultWeaponItems[*bar] != item_id || defaults[*bar].has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "已发布目录的原生默认武器槽位不一致");
      }
      defaults[*bar] = *template_id;
    }

    application::PreparedMatch prepared{
        .room_id = request.room_id,
        .mode_id = request.mode_id,
        .configuration_version_id = *catalog_version,
        .reward_rules_version_id = *reward_version,
        .score_model = *score_model,
        .ordinary_respawn_allowed =
            domain::OrdinaryMatchRespawn(request.mode_id),
        .participants = {}};
    prepared.participants.reserve(request.participants.size());
    for (const auto& participant : request.participants) {
      const auto player = transaction.Exec(
          "SELECT COALESCE(`InitialFighterId`,1) FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND "
          "`IsCharacterCreated`",
          bf::infrastructure::mysql::Parameters{transaction,
                                                participant.player_id.Value()});
      if (player.size() != 1U) {
        return MakeError(application::ErrorCode::kConflict,
                         "开局参与者资料不存在或尚未完成角色创建");
      }
      auto loadout = defaults;
      auto fighter = player[0][0].As<std::uint32_t>();
      const auto equipped = transaction.Exec(
          "SELECT template.`SlotCode`,item.`ItemId`,item.`ItemInstanceId` FROM `PlayerEquipment` AS "
          "equipment JOIN `PlayerItems` AS item ON item.`ItemInstanceId`=equipment.`ItemInstanceId` "
          "AND item.`PlayerId`=equipment.`PlayerId` JOIN `ItemTemplates` AS template ON "
          "template.`ItemId`=item.`ItemId` AND template.`ConfigurationVersionId`=$2 JOIN "
          "`PlayerProfiles` profile ON profile.`PlayerId`=equipment.`PlayerId` WHERE "
          "equipment.`PlayerId`=$1 AND equipment.`LoadoutId`=CASE WHEN "
          "`EquipmentSlot`(template.`ItemId`,template.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type')))"
          " IN ('primary','grenade','crystal') AND EXISTS(SELECT 1 FROM `PlayerItems` expansion JOIN "
          "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=expansion.`ItemId` WHERE "
          "expansion.`PlayerId`=equipment.`PlayerId` AND expansion.`ItemId`=500001 AND "
          "expansion.`Quantity`>0 AND `Kind`.`IsGrantable` AND (expansion.`ExpiresAt` IS NULL OR "
          "expansion.`ExpiresAt`>CURRENT_TIMESTAMP(6))) THEN profile.`ActiveWeaponPreset` ELSE 0 END "
          "AND item.`Quantity`>0 AND template.`IsGrantable` AND "
          "`ClothingCharacterCompatible`(item.`ItemId`,`EquippedCharacter`($1,CURRENT_TIMESTAMP(6))) "
          "AND (item.`ExpiresAt` IS NULL OR item.`ExpiresAt`>CURRENT_TIMESTAMP(6)) ORDER BY "
          "equipment.`SlotCode`",
          bf::infrastructure::mysql::Parameters{
              transaction, participant.player_id.Value(), *catalog_version});
      std::vector<std::uint32_t> equipment_items;
      std::vector<domain::EquippedItemAttributes> equipment_attributes;
      auto item_attributes =
          LoadItemAttributes(transaction, participant.player_id.Value());
      bool lucci_pouch = false;
      bool chocolate_box = false;
      for (const auto& row : equipped) {
        lucci_pouch = lucci_pouch || row[1].As<std::uint64_t>() == 100256U;
        chocolate_box = chocolate_box || row[1].As<std::uint64_t>() == 100257U;
        const auto item = row[1].As<std::uint64_t>();
        if (item > std::numeric_limits<std::uint32_t>::max())
          return MakeError(application::ErrorCode::kInternal,
                           "装备编号超出原生范围");
        equipment_items.push_back(static_cast<std::uint32_t>(item));
        const auto attributes =
            item_attributes.find(row[2].As<std::uint64_t>());
        if (attributes != item_attributes.end())
          equipment_attributes.push_back(
              {static_cast<std::uint32_t>(item), attributes->second});
        if (row[0].As<std::string>() == "character" && item >= 300001U &&
            item < 400000U)
          fighter = static_cast<std::uint32_t>(item - 300000U);
        const auto bar = WeaponBar(row[0].As<std::string>());
        if (!bar.has_value()) {
          continue;
        }
        const auto template_id = NativeTemplate(row[1].As<std::uint64_t>());
        if (!template_id.has_value()) {
          return MakeError(application::ErrorCode::kInternal,
                           "已装备武器无法映射为 build 752 原生模板");
        }
        loadout[*bar] = *template_id;
      }
      auto checked_items = equipment_items;
      for (const auto& weapon : loadout)
        if (weapon) checked_items.push_back(200000U + *weapon);
      const auto restriction = domain::RestrictedEquipmentNotice(
          request.restricted_items, checked_items);
      if (!restriction.empty())
        return MakeError(application::ErrorCode::kConflict, restriction);
      if (request.mode_id == 23U || request.mode_id == 25U) {
        // 原版 848380/81A4C0：Area32 开局不使用大厅配装；Lambient.xml 指定近战
        // 7947。
        // 此处只恢复初始栏位，拾取装备必须由后续权威拾取流程更新，不能接受任意切枪模板。
        // 5377F0 的背包初始化随后将治疗道具 5920 装入栏 4；538870
        // 赠送一个药包。
        loadout = {std::nullopt, std::nullopt, 7947U, 5003U, 5920U};
      } else if (request.mode_id == 21U) {
        // 848380 keeps the lobby bars except for the team-colored BFRunner
        // baton.
        loadout[2] = participant.team_code == 1U ? 7058U : 7057U;
      } else if (request.mode_id == 27U) {
        // 848380: Snowfight clears bars 0/1/2 and uses 5077 + feature33 in
        // bar3. This build's E01120 does not enable feature33.
        loadout = {std::nullopt, std::nullopt, std::nullopt, 5077U,
                   std::nullopt};
      }
      // 81A4C0 applies Ethi's grenade variant after the mode-specific
      // selection.
      if (fighter == 7U && loadout[3]) loadout[3] = *loadout[3] + 100U;
      for (auto& weapon : loadout)
        if (weapon)
          weapon = domain::ModeWeaponTemplate(request.mode_id, *weapon);
      if (domain::ModeRequiresSniperPrimary(request.mode_id) &&
          (!loadout[0] || !domain::IsNativeSniperWeapon(*loadout[0]))) {
        return MakeError(application::ErrorCode::kConflict,
                         "狙击模式需要装备有效的狙击主武器");
      }
      prepared.participants.push_back(
          {.identity = participant,
           .weapon_templates = loadout,
           .lucci_pouch =
               lucci_pouch && request.mode_id != 23U && request.mode_id != 25U,
           .chocolate_box = chocolate_box && request.mode_id != 23U &&
                            request.mode_id != 25U,
           .fighter = fighter,
           .equipment_items = std::move(equipment_items),
           .equipment_attributes = std::move(equipment_attributes)});
    }
    transaction.Commit();
    return prepared;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取开局配装时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取开局配装 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 拒绝开局配装查询", true);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取开局配装时发生未分类错误", true);
  }
}

}  // namespace bf::infrastructure::mysql
