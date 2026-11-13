#include "persistence/mysql/player_menu_repository.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "features/battle/match_reward.h"
#include "features/characters/character.h"
#include "persistence/mysql/item_attributes.h"
#include "persistence/mysql/sql_connection.h"
#include "persistence/mysql/user_host_prize_repository.h"

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

std::int64_t UnixMicroseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             value.time_since_epoch())
      .count();
}

std::chrono::system_clock::time_point FromUnixMicroseconds(std::int64_t value) {
  return std::chrono::system_clock::time_point{
      std::chrono::microseconds{value}};
}

application::ServiceResult<application::PlayerProgressView> ProgressView(
    std::int64_t experience, std::int64_t level_step,
    std::int64_t maximum_level) {
  if (experience < 0 || level_step <= 0 || level_step > 100'000 ||
      maximum_level <= 0 || maximum_level > 1'000 ||
      level_step * maximum_level * (maximum_level + 1) / 2 - 1 >
          domain::kNativeSignedDwordMaximum) {
    return MakeError(application::ErrorCode::kInternal,
                     "已发布奖励规则的等级参数无效");
  }
  std::int64_t level = 1;
  const auto threshold = [level_step](std::int64_t candidate) {
    return level_step * (candidate - 1) * candidate / 2;
  };
  if (experience >= threshold(maximum_level + 1)) {
    return MakeError(application::ErrorCode::kInternal,
                     "玩家经验超过已发布等级规则上限");
  }
  while (level < maximum_level && experience >= threshold(level + 1)) {
    ++level;
  }
  return application::PlayerProgressView{.level = level,
                                         .experience = experience,
                                         .level_base = threshold(level),
                                         .level_next = threshold(level + 1)};
}

}  // namespace

