#include "persistence/mysql/room_lease_repository.h"

#include <limits>
#include <stdexcept>

#include "persistence/mysql/sql_connection.h"
#include "persistence/mysql/user_host_prize_repository.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError Failure(application::ErrorCode code, std::string text,
                                  bool retry = false) {
  return {.code = code,
          .safe_message = std::move(text),
          .correlation_id = {},
          .is_retryable = retry};
}

bool ValidOwner(const application::RoomOwnership& owner) {
  return owner.owner_epoch > 0U &&
         owner.owner_epoch <= static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max());
}

template <typename Work>
auto WithTransaction(MySqlConnectionPool& pool, Work work) -> decltype(work(
    std::declval<bf::infrastructure::mysql::WriteTransaction&>())) {
  auto acquired = pool.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    auto result = work(transaction);
    if (!std::holds_alternative<application::ServiceError>(result))
      transaction.Commit();
    return result;
  } catch (const std::exception&) {
    return Failure(application::ErrorCode::kStorageUnavailable,
                   "room lease transaction failed", true);
  }
}
}  // namespace

bool LockRoomOwnership(bf::infrastructure::mysql::TransactionBase& tx,
                       const application::RoomOwnership& owner,
                       std::optional<domain::RoomId> room) {
  if (!ValidOwner(owner)) return false;
  const auto instance = owner.instance_id.ToUuidString();
  // Always take node before room locks, including expired-instance recovery.
  const auto node = tx.Exec(
      "SELECT n.`NodeId` FROM `DsNodes` n JOIN `RoomInstances` r USING(`NodeId`) WHERE "
      "r.`InstanceId`=$1 AND n.`LeaseExpiresAt`>SYSDATE(6) FOR SHARE OF n",
      bf::infrastructure::mysql::Parameters{tx, instance});
  if (node.empty()) return false;
  const auto rows = tx.Exec(
      "SELECT `RoomId` FROM `RoomInstances` WHERE `InstanceId`=$1 AND `OwnerEpoch`=$2 AND "
      "`InstanceStatus` IN('active','draining') AND `LeaseExpiresAt`>SYSDATE(6) FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, instance, owner.owner_epoch});
  return rows.size() == 1U &&
         (!room || room->Value() == rows[0][0].As<std::uint64_t>());
}

bool LockMatchOwnership(bf::infrastructure::mysql::TransactionBase& tx,
                        const domain::MatchId& match,
                        const std::optional<application::RoomOwnership>& owner,
                        bool require_live) {
  const auto rows = tx.Exec(
      "SELECT `RoomInstanceId`,`RoomOwnerEpoch` FROM `Matches` WHERE `MatchId`=$1",
      bf::infrastructure::mysql::Parameters{tx, match.ToUuidString()});
  if (rows.size() != 1U) return false;
  if (rows[0][0].IsNull()) return !owner;
  if (!owner ||
      rows[0][0].As<std::string>() != owner->instance_id.ToUuidString() ||
      rows[0][1].As<std::uint64_t>() != owner->owner_epoch)
    return false;
  return !require_live || LockRoomOwnership(tx, *owner);
}

MySqlRoomLeaseRepository::MySqlRoomLeaseRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<bool> MySqlRoomLeaseRepository::RegisterNode(
    const application::DsNodeRegistration& node) {
  if (!node.region_id || node.region_id > 32767U || node.name.empty() ||
      node.name.size() > 128U)
    return Failure(application::ErrorCode::kInvalidInput,
                   "invalid node registration");
  return WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<bool> {
        return !tx.Exec(
                      "INSERT INTO `DsNodes`(`NodeId`,`RegionId`,`NodeName`,`LeaseExpiresAt`) "
                      "VALUES($1,$2,$3,SYSDATE(6)+INTERVAL 15 SECOND) ON CONFLICT(`NodeId`) DO NOTHING RETURNING "
                      "`NodeId`",
                      bf::infrastructure::mysql::Parameters{
                          tx, node.node_id.ToUuidString(), node.region_id,
                          node.name})
                    .empty();
      });
}

