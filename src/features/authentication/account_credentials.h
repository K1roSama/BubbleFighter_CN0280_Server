#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_CREDENTIALS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_CREDENTIALS_H_

/**
 * @file account_credentials.h
 * @brief 定义与现有启动器兼容的账号名和密码输入规则。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace bf::domain {

/** @brief 账号名在进入事务前可能出现的稳定校验错误。 */
enum class AccountNameValidationError : std::uint8_t {
  kInvalidCharacter,
  kInvalidLength,
};

/**
 * @brief 已通过启动器兼容规则并生成数据库唯一键的账号名。
 * @note 只允许 3～24 个 ASCII
 * 字母、数字或下划线；展示名保留大小写，唯一键折叠为小写。
 */
class AccountName final {
 public:
  /** @brief 校验展示账号名并生成唯一键；失败时不产生部分值。 */
  [[nodiscard]] static std::variant<AccountName, AccountNameValidationError>
  Parse(std::string_view display_name);

  /** @brief 返回注册时保留的原始大小写账号名。 */
  [[nodiscard]] const std::string& DisplayName() const noexcept;

  /** @brief 返回用于数据库唯一约束和不区分 ASCII 大小写查找的键。 */
  [[nodiscard]] const std::string& Key() const noexcept;

  /** @brief 按展示名和唯一键比较两个账号名值。 */
  bool operator==(const AccountName&) const = default;

 private:
  AccountName(std::string display_name, std::string key);

  std::string display_name_;
  std::string key_;
};

/** @brief 密码输入校验失败的稳定原因；密码正文不得进入错误信息。 */
enum class PasswordValidationError : std::uint8_t {
  kInvalidUtf8,
  kInvalidLength,
};

/**
 * @brief 校验密码是 8～64 个可编码 Unicode 标量值组成的 UTF-8 文本。
 * @note 与 Python `len(str)` 的现有边界一致；该函数只借用输入且不保存密码。
 */
[[nodiscard]] std::optional<PasswordValidationError> ValidatePassword(
    std::string_view password) noexcept;

/** @brief 返回不含输入正文的中文账号名错误说明。 */
[[nodiscard]] std::string_view AccountNameValidationErrorMessage(
    AccountNameValidationError error) noexcept;

/** @brief 返回不含输入正文的中文密码错误说明。 */
[[nodiscard]] std::string_view PasswordValidationErrorMessage(
    PasswordValidationError error) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_ACCOUNT_CREDENTIALS_H_
