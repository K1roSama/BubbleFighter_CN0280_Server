#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_DEDICATED_ROOM_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_DEDICATED_ROOM_H_

/**
 * @file dedicated_room.h
 * @brief 定义与网络和数据库解耦的单房间进程执行边界。
 */
#include <map>

#include "features/rooms/room_coordinator.h"
#include "protocol/internal/room_envelope.h"

namespace bf::runtime {

/** @brief 由网关认证的会话与顺序，不接受客户端自报。 */
struct RoomCommandOrigin final {
  std::uint64_t session_id{0U};
  std::uint64_t session_generation{0U};
  std::uint64_t sequence{0U};
};

/** @brief 一个实例独占一个协调器；调用方必须从同一事件循环调用。 */
class DedicatedRoom final {
 public:
  /** @brief 固定实例身份；无效身份或租约长度会抛出 invalid_argument。 */
  DedicatedRoom(protocol::internal::RoomIdentity identity,
                RoomMatchTiming timing = {}, std::uint16_t capacity = 12U);
  /** @brief 检查实例、房间代次、会话及单调序号后调用现有房间核心。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> Command(
      const protocol::internal::RoomEnvelope& envelope, RoomActorContext actor,
      const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 对内部持久化回执和定时事件校验实例与当前房间代次。 */
  [[nodiscard]] bool Accepts(
      const protocol::internal::RoomEnvelope& envelope) const noexcept;
  /** @brief 只返回房间的只读值；不暴露可跨进程使用的对象指针。 */
  [[nodiscard]] std::vector<RoomMemberSnapshot> Members() const;
  /** @brief 返回当前房间计时令牌，进程内保留单调时钟。 */
  [[nodiscard]] std::vector<RoomTimerToken> Timers() const;
  /** @brief 运行到期事件；跨进程调用方不能提供任意 timer 指针。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> ApplyTimer(
      const RoomTimerToken& token, std::chrono::steady_clock::time_point now);
  /** @brief 当前代次；尚未建房或已关闭返回零。 */
  [[nodiscard]] std::uint64_t Generation() const noexcept;
  /** @brief 是否已经创建并关闭本实例唯一房间。 */
  [[nodiscard]] bool Closed() const noexcept;
  /** @brief 同事件循环内供持久化桥接调用，禁止保存返回引用。 */
  [[nodiscard]] RoomCoordinator& Coordinator() noexcept;
  /** @brief 返回不可变实例路由身份。 */
  [[nodiscard]] const protocol::internal::RoomIdentity& Identity()
      const noexcept;

 private:
  protocol::internal::RoomIdentity identity_;
  RoomCoordinator coordinator_;
  std::map<domain::PlayerId, RoomCommandOrigin> origins_;
  bool created_{false};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_DEDICATED_ROOM_H_
