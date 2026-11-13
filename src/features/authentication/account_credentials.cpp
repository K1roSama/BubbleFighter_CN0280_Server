#include "features/authentication/account_credentials.h"

#include <algorithm>
#include <utility>

#include "application/utf8.h"

namespace bf::domain {
namespace {

bool IsAccountCharacter(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '_';
}

char AsciiLower(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value + ('a' - 'A'));
  }
  return value;
}

}  // namespace

AccountName::AccountName(std::string display_name, std::string key)
    : display_name_(std::move(display_name)), key_(std::move(key)) {}

std::variant<AccountName, AccountNameValidationError> AccountName::Parse(
    std::string_view display_name) {
  if (display_name.size() < 3U || display_name.size() > 24U) {
    return AccountNameValidationError::kInvalidLength;
  }
  if (!std::ranges::all_of(display_name, IsAccountCharacter)) {
    return AccountNameValidationError::kInvalidCharacter;
  }

  std::string key{display_name};
  std::ranges::transform(key, key.begin(), AsciiLower);
  return AccountName{std::string{display_name}, std::move(key)};
}

const std::string& AccountName::DisplayName() const noexcept {
  return display_name_;
}

const std::string& AccountName::Key() const noexcept { return key_; }

std::optional<PasswordValidationError> ValidatePassword(
    std::string_view password) noexcept {
  const auto count = CountUtf8CodePoints(password);
  if (!count.has_value()) {
    return PasswordValidationError::kInvalidUtf8;
  }
  if (*count < 8U || *count > 64U) {
    return PasswordValidationError::kInvalidLength;
  }
  return std::nullopt;
}

std::string_view AccountNameValidationErrorMessage(
    AccountNameValidationError error) noexcept {
  switch (error) {
    case AccountNameValidationError::kInvalidCharacter:
    case AccountNameValidationError::kInvalidLength:
      return "账号须为 3～24 位英文字母、数字或下划线";
  }
  return "账号格式无效";
}

std::string_view PasswordValidationErrorMessage(
    PasswordValidationError error) noexcept {
  switch (error) {
    case PasswordValidationError::kInvalidUtf8:
      return "密码不是有效的 UTF-8 文本";
    case PasswordValidationError::kInvalidLength:
      return "密码须为 8～64 个字符";
  }
  return "密码格式无效";
}

}  // namespace bf::domain