MySqlPlayerMenuSnapshotReader::MySqlPlayerMenuSnapshotReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::optional<application::PlayerMenuSnapshot>>
MySqlPlayerMenuSnapshotReader::Load(domain::PlayerId player_id,
                                    std::chrono::system_clock::time_point now) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    {
      bf::infrastructure::mysql::WriteTransaction claims{lease.Connection()};
      ClaimUserHostPrizes(claims, player_id);
      claims.Commit();
    }
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto policy = transaction.Exec(
        "SELECT "
        "(JSON_UNQUOTE(JSON_EXTRACT(JSON_EXTRACT(`Payload`,'$.policy'),'$.level_step'))),(JSON_UNQUOTE(JSON_EXTRACT(JSON_EXTRACT(`Payload`,'$.policy'),'$.maximum_level')))"
        " FROM `ConfigurationVersions` WHERE `ConfigurationKind`='reward_rules' AND "
        "`ReleaseStatus`='published' ORDER BY `ConfigurationVersionId`");
    if (policy.size() != 1U || policy[0][0].IsNull() || policy[0][1].IsNull()) {
      return MakeError(application::ErrorCode::kInternal,
                       "菜单读取要求唯一完整的已发布奖励规则");
    }
    const auto profile = transaction.Exec(
        "SELECT "
        "profile.`Nickname`,profile.`InitialFighterId`,progress.`Experience`,coalesce((SELECT "
        "clan.`MarkCode` FROM `Clans` clan JOIN `ClanMemberships` member ON "
        "member.`ClanId`=clan.`ClanId` WHERE "
        "member.`PlayerId`=profile.`PlayerId`),0),profile.`ColorCode`,profile.`ActiveWeaponPreset`,EXISTS(SELECT"
        " 1 FROM `PlayerItems` expansion JOIN `ItemTemplates` `Kind` ON "
        "`Kind`.`ItemId`=expansion.`ItemId` WHERE expansion.`PlayerId`=profile.`PlayerId` AND "
        "expansion.`ItemId`=500001 AND expansion.`Quantity`>0 AND `Kind`.`IsGrantable` AND "
        "(expansion.`ExpiresAt` IS NULL OR expansion.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0))) FROM"
        " `PlayerProfiles` AS profile JOIN `PlayerProgressions` AS progress ON "
        "progress.`PlayerId`=profile.`PlayerId` WHERE profile.`PlayerId`=$1 AND "
        "profile.`IsCharacterCreated`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              UnixMicroseconds(now)});
    if (profile.empty()) {
      transaction.Commit();
      return std::optional<application::PlayerMenuSnapshot>{};
    }
    const auto fighter =
        domain::InitialFighterFromProtocol(profile[0][1].As<std::uint32_t>());
    if (!fighter.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "菜单资料包含未发布的初始角色");
    }
    const bool expanded = profile[0][6].As<bool>();
    const auto active_preset =
        expanded ? profile[0][5].As<std::uint32_t>() : std::uint8_t{0U};

    const auto wallets = transaction.Exec(
        "SELECT count(*) AS wallet_count,max(CASE WHEN `CurrencyCode`='lucci' THEN `Balance` END) "
        "AS lucci,max(CASE WHEN `CurrencyCode`='bubble_cash' THEN `Balance` END) AS "
        "bubble_cash,max(CASE WHEN `CurrencyCode`='treasure_bubble' THEN `Balance` END) AS "
        "treasure_bubble FROM `PlayerWallets` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (wallets[0][0].As<std::size_t>() != 3U || wallets[0][1].IsNull() ||
        wallets[0][2].IsNull() || wallets[0][3].IsNull()) {
      return MakeError(application::ErrorCode::kInternal,
                       "菜单资料缺少完整的三种钱包");
    }

    const auto item_rows = transaction.Exec(
        "SELECT "
        "item.`ItemInstanceId`,item.`ItemId`,item.`Quantity`,(UNIX_TIMESTAMP(item.`AcquiredAt`)*1000000)"
        " AS acquired_us,CASE WHEN item.`ExpiresAt` IS NULL THEN NULL ELSE "
        "(UNIX_TIMESTAMP(item.`ExpiresAt`)*1000000) END AS "
        "expires_us,coalesce(`EquipmentSlot`(template.`ItemId`,template.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type'))),''),equipment.`ItemInstanceId`"
        " IS NOT NULL AND item.`Quantity`>0 AS "
        "equipped,growth.`Level`,growth.`Experience`,(UNIX_TIMESTAMP(growth.`FedUntil`)*1000000) "
        "FROM `PlayerItems` AS item JOIN `ItemTemplates` AS template ON "
        "template.`ItemId`=item.`ItemId` LEFT JOIN `PlayerPetGrowth` AS growth ON "
        "growth.`ItemInstanceId`=item.`ItemInstanceId` LEFT JOIN `PlayerEquipment` AS equipment ON "
        "equipment.`PlayerId`=item.`PlayerId` AND equipment.`LoadoutId`=CASE WHEN "
        "`EquipmentSlot`(template.`ItemId`,template.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type')))"
        " IN ('primary','grenade','crystal') THEN $3 ELSE 0 END AND "
        "equipment.`ItemInstanceId`=item.`ItemInstanceId` AND "
        "`ClothingCharacterCompatible`(item.`ItemId`,(SELECT "
        "`EquippedCharacter`($1,FROM_UNIXTIME($2/1000000.0)))) AND "
        "(equipment.`SlotCode`=`EquipmentSlot`(template.`ItemId`,template.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type')))"
        " OR (equipment.`SlotCode`=template.`SlotCode` AND "
        "JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type'))='10001')) AND "
        "(coalesce(JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.use_type')),'')<>'10001' OR"
        " item.`ItemInstanceId`=(SELECT candidate.`ItemInstanceId` FROM `PlayerEquipment` candidate"
        " JOIN `PlayerItems` owned ON owned.`ItemInstanceId`=candidate.`ItemInstanceId` JOIN "
        "`ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` WHERE "
        "candidate.`PlayerId`=item.`PlayerId` AND candidate.`LoadoutId`=0 AND "
        "JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))='10001' AND "
        "(owned.`ExpiresAt` IS NULL OR owned.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0)) ORDER BY "
        "candidate.`EquippedAt` DESC,candidate.`ItemInstanceId` DESC LIMIT 1)) WHERE "
        "item.`PlayerId`=$1 AND (item.`ExpiresAt` IS NULL OR "
        "item.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0)) ORDER BY item.`ItemInstanceId`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              UnixMicroseconds(now),
                                              active_preset});

    std::vector<application::PlayerItemView> items;
    auto item_attributes = LoadItemAttributes(transaction, player_id.Value());
    items.reserve(item_rows.size());
    for (const auto& row : item_rows) {
      std::optional<std::chrono::system_clock::time_point> expires_at;
      if (!row[4].IsNull()) {
        expires_at = FromUnixMicroseconds(row[4].As<std::int64_t>());
      }
      std::optional<application::PetGrowthView> growth;
      if (!row[7].IsNull()) {
        growth = application::PetGrowthView{
            .level = row[7].As<std::uint32_t>(),
            .experience = row[8].As<std::uint32_t>()};
        if (!row[9].IsNull()) {
          growth->fed_until = FromUnixMicroseconds(row[9].As<std::int64_t>());
        }
      }
      items.push_back(
          {.item_instance_id =
               domain::ItemInstanceId{row[0].As<std::uint64_t>()},
           .item_id = domain::ItemId{row[1].As<std::uint64_t>()},
           .quantity = row[2].As<std::int64_t>(),
           .acquired_at = FromUnixMicroseconds(row[3].As<std::int64_t>()),
           .expires_at = expires_at,
           .slot_code = row[5].As<std::string>(),
           .equipped = row[6].As<bool>(),
           .pet_growth = growth,
           .attributes =
               std::move(item_attributes[row[0].As<std::uint64_t>()])});
    }

    auto progress_result = ProgressView(profile[0][2].As<std::int64_t>(),
                                        policy[0][0].As<std::int64_t>(),
                                        policy[0][1].As<std::int64_t>());
    if (const auto* error =
            std::get_if<application::ServiceError>(&progress_result)) {
      return *error;
    }
    application::PlayerMenuSnapshot snapshot{
        .player_id = player_id,
        .nickname = profile[0][0].As<std::string>(),
        .initial_fighter = *fighter,
        .progress = std::get<application::PlayerProgressView>(
            std::move(progress_result)),
        .wallets = {.lucci = wallets[0][1].As<std::int64_t>(),
                    .bubble_cash = wallets[0][2].As<std::int64_t>(),
                    .treasure_bubble = wallets[0][3].As<std::int64_t>()},
        .items = std::move(items),
        .observed_at = now,
        .clan_mark = profile[0][3].As<std::uint32_t>(),
        .color_code = profile[0][4].As<std::uint16_t>()};
    snapshot.active_weapon_preset = static_cast<std::uint8_t>(active_preset);
    // 原称号槽不属于武器页；仅本人有效实例会进入802/806/场景的装备标记。
    const auto title = transaction.Exec(
        "SELECT e.`ItemInstanceId`,d.`TitleType` FROM `NativeTitleEquipment` e JOIN "
        "`NativeTitleDefinitions` d ON d.`TitleId`=e.`TitleId` WHERE e.`PlayerId`=$1 AND "
        "(d.`TitleType`<>11 OR EXISTS (SELECT 1 FROM `CoupleMembers` WHERE `PlayerId`=$1))",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    for (auto& item : snapshot.items) {
      if (item.item_id.Value() >= 400000U && item.item_id.Value() < 401000U) {
        item.equipped = !title.empty() && title[0][0].As<std::uint64_t>() ==
                                              item.item_instance_id.Value();
        item.slot_code = "title";
      }
    }
    snapshot.weapon_presets[0].enabled = true;
    const auto expired_rows = transaction.Exec(
        "SELECT "
        "item.`ItemInstanceId`,item.`ItemId`,item.`Quantity`,(UNIX_TIMESTAMP(item.`AcquiredAt`)*1000000),(UNIX_TIMESTAMP(item.`ExpiresAt`)*1000000),coalesce(`EquipmentSlot`(`Kind`.`ItemId`,`Kind`.`SlotCode`,JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))),''),EXISTS(SELECT"
        " 1 FROM `PlayerEquipment` equipment WHERE equipment.`PlayerId`=item.`PlayerId` AND "
        "equipment.`ItemInstanceId`=item.`ItemInstanceId`),growth.`Level`,growth.`Experience`,(UNIX_TIMESTAMP(growth.`FedUntil`)*1000000)"
        " FROM `PlayerItems` item JOIN `ItemTemplates` `Kind` ON `Kind`.`ItemId`=item.`ItemId` LEFT"
        " JOIN `PlayerPetGrowth` growth ON growth.`ItemInstanceId`=item.`ItemInstanceId` WHERE "
        "item.`PlayerId`=$1 AND item.`ExpiresAt`<=FROM_UNIXTIME($2/1000000.0) ORDER BY "
        "item.`ItemInstanceId`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              UnixMicroseconds(now)});
    for (const auto& row : expired_rows) {
      std::optional<application::PetGrowthView> growth;
      if (!row[7].IsNull()) {
        growth = application::PetGrowthView{
            .level = row[7].As<std::uint32_t>(),
            .experience = row[8].As<std::uint32_t>()};
        if (!row[9].IsNull())
          growth->fed_until = FromUnixMicroseconds(row[9].As<std::int64_t>());
      }
      snapshot.expired_items.push_back(
          {.item_instance_id =
               domain::ItemInstanceId{row[0].As<std::uint64_t>()},
           .item_id = domain::ItemId{row[1].As<std::uint64_t>()},
           .quantity = row[2].As<std::int64_t>(),
           .acquired_at = FromUnixMicroseconds(row[3].As<std::int64_t>()),
           .expires_at = FromUnixMicroseconds(row[4].As<std::int64_t>()),
           .slot_code = row[5].As<std::string>(),
           .equipped = row[6].As<bool>(),
           .pet_growth = growth});
    }
    snapshot.weapon_presets[1].enabled = expanded;
    snapshot.weapon_presets[2].enabled = expanded;
    const auto preset_rows = transaction.Exec(
        "SELECT equipment.`LoadoutId`,equipment.`SlotCode`,item.`ItemId` FROM `PlayerEquipment` "
        "equipment JOIN `PlayerItems` item ON item.`ItemInstanceId`=equipment.`ItemInstanceId` AND "
        "item.`PlayerId`=equipment.`PlayerId` JOIN `ItemTemplates` template ON "
        "template.`ItemId`=item.`ItemId` WHERE equipment.`PlayerId`=$1 AND equipment.`LoadoutId` "
        "BETWEEN 0 AND 2 AND (equipment.`SlotCode`='primary' OR (equipment.`SlotCode`='secondary' "
        "AND equipment.`LoadoutId`=0)) AND template.`SlotCode`=equipment.`SlotCode` AND "
        "item.`Quantity`>0 AND (item.`ExpiresAt` IS NULL OR "
        "item.`ExpiresAt`>FROM_UNIXTIME($2/1000000.0))",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              UnixMicroseconds(now)});
    for (const auto& row : preset_rows) {
      auto& preset = snapshot.weapon_presets[row[0].As<std::size_t>()];
      const auto weapon = domain::ItemId{row[2].As<std::uint64_t>()};
      if (row[1].As<std::string_view>() == "primary") {
        preset.primary = weapon;
      } else {
        for (auto& shared : snapshot.weapon_presets) {
          shared.secondary = weapon;
        }
      }
    }
    const auto public_profile = transaction.Exec(
        "SELECT "
        "account.`LoginName`,progress.`Rating`,coalesce(clan.`ClanName`,''),profile.`LevelIconType`,coalesce(member.`ClanId`,0)"
        " FROM `PlayerProfiles` profile JOIN `Accounts` account USING(`AccountId`) JOIN "
        "`PlayerProgressions` progress USING(`PlayerId`) LEFT JOIN `ClanMemberships` member "
        "USING(`PlayerId`) LEFT JOIN `Clans` clan USING(`ClanId`) WHERE profile.`PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    snapshot.login_name = public_profile.OneRow()[0].As<std::string>();
    snapshot.rating = public_profile.OneRow()[1].As<std::uint32_t>();
    snapshot.clan_name = public_profile.OneRow()[2].As<std::string>();
    snapshot.level_icon_type = public_profile.OneRow()[3].As<std::uint32_t>();
    snapshot.clan_id = public_profile.OneRow()[4].As<std::uint32_t>();
    if (snapshot.level_icon_type == 3U && snapshot.clan_id == 0U)
      snapshot.level_icon_type = 0U;
    const auto statistics = transaction.Exec(
        "SELECT `ModeId`,`Matches`,`Wins`,`Losses`,`Draws`,`Bubbles`,`Pangs`,`Helps` FROM "
        "`PlayerModeStatistics` WHERE `PlayerId`=$1 ORDER BY `ModeId`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    for (const auto& row : statistics) {
      snapshot.mode_statistics.push_back({.mode_id = row[0].As<std::uint32_t>(),
                                          .matches = row[1].As<std::uint64_t>(),
                                          .wins = row[2].As<std::uint64_t>(),
                                          .losses = row[3].As<std::uint64_t>(),
                                          .draws = row[4].As<std::uint64_t>(),
                                          .bubbles = row[5].As<std::uint64_t>(),
                                          .pangs = row[6].As<std::uint64_t>(),
                                          .helps = row[7].As<std::uint64_t>()});
    }
    // Read only persisted medal events, in the same database snapshot as the
    // identity. No inference from PlayerModeStatistics or 400xxx inventory
    // ownership is allowed.
    const auto medals = transaction.Exec(
        "SELECT `MedalKey`,AvailableCount FROM `NativeMedalBalances` WHERE `PlayerId`=$1 ORDER BY "
        "`MedalKey`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    const auto single_clears = transaction.Exec(
        "SELECT `StageKey`,BestReportedElapsedMs DIV 1000 FROM `SinglePlayProgress` WHERE "
        "`PlayerId`=$1 AND ReportedClears>0 AND BestReportedElapsedMs IS NOT NULL ORDER BY "
        "`StageKey`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    for (const auto& row : single_clears)
      snapshot.single_play_clears.push_back(
          {.stage_key = row[0].As<std::uint32_t>(),
           .observed_seconds = row[1].As<std::uint32_t>()});
    snapshot.medal_progress.emplace();
    for (const auto& row : medals) {
      snapshot.medal_progress->push_back(
          {.medal_key = row[0].As<std::uint32_t>(),
           .count = row[1].As<std::uint64_t>()});
    }
    const auto couple = transaction.Exec(
        "SELECT "
        "peer.`PlayerId`,profile.`Nickname`,c.`RingItemId`,(UNIX_TIMESTAMP(c.`FormedAt`)*1000000) "
        "FROM `CoupleMembers` own JOIN `CoupleMembers` peer ON peer.`CoupleId`=own.`CoupleId` AND "
        "peer.`PlayerId`<>own.`PlayerId` JOIN `Couples` c ON c.`CoupleId`=own.`CoupleId` JOIN "
        "`PlayerProfiles` profile ON profile.`PlayerId`=peer.`PlayerId` WHERE own.`PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (!couple.empty()) {
      snapshot.couple = application::CoupleProfileView{
          .partner_id = domain::PlayerId{couple[0][0].As<std::uint64_t>()},
          .partner_nickname = couple[0][1].As<std::string>(),
          .ring_item_id = couple[0][2].As<std::uint32_t>(),
          .since = FromUnixMicroseconds(couple[0][3].As<std::int64_t>())};
    }
    // 主武器按页读取；三个页签中的手枪都来自共享零号页。
    transaction.Commit();
    return std::optional<application::PlayerMenuSnapshot>{std::move(snapshot)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取菜单快照时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取菜单快照 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 拒绝菜单快照查询");
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取菜单快照时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
