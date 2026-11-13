#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHAT_USER_MESSAGE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHAT_USER_MESSAGE_H_

/**
 * @file user_message.h
 * @brief 定义聊天、好友申请附言和私聊共用的 Unicode 消息值对象。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace bf::domain {

inline constexpr std::size_t kMaximumUserMessageCodeUnits =
    256U;  ///< 旧协议消息 UTF-16 上限。

/** @brief 用户消息在进入广播或持久化用例前可能产生的稳定校验错误。 */
enum class UserMessageValidationError : std::uint8_t {
  kEmptyOrWhitespace,
  kTooLong,
  kInvalidUnicodeCharacter,
  kRestrictedContent,
};

/**
 * @brief 已通过旧服务 Unicode 16.0 文本规则的非空消息。
 * @note 长度按 UTF-16 代码单元计算；拒绝未配对代理项及 Unicode General Category
 * C （控制、格式、代理、私用、未分配）字符，并拒绝仅由 Python `str.strip`
 * 空白组成的值。
 */
class UserMessage final {
 public:
  /** @brief 校验最多 256 个 UTF-16 代码单元并复制为拥有型值。 */
  [[nodiscard]] static std::variant<UserMessage, UserMessageValidationError>
  Parse(std::u16string_view text, bool enforce_content_policy = true);

  /** @brief 返回保留原始空格和字符的消息正文。 */
  [[nodiscard]] const std::u16string& Text() const noexcept;

  /** @brief 按保留的原始 UTF-16 正文比较。 */
  bool operator==(const UserMessage&) const = default;

 private:
  explicit UserMessage(std::u16string text);

  std::u16string text_;
};

/** @brief 返回可展示给本地客户端的稳定中文消息校验说明。 */
[[nodiscard]] std::string_view UserMessageValidationErrorMessage(
    UserMessageValidationError error) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHAT_USER_MESSAGE_H_
