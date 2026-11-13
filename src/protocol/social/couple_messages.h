#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COUPLE_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COUPLE_MESSAGES_H_

/**
 * @file couple_messages.h
 * @brief 构建号 752 原版情侣请求和通知的严格编解码。
 */

#include "features/lobby/player_menu_snapshot.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {

// Build 752: DA9780 / DA9830 / DA9600 / DA96B0 / 774D90.
/** @brief 完整解析原版邀请及装备预览；外观仍从服务端读取。 */
struct CoupleInviteRequest final {
  bool confirmed{false};
  std::u16string target;
  std::uint32_t ring{0U};
  std::u16string message;
  std::uint32_t equipment_owner{0U};
};

/** @brief 绑定双方昵称的原版邀请答复。 */
struct CoupleAnswerRequest final {
  bool flag{false};
  bool accepted{false};
  std::u16string inviter;
  std::u16string recipient;
};

/** @brief 原版更换情侣戒指请求。 */
struct CoupleRingRequest final {
  std::uint32_t ring{0U};
  bool flag{false};
};

[[nodiscard]] CoupleInviteRequest DecodeCoupleInvite(
    std::span<const std::uint8_t> body);
[[nodiscard]] CoupleAnswerRequest DecodeCoupleAnswer(
    std::span<const std::uint8_t> body);
[[nodiscard]] bool DecodeCoupleCancel(std::span<const std::uint8_t> body);
[[nodiscard]] std::u16string DecodeCoupleBreakup(
    std::span<const std::uint8_t> body);
[[nodiscard]] CoupleRingRequest DecodeCoupleRing(
    std::span<const std::uint8_t> body);
[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleInvite(
    const application::PlayerMenuSnapshot& inviter,
    std::u16string_view recipient, std::u16string_view message);
[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleResult(
    std::uint32_t result, std::u16string_view target);
[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleAnswer(
    bool accepted, std::u16string_view inviter, std::u16string_view recipient);
[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleBreakup(
    std::u16string_view actor, std::u16string_view partner,
    std::uint32_t result);
[[nodiscard]] std::vector<std::uint8_t> EncodeCouplePartner(
    const std::optional<application::CoupleProfileView>& couple);
[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleRingResult(
    std::int32_t result, std::uint32_t ring, bool flag);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SOCIAL_COUPLE_MESSAGES_H_