application::ServiceResult<bool> MySqlRoomLeaseRepository::RenewNode(
    const domain::MatchId& node, bool draining) {
  return WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<bool> {
        return !tx.Exec(
                      "UPDATE `DsNodes` SET `LeaseExpiresAt`=SYSDATE(6)+INTERVAL 15 SECOND,`Draining`=$2 WHERE "
                      "`NodeId`=$1 AND `LeaseExpiresAt`>SYSDATE(6) RETURNING `NodeId`",
                      bf::infrastructure::mysql::Parameters{
                          tx, node.ToUuidString(), draining})
                    .empty();
      });
}

application::ServiceResult<application::RoomLease>
MySqlRoomLeaseRepository::AllocateRoom(const domain::MatchId& node,
                                       const domain::MatchId& instance) {
  return WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<application::RoomLease> {
        const auto node_uuid = node.ToUuidString();
        const auto instance_uuid = instance.ToUuidString();
        if (tx.Exec(
                  "SELECT `NodeId` FROM `DsNodes` WHERE `NodeId`=$1 AND NOT `Draining` AND "
                  "`LeaseExpiresAt`>SYSDATE(6) FOR SHARE",
                  bf::infrastructure::mysql::Parameters{tx, node_uuid})
                .empty())
          return Failure(application::ErrorCode::kConflict,
                         "node lease expired or draining");
        // Serialize assignment of the client's room IDs.
        tx.Exec(
            "SELECT LockKey FROM TransactionLocks WHERE LockKey='room_placement' FOR UPDATE");
        const auto existing = tx.Exec(
            "SELECT `RoomId`,`OwnerEpoch`,`NodeId`,`InstanceStatus`,`LeaseExpiresAt`>SYSDATE(6) FROM "
            "`RoomInstances` WHERE `InstanceId`=$1 FOR UPDATE",
            bf::infrastructure::mysql::Parameters{tx, instance_uuid});
        if (!existing.empty()) {
          if (existing[0][2].As<std::string>() != node_uuid ||
              !existing[0][4].As<bool>() ||
              existing[0][3].As<std::string_view>() != "active")
            return Failure(application::ErrorCode::kConflict,
                           "instance cannot be allocated again");
          return application::RoomLease{
              domain::RoomId{existing[0][0].As<std::uint64_t>()},
              {instance, existing[0][1].As<std::uint64_t>()}};
        }
        const auto free = tx.Exec(
            "SELECT candidate FROM (SELECT 1 candidate UNION SELECT RoomId+1 FROM RoomInstances WHERE "
            "InstanceStatus IN('active','draining') AND RoomId<32767) candidates WHERE NOT "
            "EXISTS(SELECT 1 FROM RoomInstances r WHERE r.RoomId=candidate AND r.InstanceStatus "
            "IN('active','draining')) ORDER BY candidate LIMIT 1");
        if (free.empty())
          return Failure(application::ErrorCode::kOverloaded,
                         "room ID capacity exhausted", true);
        const auto room = free[0][0].As<std::uint64_t>();
        tx.Exec(
            "INSERT INTO `RoomInstances`(`InstanceId`,`NodeId`,`RoomId`,`OwnerEpoch`,`LeaseExpiresAt`) "
            "SELECT $1,$2,$3,coalesce((SELECT max(`OwnerEpoch`) FROM `RoomInstances` WHERE "
            "`RoomId`=$3),0)+1,least(SYSDATE(6)+INTERVAL 10 SECOND,`LeaseExpiresAt`) FROM `DsNodes` "
            "WHERE `NodeId`=$2",
            bf::infrastructure::mysql::Parameters{tx, instance_uuid, node_uuid,
                                                  room});
        const auto epoch =
            tx.Exec(
                  "SELECT `OwnerEpoch` FROM `RoomInstances` WHERE `InstanceId`=$1",
                  Parameters{tx, instance_uuid})
                .OneField()
                .As<std::uint64_t>();
        return application::RoomLease{domain::RoomId{room}, {instance, epoch}};
      });
}

