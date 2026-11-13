#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NICKNAME_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NICKNAME_H_

/**
 * @file nickname.h
 * @brief 定义与现有服务兼容的玩家昵称值对象及其校验结果。
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace bf::domain {

/** @brief 昵称在进入数据库用例前可能出现的稳定校验错误。 */
enum class NicknameValidationError : std::uint8_t {
  kInvalidUtf8,
  kInvalidCharacter,
  kInvalidLength,
};

/**
 * @brief 已通过字符集、长度和唯一键规范化的玩家昵称。
 * @note 允许 ASCII 字母、数字、下划线和 U+3400–U+9FFF；ASCII 权重为
 * 1，中文权重为 2。
 * @note 旧 Python 逻辑先校验字符集再做 NFKC+casefold。允许集合内跨
 * Unicode 12.1/16.0 唯一会改变的字符是 ASCII 大写字母，因此 key 精确采用 ASCII
 * 小写加原样 CJK 字节。
 */
class Nickname final {
 public:
  /** @brief 校验 UTF-8 昵称并生成数据库唯一键；失败时不产生部分值。 */
  [[nodiscard]] static std::variant<Nickname, NicknameValidationError> Parse(
      std::string_view display_name);

  /** @brief 返回客户端和资料页保留的原始大小写昵称。 */
  [[nodiscard]] const std::string& DisplayName() const noexcept;

  /** @brief 返回与旧 NFKC+casefold 规则等价的数据库唯一键。 */
  [[nodiscard]] const std::string& Key() const noexcept;

  /** @brief 按展示昵称和冻结唯一键比较。 */
  bool operator==(const Nickname&) const = default;

 private:
  Nickname(std::string display_name, std::string key);

  std::string display_name_;
  std::string key_;
};

/** @brief 返回可记录并可向本地客户端展示的中文昵称错误说明。 */
[[nodiscard]] std::string_view NicknameValidationErrorMessage(
    NicknameValidationError error) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NICKNAME_H_
