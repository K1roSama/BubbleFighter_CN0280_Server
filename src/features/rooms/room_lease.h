#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_LEASE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_LEASE_H_

/**
 * @file room_lease.h
 * @brief 定义数据库时钟控制的房间所有权及节点登记端口。
 */
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/rooms/room_ownership.h"

namespace bf::application {
/** @brief 节点进程的生命周期身份，重启时必须分配新的 UUID。 */
struct DsNodeRegistration final {
  domain::MatchId node_id;
  std::uint32_t region_id{1U};
  std::string name;
};

/** @brief 已分配的客户端房间编号及不可变实例身份。 */
struct RoomLease final {
  domain::RoomId room_id;
  RoomOwnership owner;
};

/** @brief 有界的过期实例恢复结果，保留已持久化的待结算命令。 */
struct RoomLeaseRecovery final {
  std::vector<RoomOwnership> expired;
  std::size_t interrupted_matches{0U};
};

/** @brief 网关后台服务使用的租约端口，DS 进程不执行 SQL。 */
class RoomLeaseStore {
 public:
  virtual ~RoomLeaseStore() = default;
  [[nodiscard]] virtual ServiceResult<bool> RegisterNode(
      const DsNodeRegistration& node) = 0;
  [[nodiscard]] virtual ServiceResult<bool> RenewNode(
      const domain::MatchId& node, bool draining) = 0;
  [[nodiscard]] virtual ServiceResult<RoomLease> AllocateRoom(
      const domain::MatchId& node, const domain::MatchId& instance) = 0;
  [[nodiscard]] virtual ServiceResult<bool> RenewRoom(
      const RoomOwnership& owner) = 0;
  [[nodiscard]] virtual ServiceResult<bool> ReleaseRoom(
      const RoomOwnership& owner) = 0;
  [[nodiscard]] virtual ServiceResult<RoomLeaseRecovery> RecoverExpired(
      std::size_t limit) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_LEASE_H_
