#include "persistence/mysql/couple_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
using application::ErrorCode;

application::ServiceError Fail(ErrorCode code, std::string message) {
  return {.code = code, .safe_message = std::move(message)};
}

bf::infrastructure::mysql::QueryResult LockProfiles(
    bf::infrastructure::mysql::WriteTransaction& tx, domain::PlayerId a,
    domain::PlayerId b) {
  return tx.Exec(
      "SELECT `PlayerId`,`Nickname` FROM `PlayerProfiles` WHERE `PlayerId` IN ($1,$2) AND "
      "`IsCharacterCreated` ORDER BY `PlayerId` FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, a.Value(), b.Value()});
}

std::string Name(const bf::infrastructure::mysql::QueryResult& rows,
                 domain::PlayerId player) {
  for (const auto& row : rows)
    if (row[0].As<std::uint64_t>() == player.Value())
      return row[1].As<std::string>();
  return {};
}

void Audit(bf::infrastructure::mysql::WriteTransaction& tx,
           domain::PlayerId player, std::uint32_t item, int delta,
           std::string_view reason, const std::string& operation) {
  tx.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`QuantityDelta`,`ReasonCode`,`BusinessId`) "
      "VALUES($1,$2,$3,$4,$5)",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item, delta,
                                            reason, operation});
}

bool Consume(bf::infrastructure::mysql::WriteTransaction& tx,
             domain::PlayerId player, std::uint32_t item,
             std::string_view reason, const std::string& operation,
             std::chrono::system_clock::time_point now, bool require_active) {
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                          now.time_since_epoch())
                          .count();
  const auto rows = tx.Exec(
      "SELECT `ItemInstanceId`,`Quantity` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 "
      "AND (NOT $4 OR `ExpiresAt` IS NULL OR `ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), item, micros,
                                            require_active});
  if (rows.empty()) return false;
  const auto id = rows[0][0].As<std::uint64_t>();
  if (rows[0][1].As<std::int64_t>() == 1) {
    tx.Exec("DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{tx, id});
    tx.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
            bf::infrastructure::mysql::Parameters{tx, id});
  } else {
    tx.Exec(
        "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{tx, id});
  }
  Audit(tx, player, item, -1, reason, operation);
  return true;
}

void Grant(bf::infrastructure::mysql::WriteTransaction& tx,
           domain::PlayerId player, std::uint32_t ring,
           const std::string& operation) {
  tx.Exec(
      "INSERT INTO "
      "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
      "VALUES($1,$2,1,CURRENT_TIMESTAMP(6),NULL,'couple') ON CONFLICT(`PlayerId`,`ItemId`) DO "
      "UPDATE SET `Quantity`=`PlayerItems`.`Quantity`+1,`ExpiresAt`=NULL",
      bf::infrastructure::mysql::Parameters{tx, player.Value(), ring});
  Audit(tx, player, ring, 1, "couple_ring_grant", operation);
}
}  // namespace

application::ServiceResult<application::CoupleChange>
MySqlCoupleRepository::Form(const application::CoupleFormCommand& command) {
  return Apply(command, false);
}

std::optional<application::ServiceError> MySqlCoupleRepository::CheckInvitation(
    domain::PlayerId actor, domain::PlayerId partner) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    if (!tx.Exec(
               "SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR "
               "(`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)",
               bf::infrastructure::mysql::Parameters{tx, actor.Value(),
                                                     partner.Value()})
             .empty())
      return Fail(ErrorCode::kPermissionDenied,
                  "双方存在屏蔽关系，不能邀请情侣");
    tx.Commit();
    return std::nullopt;
  } catch (const std::exception&) {
    return Fail(ErrorCode::kStorageUnavailable, "无法核实情侣邀请权限");
  }
}

application::ServiceResult<application::CoupleChange>
MySqlCoupleRepository::ChangeRing(
    const application::CoupleFormCommand& command) {
  return Apply(command, true);
}

