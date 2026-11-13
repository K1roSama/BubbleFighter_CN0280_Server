#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_MESSAGES_H_

/**
 * @file chat_messages.h
 * @brief 定义场景附近/队伍聊天 C2S502 与原生气泡 S2C509 消息。
 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf::protocol {

/** @brief C2S520 的类型、正文、道具选择和不可信颜色。 */
struct MegaphoneRequest final {
  std::uint32_t type{0U};
  std::u16string text;
  std::uint32_t selection{0U};
  std::uint32_t client_color{0U};
};

/** @brief 严格解析 B8CA30/71F1D0 的四个字段并拒绝尾字节。 */
[[nodiscard]] MegaphoneRequest DecodeMegaphoneRequest(
    std::span<const std::uint8_t> body);
/** @brief S2C509 的 +88 字段传真实喇叭模板，客户端用它派生双色效果。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMegaphoneResponse(
    std::u16string_view sender, std::u16string_view text, std::uint32_t item);

/** @brief C2S502 的频道编号和未做业务字符策略的 UTF-16 正文。 */
struct NearbyChatRequest final {
  std::uint32_t channel{
      0U};              ///< 0 为附近，1 为房间队伍；其他值由业务边界拒绝。
  std::u16string text;  ///< 最多 256 个 UTF-16 代码单元且代理项合法。

  /** @brief 按频道标记和正文比较。 */
  bool operator==(const NearbyChatRequest&) const = default;
};

/** @brief S2C509 原生聊天窗和头顶气泡所需的不可交换字段。 */
struct NearbyChatResponse final {
  std::int16_t sender_slot{-3};      ///< 真实场景席位；-3 表示只有聊天窗文本。
  std::u16string_view sender_name;   ///< 客户端显示的权威玩家昵称。
  bool team_only{false};             ///< 是否以队伍聊天样式显示。
  std::u16string_view text;          ///< 已通过业务文本策略的正文。
  std::uint32_t color{0xFFFFFFFFU};  ///< 原版聊天窗与气泡共用的 ARGB 色值。
};

/** @brief 严格解析 C2S502 的频道 DWORD 和 UTF-16 正文并拒绝尾字段。 */
[[nodiscard]] NearbyChatRequest DecodeNearbyChatRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief 按已恢复 B44060/B480C0 字段顺序编码 S2C509。
 * @note 只有 -3 或 0～255 的席位可发布；真实席位附带原生 e
 * 标记，启用头顶气泡及关键词动作。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNearbyChatResponse(
    const NearbyChatResponse& response);

/** @brief 752桥接 S2C65031，原509正文前附服务端处罚事实；不改变原聊天编码。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCleanChatEnvelope(
    std::uint64_t sender, bool punished, bool bypass,
    std::span<const std::uint8_t> native_body);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_MESSAGES_H_
