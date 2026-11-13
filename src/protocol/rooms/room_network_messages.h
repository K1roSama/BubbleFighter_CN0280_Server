#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_NETWORK_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_NETWORK_MESSAGES_H_

/** @file room_network_messages.h @brief
 * 本服房间网络设置扩展，不占用未确认的原房间字段。 */
#include <cstdint>
#include <vector>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
inline constexpr std::uint16_t kRoomNetworkRequest = 65070U;
inline constexpr std::uint16_t kRoomNetworkResponse = 65071U;
inline constexpr std::uint16_t kRoomNetworkPeers = 65072U;
inline constexpr std::uint16_t kRoomNetworkRegister = 65073U;
inline constexpr std::uint16_t kRoomNetworkMovement = 65075U;
inline constexpr std::uint16_t kRoomNetworkMovementRelay = 65076U;
/** @brief 房间偏好；实际路径必须另行报告，直连缺失时仍由服务器处理。 */
enum class RoomNetworkMode : std::uint32_t {
  kServerDirect = 0U,
  kP2pPreferred = 1U
};
inline constexpr std::uint32_t kRoomRuleNoGrenades = 1U;

/** @brief 查询或设置当前房间；代次由服务器发布，防止旧窗口请求写入新房间。 */
struct RoomNetworkRequest final {
  std::uint32_t action{0U};      ///< 0查询、1设置。
  std::uint32_t room{0U};        ///< 当前房间标识。
  std::uint64_t generation{0U};  ///< 设置必须匹配房间代次。
  RoomNetworkMode mode{RoomNetworkMode::kServerDirect};  ///< 新房间默认服务器。
  std::uint32_t global_rules{0U};  ///< v2 房间全局规则；v1 不能修改此字段。
  std::uint32_t version{1U};
};

/** @brief 下发偏好、权限和真实通道状态，不能用偏好代替实际连接。 */
struct RoomNetworkState final {
  std::uint32_t room{0U};
  std::uint64_t generation{0U};
  RoomNetworkMode selected{RoomNetworkMode::kServerDirect};
  RoomNetworkMode actual{RoomNetworkMode::kServerDirect};
  bool editable{false};
  bool p2p_available{false};
  std::uint32_t reason{0U};  ///< 0服务器、1直连就绪、2通道不可用自动回退。
  std::uint32_t global_rules{0U};
};

inline RoomNetworkRequest DecodeRoomNetworkRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  RoomNetworkRequest result;
  result.version = reader.ReadU32();
  if (result.version != 1U && result.version != 2U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unsupported room settings version"};
  result.action = reader.ReadU32();
  result.room = reader.ReadU32();
  result.generation = reader.ReadU64();
  const auto mode = reader.ReadU32();
  if (result.version == 2U) result.global_rules = reader.ReadU32();
  reader.RequireEnd();
  if (result.action > 1U || mode > 1U || result.room > 65535U ||
      (result.global_rules & ~kRoomRuleNoGrenades))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid room network request"};
  result.mode = static_cast<RoomNetworkMode>(mode);
  return result;
}

inline std::vector<std::uint8_t> EncodeRoomNetworkState(
    const RoomNetworkState& state, std::uint32_t version = 1U) {
  if (version != 1U && version != 2U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unsupported room settings state version"};
  PacketWriter writer;
  writer.WriteU32(version);
  writer.WriteU32(state.room);
  writer.WriteU64(state.generation);
  writer.WriteU32(static_cast<std::uint32_t>(state.selected));
  writer.WriteU32(static_cast<std::uint32_t>(state.actual));
  writer.WriteU32(state.editable ? 1U : 0U);
  writer.WriteU32(state.p2p_available ? 1U : 0U);
  writer.WriteU32(state.reason);
  if (version == 2U) writer.WriteU32(state.global_rules);
  return writer.Release();
}
}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_NETWORK_MESSAGES_H_