application::ServiceResult<application::CoupleChange>
MySqlCoupleRepository::Apply(const application::CoupleFormCommand& c,
                             bool change) {
  const auto ring = application::CoupleDisplayRing(c.invitation_ring);
  if (!c.actor.IsValid() || !c.partner.IsValid() || c.actor == c.partner ||
      ring == 0U)
    return Fail(ErrorCode::kInvalidInput, "情侣身份或戒指无效");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto profiles = LockProfiles(tx, c.actor, c.partner);
    if (profiles.size() != 2U || Name(profiles, c.actor) != c.actor_name ||
        Name(profiles, c.partner) != c.partner_name)
      return Fail(ErrorCode::kConflict, "玩家昵称已变化，请重新发起情侣邀请");
    const auto memberships = tx.Exec(
        "SELECT m.`PlayerId`,m.`CoupleId`,c.`RingItemId` FROM `CoupleMembers` m JOIN `Couples` c "
        "USING(`CoupleId`) WHERE m.`PlayerId` IN ($1,$2) ORDER BY m.`PlayerId`",
        bf::infrastructure::mysql::Parameters{tx, c.actor.Value(),
                                              c.partner.Value()});
    if ((!change && !memberships.empty()) ||
        (change &&
         (memberships.size() != 2U || memberships[0][1].As<std::string>() !=
                                          memberships[1][1].As<std::string>())))
      return Fail(ErrorCode::kConflict, "情侣关系已经变化，请刷新资料");
    if (!tx.Exec(
               "SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR "
               "(`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)",
               bf::infrastructure::mysql::Parameters{tx, c.actor.Value(),
                                                     c.partner.Value()})
             .empty())
      return Fail(ErrorCode::kPermissionDenied,
                  "双方存在屏蔽关系，不能建立或变更情侣");
    const auto template_rows = tx.Exec(
        "SELECT 1 FROM `ItemTemplates` t JOIN `ConfigurationVersions` v "
        "USING(`ConfigurationVersionId`) WHERE t.`ItemId`=$1 AND t.`IsGrantable` AND "
        "JSON_UNQUOTE(JSON_EXTRACT(t.`ClientFields`,'$.use_type'))='17' AND "
        "v.`ReleaseStatus`='published'",
        bf::infrastructure::mysql::Parameters{tx, c.invitation_ring});
    if (template_rows.empty())
      return Fail(ErrorCode::kPermissionDenied, "此情侣戒指尚未开放");
    const auto operation =
        tx.Exec("SELECT UUID()").OneRow()[0].As<std::string>();
    const auto old_ring = change ? memberships[0][2].As<std::uint32_t>() : 0U;
    if (change &&
        (old_ring == ring ||
         ((old_ring == 100605U || old_ring == 100985U || old_ring == 101035U) &&
          ring == 101561U)))
      return Fail(ErrorCode::kConflict, "不能重复使用相同戒指或退回活动戒指");
    if (!Consume(tx, c.actor, c.invitation_ring, "couple_invitation_used",
                 operation, c.now, true))
      return Fail(ErrorCode::kInsufficientBalance, "情侣戒指不足或已经过期");
    if (change) {
      for (auto player : {c.actor, c.partner})
        static_cast<void>(Consume(tx, player, old_ring, "couple_ring_replaced",
                                  operation, c.now, false));
      tx.Exec("UPDATE `Couples` SET `RingItemId`=$2 WHERE `CoupleId`=$1",
              bf::infrastructure::mysql::Parameters{
                  tx, memberships[0][1].As<std::string>(), ring});
    } else {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              c.now.time_since_epoch())
                              .count();
      tx.Exec(
          "INSERT INTO `Couples`(`CoupleId`,`RingItemId`,`FormedAt`) "
          "VALUES($1,$2,FROM_UNIXTIME($3/1000000.0))",
          bf::infrastructure::mysql::Parameters{tx, operation, ring, micros});
      tx.Exec(
          "INSERT INTO `CoupleMembers`(`PlayerId`,`CoupleId`) VALUES($1,$3),($2,$3)",
          bf::infrastructure::mysql::Parameters{tx, c.actor.Value(),
                                                c.partner.Value(), operation});
    }
    const auto pair_id =
        change ? memberships[0][1].As<std::string>() : operation;
    const auto count =
        tx.Exec("SELECT COUNT(*) FROM CoupleMembers WHERE CoupleId=$1",
                Parameters{tx, pair_id})
            .OneField()
            .As<std::uint64_t>();
    if (count != 2U)
      return Fail(ErrorCode::kConflict, "情侣关系必须恰好包含两名成员");
    Grant(tx, c.actor, ring, operation);
    Grant(tx, c.partner, ring, operation);
    tx.Commit();
    return application::CoupleChange{.actor = c.actor,
                                     .partner = c.partner,
                                     .actor_name = c.actor_name,
                                     .partner_name = c.partner_name,
                                     .ring = ring,
                                     .changed = true};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return Fail(ErrorCode::kConflict, "玩家已建立其他情侣关系");
  } catch (const bf::infrastructure::mysql::SqlError& e) {
    spdlog::error("couple transaction SQL failed sqlstate={}", e.Sqlstate());
    return Fail(ErrorCode::kStorageUnavailable,
                "情侣事务暂时无法确认，请刷新资料和库存后重试");
  } catch (const std::exception&) {
    return Fail(ErrorCode::kStorageUnavailable, "情侣数据库暂时不可用");
  }
}

