#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_EQUIPMENT_RELAY_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_EQUIPMENT_RELAY_MESSAGES_H_

/**
 * @file equipment_relay_messages.h
 * @brief 原生 RelayServerConnection 的装备序列协议，与主网关 opcode
 * 命名空间分离。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf::protocol {

inline constexpr std::size_t kEquipmentSequenceLength = 1000U;
inline constexpr std::size_t kEquipmentSequenceWords = 32U;
/** @brief B5C6A0 原样读取的 20 字节会话键；不将未恢复字段误命名为玩家 ID。 */
using EquipmentRelayKey = std::array<std::uint8_t, 20U>;
/** @brief 原版按最低位优先展开，最后一个 DWORD 仅前 8 位有效。 */
using EquipmentSequenceBits =
    std::array<std::uint32_t, kEquipmentSequenceWords>;

/** @brief 一名角色的防御百分比序列和暴击抵消序列。 */
struct EquipmentDefenseSequence final {
  std::array<std::uint8_t, kEquipmentSequenceLength> defense{};
  EquipmentSequenceBits critical_cancel{};
  bool operator==(const EquipmentDefenseSequence&) const = default;
};

/** @brief Relay C2S1；B5A0D0 → B5B930，不是主连接 C2S1。 */
struct EquipmentRelayLogin final {
  EquipmentRelayKey key{};
  std::uint8_t seat{0U};
  std::uint32_t client_context{0U};
  std::uint32_t profile_id{
      0U};  ///< B5A0D0 从本地个人资料 +64/+68 读取的身份字段。
  std::string nickname;
  bool operator==(const EquipmentRelayLogin&) const = default;
};

/** @brief 7D65D0/480840 的 Relay C2S16 状态事件；没有 HP 或加成数值。 */
struct EquipmentHealthEvent final {
  std::uint32_t actor{0U};
  std::uint8_t reason{0U};
  std::uint32_t tick{0U};   ///< 原版只初始化低 24 位，最高字节不参与事件标识。
  std::uint8_t detail{0U};  ///< 仅 reason=16 序列化的附加字段。
  bool operator==(const EquipmentHealthEvent&) const = default;
};

[[nodiscard]] EquipmentHealthEvent DecodeEquipmentHealthEvent(
    std::span<const std::uint8_t> body);

/** @brief 原版辅助角色报告；只供审计，不授权战斗效果。 */
struct EquipmentActorReport final {
  std::uint16_t opcode{0U};
  std::uint32_t actor{0U};
  std::uint32_t related{
      0U};  ///< C2S13 的来源字段；可属于 NPC/武器，不等同于玩家席位。
  std::uint8_t kind{0U};
  std::uint16_t value{0U};
  std::uint32_t tick{0U};  ///< 有时间戳的报告只初始化低 24 位。
  std::uint16_t first{0U};
  std::uint16_t second{0U};
  std::uint32_t seat{
      0U};  ///< C2S27 repeats the recipient's own seat from S2C960.
  /// C2S24 CriticalBuffData (vtable 1283800 -> 7F10E0): the two critical
  /// attributes the native client decrypts from Character+0x234/+0x294 after
  /// every weapon change.
  std::array<float, 2U> critical{};
  bool operator==(const EquipmentActorReport&) const = default;
};

/** @brief 严格校验报告字段、原生状态原因和正文边界。 */
[[nodiscard]] EquipmentActorReport DecodeEquipmentActorReport(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 主网关 S2C601，IPv4 网络字节顺序、端口小端、原样会话键。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentRelayEndpoint(
    const std::array<std::uint8_t, 4U>& ipv4, std::uint16_t port,
    const EquipmentRelayKey& key);
/** @brief 严格解码 Relay C2S1 的全部字段并拒绝尾随数据。 */
[[nodiscard]] EquipmentRelayLogin DecodeEquipmentRelayLogin(
    std::span<const std::uint8_t> body);
/** @brief Relay S2C5，角色对象 ID + 1000 个百分比字节；收包会重置原版防御游标。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentDefenseSequence(
    std::uint32_t object, const EquipmentDefenseSequence& sequence);
/** @brief Relay S2C6，角色对象 ID + 32 个 DWORD；收包会重置原版暴击抵消游标。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentCriticalCancelSequence(
    std::uint32_t object, const EquipmentDefenseSequence& sequence);
/** @brief Relay S2C4 的两组攻击暴击位；与抵消序列分开编码。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentCriticalAttackSequences(
    std::uint32_t object, const EquipmentSequenceBits& first,
    const EquipmentSequenceBits& second);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_EQUIPMENT_RELAY_MESSAGES_H_
