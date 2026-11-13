#include "features/chat/user_message.h"

#include <array>
#include <cstddef>
#include <utility>

#include "features/chat/message_policy.h"

namespace bf::domain {
namespace {

struct UnicodeRange final {
  char32_t first;
  char32_t last;
};

// 由 Python 3.14 的 unicodedata 16.0.0 生成；更新 Unicode
// 版本时必须重跑全码点对照测试。
#include "application/generated/unicode16_category_c_ranges.inc"

[[nodiscard]] bool IsCategoryC(char32_t value) noexcept {
  std::size_t first = 0U;
  std::size_t last = kUnicode16CategoryCRanges.size();
  while (first < last) {
    const auto middle = first + ((last - first) / 2U);
    const auto& range = kUnicode16CategoryCRanges[middle];
    if (value < range.first) {
      last = middle;
    } else if (value > range.last) {
      first = middle + 1U;
    } else {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool IsPythonWhitespace(char32_t value) noexcept {
  return value == 0x20 || value == 0x85 || value == 0xA0 || value == 0x1680 ||
         (value >= 0x2000 && value <= 0x200A) || value == 0x2028 ||
         value == 0x2029 || value == 0x202F || value == 0x205F ||
         value == 0x3000;
}

[[nodiscard]] bool IsHighSurrogate(char16_t value) noexcept {
  return value >= 0xD800 && value <= 0xDBFF;
}

[[nodiscard]] bool IsLowSurrogate(char16_t value) noexcept {
  return value >= 0xDC00 && value <= 0xDFFF;
}

}  // namespace

UserMessage::UserMessage(std::u16string text) : text_(std::move(text)) {}

std::variant<UserMessage, UserMessageValidationError> UserMessage::Parse(
    std::u16string_view text, bool enforce_content_policy) {
  if (text.size() > kMaximumUserMessageCodeUnits) {
    return UserMessageValidationError::kTooLong;
  }

  bool has_non_whitespace = false;
  for (std::size_t offset = 0U; offset < text.size();) {
    const auto first = text[offset++];
    char32_t value = first;
    if (IsHighSurrogate(first)) {
      if (offset >= text.size() || !IsLowSurrogate(text[offset])) {
        return UserMessageValidationError::kInvalidUnicodeCharacter;
      }
      value = 0x10000 + ((static_cast<char32_t>(first) - 0xD800) << 10U) +
              (static_cast<char32_t>(text[offset++]) - 0xDC00);
    } else if (IsLowSurrogate(first)) {
      return UserMessageValidationError::kInvalidUnicodeCharacter;
    }

    if (IsCategoryC(value)) {
      return UserMessageValidationError::kInvalidUnicodeCharacter;
    }
    has_non_whitespace = has_non_whitespace || !IsPythonWhitespace(value);
  }

  if (!has_non_whitespace) {
    return UserMessageValidationError::kEmptyOrWhitespace;
  }
  if (enforce_content_policy && IsRestrictedMessage(text))
    return UserMessageValidationError::kRestrictedContent;
  return UserMessage{std::u16string{text}};
}

const std::u16string& UserMessage::Text() const noexcept { return text_; }

std::string_view UserMessageValidationErrorMessage(
    UserMessageValidationError error) noexcept {
  switch (error) {
    case UserMessageValidationError::kRestrictedContent:
      return "消息包含禁用词，请修改后再发送";
    case UserMessageValidationError::kEmptyOrWhitespace:
    case UserMessageValidationError::kTooLong:
    case UserMessageValidationError::kInvalidUnicodeCharacter:
      return "消息为空、过长或包含控制字符";
  }
  return "消息格式无效";
}

}  // namespace bf::domain