application::ServiceResult<application::CoupleChange>
MySqlCoupleRepository::Breakup(domain::PlayerId actor,
                               domain::PlayerId partner) {
  if (!actor.IsValid() || !partner.IsValid() || actor == partner)
    return Fail(ErrorCode::kInvalidInput, "解除情侣身份无效");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto profiles = LockProfiles(tx, actor, partner);
    const auto pair = tx.Exec(
        "SELECT c.`CoupleId`,c.`RingItemId` FROM `CoupleMembers` a JOIN `CoupleMembers` b ON "
        "a.`CoupleId`=b.`CoupleId` JOIN `Couples` c ON c.`CoupleId`=a.`CoupleId` WHERE "
        "a.`PlayerId`=$1 AND b.`PlayerId`=$2",
        bf::infrastructure::mysql::Parameters{tx, actor.Value(),
                                              partner.Value()});
    if (profiles.size() != 2U || pair.empty())
      return Fail(ErrorCode::kConflict, "情侣关系已解除或发生变化");
    const auto ring = pair[0][1].As<std::uint32_t>();
    const auto operation =
        tx.Exec("SELECT UUID()").OneRow()[0].As<std::string>();
    for (auto player : {actor, partner})
      static_cast<void>(Consume(tx, player, ring, "couple_breakup", operation,
                                std::chrono::system_clock::now(), false));
    tx.Exec("DELETE FROM `CoupleMembers` WHERE `CoupleId`=$1",
            bf::infrastructure::mysql::Parameters{
                tx, pair[0][0].As<std::string>()});
    tx.Exec("DELETE FROM `Couples` WHERE `CoupleId`=$1",
            bf::infrastructure::mysql::Parameters{
                tx, pair[0][0].As<std::string>()});
    tx.Commit();
    return application::CoupleChange{.actor = actor,
                                     .partner = partner,
                                     .actor_name = Name(profiles, actor),
                                     .partner_name = Name(profiles, partner),
                                     .ring = 0U,
                                     .changed = true};
  } catch (const bf::infrastructure::mysql::SqlError& e) {
    spdlog::error("couple breakup SQL failed sqlstate={}", e.Sqlstate());
    return Fail(ErrorCode::kStorageUnavailable, "解除情侣事务暂时无法提交");
  } catch (const std::exception&) {
    return Fail(ErrorCode::kStorageUnavailable, "情侣数据库暂时不可用");
  }
}
}  // namespace bf::infrastructure::mysql
