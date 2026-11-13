#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SMART_PING_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SMART_PING_MESSAGES_H_

/**
 * @file smart_ping_messages.h
 * @brief 定义战斗内智能标记（Smart Ping）兼容消息的字段模型与编解码。
 */
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bf::protocol {
// 1302 uses 33283. In 752 that opcode starts a rescue, so the compatibility
// transport reserves 65032 and preserves the reference's nine-DWORD body.
inline constexpr std::uint16_t kSmartPingOpcode = 65032U;
inline constexpr std::uint32_t kSmartPingCooldownMs = 1000U;

/** @brief 一次智能标记：发起者、目标对象、标记种类、位置和法线。 */
struct SmartPing final {
  std::uint32_t actor{0U};
  std::uint32_t object{0xffffffffU};
  std::uint32_t kind{0U};
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  bool operator==(const SmartPing&) const = default;
};

/** @brief 参考客户端允许在该模式发送此类标记（物品标记仅限少数生存/道具模式）。
 */
[[nodiscard]] bool SmartPingMode(std::uint32_t mode,
                                 std::uint32_t kind) noexcept;
/** @brief 解码九个 DWORD；身份、种类、坐标或法线越界抛 ProtocolError。 */
[[nodiscard]] SmartPing DecodeSmartPing(std::span<const std::uint8_t> body);
[[nodiscard]] std::vector<std::uint8_t> EncodeSmartPing(const SmartPing& value);
}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SMART_PING_MESSAGES_H_
