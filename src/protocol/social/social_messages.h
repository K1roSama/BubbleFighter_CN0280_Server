#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_SOCIAL_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_SOCIAL_MESSAGES_H_

/**
 * @file social_messages.h
 * @brief 定义本地原版通讯适配使用的好友、申请和私聊扩展消息。
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "application/identifier.h"

namespace bf::protocol {

inline constexpr std::size_t kMaximumSocialRosterEntries =
    512U;  ///< 合并好友/申请/屏蔽后的防分配上限。

/** @brief C2S65010 中由本地适配器发送的已发布社交动作。 */
enum class SocialAction : std::uint8_t {
  kNative,
  kList,
  kSearch,
  kHistory,
  kMessage,
  kAdd,
  kAccept,
  kDecline,
  kRemove,
  kBlock,
  kUnblock,
};

/** @brief C2S65010 的动作、目标游戏昵称或 #玩家编号和可选正文。 */
struct SocialRequest final {
  SocialAction action{
      SocialAction::kList};  ///< 已发布动作；未知文本在协议边界拒绝。
  std::u16string
      account_name;     ///< 目标游戏昵称或 #玩家编号；native/list 可为空。
  std::u16string text;  ///< 申请附言或私聊正文；其他动作通常为空。

  /** @brief 按动作、目标账号和正文比较。 */
  bool operator==(const SocialRequest&) const = default;
};

/** @brief S2C65011 中一条好友、申请、搜索或屏蔽状态。 */
struct SocialRosterEntry final {
  domain::PlayerId player_id;   ///< 服务端解析的目标玩家；编码到旧协议 uint32。
  std::u16string account_name;  ///< 原版通讯 UI 显示的游戏昵称。
  std::uint32_t relationship_state{
      0U};             ///< 0 搜索、1 好友、2 收到、3 发出、4 屏蔽。
  bool online{false};  ///< 是否有活动主网关会话。

  /** @brief 按玩家、账号、关系和在线状态比较。 */
  bool operator==(const SocialRosterEntry&) const = default;
};

/** @brief S2C65013 的申请发起人快照和申请正文。 */
struct SocialInvitationPayload final {
  domain::PlayerId sender_player_id;  ///< 申请发起玩家；编码到旧协议 uint32。
  std::u16string_view sender_account_name;  ///< 申请发起人的游戏昵称。
  std::u16string_view request_message;      ///< 申请附言，可为空。
};

/** @brief S2C65015 的会话定位、发言者快照和私聊正文。 */
struct NativePrivateMessagePayload final {
  domain::PlayerId peer_player_id;           ///< 客户端用于定位会话的对端玩家。
  std::u16string_view peer_account_name;     ///< 会话对端的游戏昵称。
  std::u16string_view speaker_account_name;  ///< 当前消息实际发言者的游戏昵称。
  std::u16string_view message;               ///< 私聊正文。
};

/** @brief S2C65012 兼容通知的显示标签和正文。 */
struct LegacyPrivateMessagePayload final {
  std::u16string_view label;    ///< 旧客户端扩展显示的来源标签。
  std::u16string_view message;  ///< 通知正文。
};

/** @brief 严格解析 C2S65010 的三个 UTF-16 长度前缀字段和动作枚举。 */
[[nodiscard]] SocialRequest DecodeSocialRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C65011 状态文本和 roster vector。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSocialRosterResponse(
    std::u16string_view status, std::span<const SocialRosterEntry> entries);

/** @brief 编码 S2C65013 原生好友申请推送。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSocialInvitationResponse(
    const SocialInvitationPayload& payload);

/** @brief 编码 S2C65015 原生私聊定位信息、发言者和正文。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativePrivateMessageResponse(
    const NativePrivateMessagePayload& payload);

/** @brief 编码旧扩展 S2C65012 的显示标签和正文。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLegacyPrivateMessageResponse(
    const LegacyPrivateMessagePayload& payload);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_SOCIAL_MESSAGES_H_
