#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_INTERNAL_ROOM_ENVELOPE_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_INTERNAL_ROOM_ENVELOPE_H_

/**
 * @file room_envelope.h
 * @brief 定义进程间有界消息及房间、实例、会话和所有权隔离字段。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "application/identifier.h"

namespace bf::protocol::internal {

inline constexpr std::uint16_t kRoomProtocolVersion = 1U;
inline constexpr std::size_t kRoomEnvelopeHeaderBytes = 108U;
inline constexpr std::size_t kMaximumRoomPayloadBytes = 1'048'576U;

/** @brief 内部协议消息种类，不复用客户端 opcode 命名空间。 */
enum class RoomMessageKind : std::uint16_t {
  kAuthenticate = 1U,
  kBind = 2U,
  kCommand = 3U,
  kReply = 4U,
  kEvent = 5U,
  kHeartbeat = 6U,
  kStop = 7U,
  kError = 8U,
};

/** @brief 一个房间实例的不可混用路由身份；实例随机值由可信调度端分配。 */
struct RoomIdentity final {
  domain::RoomId room_id;
  std::array<std::uint8_t, 16U> instance_id{};
  std::uint64_t owner_epoch{0U};
  bool operator==(const RoomIdentity&) const = default;
  [[nodiscard]] bool IsValid() const noexcept;
};

/** @brief 拥有全部负载的内部消息；时间戳不能跨主机充当单调时钟。 */
struct RoomEnvelope final {
  RoomMessageKind kind{RoomMessageKind::kCommand};
  std::uint64_t request_id{0U};
  RoomIdentity identity;
  std::uint64_t room_generation{0U};
  std::array<std::uint8_t, 16U> match_id{};
  domain::PlayerId player_id;
  std::uint64_t session_id{0U};
  std::uint64_t session_generation{0U};
  std::uint64_t sequence{0U};
  std::vector<std::uint8_t> payload;
  bool operator==(const RoomEnvelope&) const = default;
};

/** @brief 编码严格长度前缀，超出上限或无效消息种类抛出 ProtocolError。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomEnvelope(
    const RoomEnvelope& message);
/** @brief 严格验证 magic、版本、长度和种类后返回拥有型消息。 */
[[nodiscard]] RoomEnvelope DecodeRoomEnvelope(
    std::span<const std::uint8_t> bytes);

}  // namespace bf::protocol::internal

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_INTERNAL_ROOM_ENVELOPE_H_
