#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_BLOCK_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_BLOCK_MESSAGES_H_

/** @file chat_block_messages.h
 *  @brief Build 1302 C2S147/148 与 S2C170/171/172 的字节契约。
 */
#include <span>

#include "features/chat/chat_blocks.h"

namespace bf::protocol {
[[nodiscard]] application::ChatBlockTarget DecodeChatBlockTarget(
    std::span<const std::uint8_t> body);
[[nodiscard]] std::vector<std::uint8_t> EncodeChatBlockList(
    std::span<const application::ChatBlockEntry> entries);
[[nodiscard]] std::vector<std::uint8_t> EncodeChatBlockAdded(
    const application::ChatBlockEntry& entry);
[[nodiscard]] std::vector<std::uint8_t> EncodeChatBlockRemoved(
    const application::ChatBlockEntry& entry);
}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CHAT_CHAT_BLOCK_MESSAGES_H_
