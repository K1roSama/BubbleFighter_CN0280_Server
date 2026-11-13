#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MESSAGE_POLICY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MESSAGE_POLICY_H_

/**
 * @file message_policy.h
 * @brief 聊天词库不复用昵称保留前缀规则，输入必须是合法 Unicode。
 */
#include <string_view>

namespace bf::domain {
// Shared offline lexicon for player-authored chat; reserved name prefixes do
// not apply.
[[nodiscard]] bool IsRestrictedMessage(std::string_view utf8);
[[nodiscard]] bool IsRestrictedMessage(std::u16string_view utf16);
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MESSAGE_POLICY_H_
