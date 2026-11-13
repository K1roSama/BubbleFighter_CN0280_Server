#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ROOM_LEASE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ROOM_LEASE_REPOSITORY_H_

/**
 * @file room_lease_repository.h
 * @brief 定义带所有权代次保护的 MySQL 节点和房间租约。
 */
#include "features/rooms/room_lease.h"
#include "persistence/mysql/connection_pool.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
/** @brief Locks node then room, preventing lease renewal and recovery from
 * crossing a write. */
[[nodiscard]] bool LockRoomOwnership(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::RoomOwnership& owner,
    std::optional<domain::RoomId> room = std::nullopt);
/** @brief Checks immutable match ownership and, for a new write, the live
 * lease. */
[[nodiscard]] bool LockMatchOwnership(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const domain::MatchId& match,
    const std::optional<application::RoomOwnership>& owner,
    bool require_live = true);

/** @brief 使用 15 秒节点和 10 秒房间租约，禁止续活已经过期的所有者。 */
class MySqlRoomLeaseRepository final : public application::RoomLeaseStore {
 public:
  explicit MySqlRoomLeaseRepository(MySqlConnectionPool& pool) noexcept;
  [[nodiscard]] application::ServiceResult<bool> RegisterNode(
      const application::DsNodeRegistration& node) override;
  [[nodiscard]] application::ServiceResult<bool> RenewNode(
      const domain::MatchId& node, bool draining) override;
  [[nodiscard]] application::ServiceResult<application::RoomLease> AllocateRoom(
      const domain::MatchId& node, const domain::MatchId& instance) override;
  [[nodiscard]] application::ServiceResult<bool> RenewRoom(
      const application::RoomOwnership& owner) override;
  [[nodiscard]] application::ServiceResult<bool> ReleaseRoom(
      const application::RoomOwnership& owner) override;
  [[nodiscard]] application::ServiceResult<application::RoomLeaseRecovery>
  RecoverExpired(std::size_t limit) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ROOM_LEASE_REPOSITORY_H_
