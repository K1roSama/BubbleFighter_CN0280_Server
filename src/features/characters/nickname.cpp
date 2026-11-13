#include "features/characters/nickname.h"

#include <cstddef>
#include <utility>

#include "application/utf8.h"

namespace bf::domain {
namespace {

bool IsAsciiNicknameCharacter(char32_t value) noexcept {
  return (value >= U'A' && value <= U'Z') || (value >= U'a' && value <= U'z') ||
         (value >= U'0' && value <= U'9') || value == U'_';
}

}  // namespace

Nickname::Nickname(std::string display_name, std::string key)
    : display_name_(std::move(display_name)), key_(std::move(key)) {}

std::variant<Nickname, NicknameValidationError> Nickname::Parse(
    std::string_view display_name) {
  std::size_t code_point_count = 0U;
  std::size_t weighted_length = 0U;
  std::string normalized_key;
  normalized_key.reserve(display_name.size());

  for (std::size_t offset = 0U; offset < display_name.size();) {
    const auto decoded = DecodeUtf8CodePoint(display_name, offset);
    if (!decoded.has_value()) {
      return NicknameValidationError::kInvalidUtf8;
    }

    const bool is_ascii = IsAsciiNicknameCharacter(decoded->value);
    const bool is_cjk = decoded->value >= 0x3400 && decoded->value <= 0x9FFF;
    if (!is_ascii && !is_cjk) {
      return NicknameValidationError::kInvalidCharacter;
    }

    ++code_point_count;
    weighted_length += is_cjk ? 2U : 1U;
    if (decoded->value >= U'A' && decoded->value <= U'Z') {
      normalized_key.push_back(
          static_cast<char>(decoded->value + (U'a' - U'A')));
    } else {
      normalized_key.append(display_name.substr(offset, decoded->width));
    }
    offset += decoded->width;
  }

  if (code_point_count < 2U || code_point_count > 12U || weighted_length < 4U ||
      weighted_length > 12U) {
    return NicknameValidationError::kInvalidLength;
  }
  return Nickname{std::string{display_name}, std::move(normalized_key)};
}

const std::string& Nickname::DisplayName() const noexcept {
  return display_name_;
}

const std::string& Nickname::Key() const noexcept { return key_; }

std::string_view NicknameValidationErrorMessage(
    NicknameValidationError error) noexcept {
  switch (error) {
    case NicknameValidationError::kInvalidUtf8:
      return "昵称不是有效的 UTF-8 文本";
    case NicknameValidationError::kInvalidCharacter:
    case NicknameValidationError::kInvalidLength:
      return "昵称须为中文 2～6 字或英文 4～12 字，可使用数字或下划线";
  }
  return "昵称格式无效";
}

}  // namespace bf::domain