application::ServiceResult<bool> MySqlRoomLeaseRepository::RenewRoom(
    const application::RoomOwnership& owner) {
  return WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<bool> {
        if (!LockRoomOwnership(tx, owner)) return false;
        tx.Exec(
            "UPDATE `RoomInstances` r, `DsNodes` n SET r.`LeaseExpiresAt`=least(SYSDATE(6)+INTERVAL 10 "
            "SECOND,n.`LeaseExpiresAt`) WHERE r.`NodeId`=n.`NodeId` AND r.`InstanceId`=$1",
            bf::infrastructure::mysql::Parameters{
                tx, owner.instance_id.ToUuidString()});
        return true;
      });
}

application::ServiceResult<bool> MySqlRoomLeaseRepository::ReleaseRoom(
    const application::RoomOwnership& owner) {
  if (!ValidOwner(owner))
    return Failure(application::ErrorCode::kInvalidInput,
                   "invalid owner epoch");
  return WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<bool> {
        const auto instance = owner.instance_id.ToUuidString();
        tx.Exec(
            "SELECT n.`NodeId` FROM `DsNodes` n JOIN `RoomInstances` r USING(`NodeId`) WHERE "
            "r.`InstanceId`=$1 FOR SHARE OF n",
            bf::infrastructure::mysql::Parameters{tx, instance});
        const auto rows = tx.Exec(
            "SELECT `InstanceStatus` FROM `RoomInstances` WHERE `InstanceId`=$1 AND `OwnerEpoch`=$2 FOR"
            " UPDATE",
            bf::infrastructure::mysql::Parameters{tx, instance,
                                                  owner.owner_epoch});
        if (rows.empty()) return false;
        if (rows[0][0].As<std::string_view>() == "closed") return true;
        if (rows[0][0].As<std::string_view>() == "failed") return false;
        if (!tx.Exec(
                   "SELECT `MatchId` FROM `Matches` WHERE `RoomInstanceId`=$1 AND `MatchStatus` "
                   "IN('loading','playing')",
                   bf::infrastructure::mysql::Parameters{tx, instance})
                 .empty())
          return false;
        tx.Exec(
            "UPDATE `RoomInstances` SET `InstanceStatus`='closed',`LeaseExpiresAt`=SYSDATE(6) WHERE "
            "`InstanceId`=$1",
            bf::infrastructure::mysql::Parameters{tx, instance});
        return true;
      });
}

