#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NATIVE_PROGRESSION_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NATIVE_PROGRESSION_MESSAGES_H_

/**
 * @file native_progression_messages.h
 * @brief 原生勋章只读快照和称号请求保护；不推测授勋或称号状态规则。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/lobby/player_menu_snapshot.h"

namespace bf::protocol {

/** @brief C2S931，BE8110 写入本人玩家和请求称号，不是 C2S935 排名请求。 */
struct NativeTitleClaimRequest final {
  domain::PlayerId self;       ///< 须与会话真实本人一致。
  std::uint32_t title_id{0U};  ///< 原始请求称号 ID，不是勋章键。
};

/** @brief C2S932，BE6D63/BE6D92 与 BE8010 确認线序为本人、0/1动作、称号。 */
struct NativeTitleSetRequest final {
  domain::PlayerId self;  ///< 须与会话真实本人一致。
  std::uint32_t action_flag{
      0U};  ///< 原版两条调用路径只传 0/1；具体控件语义尚未恢复。
  std::uint32_t title_id{
      0U};  ///< 调用路径取自 7B33D0 选中称号，不能颠倒为动作。
};

/** @brief 严格解析完整 C2S931；拒绝截断、尾随字段和空身份/称号。 */
[[nodiscard]] NativeTitleClaimRequest DecodeNativeTitleClaimRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析完整 C2S932；拒绝未知动作、截断、尾随字段和空身份/称号。 */
[[nodiscard]] NativeTitleSetRequest DecodeNativeTitleSetRequest(
    std::span<const std::uint8_t> body);

/** @brief S2C936
 * 完整失败结构：i32失败结果、两个未恢复状态DWORD零值；不声称设置成功。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativeTitleSetUnavailable();

/** @brief 原936结果1穿戴/2解除；两个读取后忽略字段保留兼容零值。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativeTitleSetResponse(
    std::int32_t result);

/** @brief S2C126
 * 使用已读取的真实账本；未知账本返回失败，不把未知当成零事件成功。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativeMedalSnapshot(
    const application::PlayerMenuSnapshot& snapshot);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NATIVE_PROGRESSION_MESSAGES_H_
