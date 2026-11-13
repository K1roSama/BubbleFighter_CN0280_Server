#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_KEY_MAP_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_KEY_MAP_MESSAGES_H_

/** @file key_map_messages.h
 *  @brief IDA 79F3A0/473C90 与 B84920/473CF0 的按键协议。
 */
#include <span>

#include "features/inventory/key_map.h"

namespace bf::protocol {
/** @brief 严格解码 C2S2016：DWORD reset、数量、{长度前缀 UTF16 动作、DWORD
 * 键码}。 */
[[nodiscard]] application::KeyMapSettings DecodeKeyMapRequest(
    std::span<const std::uint8_t> body);
/** @brief S2C981：DWORD apply、同格式的按键表；失败不改变客户端当前按键。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeKeyMapResponse(
    bool apply, std::span<const application::KeyBinding> bindings);
}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_KEY_MAP_MESSAGES_H_