application::ServiceResult<application::RoomLeaseRecovery>
MySqlRoomLeaseRepository::RecoverExpired(std::size_t limit) {
  if (!limit || limit > 256U)
    return Failure(application::ErrorCode::kInvalidInput,
                   "invalid recovery limit");
  using Candidates = std::vector<std::pair<std::string, std::string>>;
  auto loaded = WithTransaction(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> application::ServiceResult<Candidates> {
        const auto candidates = tx.Exec(
            "SELECT r.`InstanceId`,r.`NodeId` FROM `RoomInstances` r JOIN `DsNodes` n USING(`NodeId`) "
            "WHERE r.`InstanceStatus` IN('active','draining') AND (r.`LeaseExpiresAt`<=SYSDATE(6) OR "
            "n.`LeaseExpiresAt`<=SYSDATE(6)) ORDER BY r.`NodeId`,r.`InstanceId` LIMIT $1",
            bf::infrastructure::mysql::Parameters{tx, limit});
        Candidates result;
        for (const auto& row : candidates)
          result.emplace_back(row[0].As<std::string>(),
                              row[1].As<std::string>());
        return result;
      });
  if (const auto* error = std::get_if<application::ServiceError>(&loaded))
    return *error;
  application::RoomLeaseRecovery report;
  for (const auto& [instance, node_id] : std::get<Candidates>(loaded)) {
    // Release every instance's locks before visiting another room. This keeps
    // profile locks from one refund out of the next room's ownership lock
    // order.
    auto
        recovered = WithTransaction(pool_,
                                    [&](bf::infrastructure::mysql::
                                            WriteTransaction& tx) -> application::
                                                                      ServiceResult<
                                                                          application::
                                                                              RoomLeaseRecovery> {
                                                                        application::
                                                                            RoomLeaseRecovery
                                                                                one;
                                                                        const auto node = tx.Exec(
                                                                            "SELECT `LeaseExpiresAt`<=SYSDATE(6) FROM `DsNodes` WHERE `NodeId`=$1 FOR SHARE",
                                                                            bf::infrastructure::
                                                                                mysql::Parameters{
                                                                                    tx,
                                                                                    node_id});
                                                                        const auto room = tx.Exec(
                                                                            "SELECT `OwnerEpoch`,`LeaseExpiresAt`<=SYSDATE(6) FROM `RoomInstances` WHERE "
                                                                            "`InstanceId`=$1 AND `InstanceStatus` IN('active','draining') FOR UPDATE",
                                                                            bf::infrastructure::
                                                                                mysql::Parameters{
                                                                                    tx,
                                                                                    instance});
                                                                        if (room.empty() ||
                                                                            (!node[0][0]
                                                                                  .As<bool>() &&
                                                                             !room[0][1]
                                                                                  .As<bool>()))
                                                                          return one;
                                                                        tx.Exec(
                                                                            "UPDATE `RoomInstances` SET `InstanceStatus`='failed' WHERE `InstanceId`=$1",
                                                                            bf::infrastructure::
                                                                                mysql::Parameters{
                                                                                    tx,
                                                                                    instance});
                                                                        const auto matches = tx.Exec(
                                                                            "SELECT `MatchId` FROM `Matches` WHERE `RoomInstanceId`=$1 AND `MatchStatus` "
                                                                            "IN('loading','playing') ORDER BY `MatchId` FOR UPDATE",
                                                                            bf::infrastructure::
                                                                                mysql::Parameters{
                                                                                    tx,
                                                                                    instance});
                                                                        const auto sponsors = tx.Exec(
                                                                            "SELECT DISTINCT p.`SponsorPlayerId` FROM `UserHostPrizes` p JOIN `Matches` m "
                                                                            "USING(`MatchId`) WHERE m.`RoomInstanceId`=$1 AND m.`MatchStatus` IN('loading','playing') "
                                                                            "AND p.`PrizeStatus`='funded' ORDER BY p.`SponsorPlayerId`",
                                                                            bf::infrastructure::
                                                                                mysql::Parameters{
                                                                                    tx,
                                                                                    instance});
                                                                        for (
                                                                            const auto&
                                                                                sponsor :
                                                                            sponsors)
                                                                          tx.Exec(
                                                                              "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
                                                                              bf::infrastructure::mysql::Parameters{
                                                                                  tx,
                                                                                  sponsor[0]
                                                                                      .As<std::
                                                                                              uint64_t>()});
                                                                        for (
                                                                            const auto&
                                                                                match :
                                                                            matches) {
                                                                          const auto uuid =
                                                                              match[0]
                                                                                  .As<std::
                                                                                          string>();
                                                                          tx.Exec(
                                                                              "UPDATE `MatchParticipants` SET "
                                                                              "`ParticipantStatus`='disconnected',`LeftAt`=greatest(`JoinedAt`,SYSDATE(6)) WHERE "
                                                                              "`MatchId`=$1 AND `ParticipantStatus`='joined'",
                                                                              bf::infrastructure::
                                                                                  mysql::Parameters{
                                                                                      tx,
                                                                                      uuid});
                                                                          tx.Exec(
                                                                              "UPDATE `Matches` SET "
                                                                              "`MatchStatus`='interrupted',`EndedAt`=greatest(`StartedAt`,SYSDATE(6)) WHERE `MatchId`=$1",
                                                                              bf::infrastructure::
                                                                                  mysql::Parameters{
                                                                                      tx,
                                                                                      uuid});
                                                                          RefundUserHostPrize(
                                                                              tx,
                                                                              uuid);
                                                                        }
                                                                        one.expired
                                                                            .push_back(
                                                                                {*domain::MatchId::
                                                                                     FromUuidString(
                                                                                         instance),
                                                                                 room[0][0]
                                                                                     .As<std::
                                                                                             uint64_t>()});
                                                                        one.interrupted_matches =
                                                                            matches
                                                                                .size();
                                                                        return one;
                                                                      });
    if (const auto* error = std::get_if<application::ServiceError>(&recovered))
      return *error;
    auto one = std::get<application::RoomLeaseRecovery>(std::move(recovered));
    report.expired.insert(report.expired.end(), one.expired.begin(),
                          one.expired.end());
    report.interrupted_matches += one.interrupted_matches;
  }
  return report;
}
}  // namespace bf::infrastructure::mysql
