#include "application/utf8.h"

#include <cstdint>

namespace bf::domain {

std::optional<DecodedCodePoint> DecodeUtf8CodePoint(
    std::string_view text, std::size_t offset) noexcept {
  if (offset >= text.size()) {
    return std::nullopt;
  }

  const auto first = static_cast<std::uint8_t>(text[offset]);
  if (first <= 0x7FU) {
    return DecodedCodePoint{.value = first, .width = 1U};
  }

  std::size_t width = 0U;
  char32_t value = 0;
  char32_t minimum = 0;
  if (first >= 0xC2U && first <= 0xDFU) {
    width = 2U;
    value = static_cast<char32_t>(first & 0x1FU);
    minimum = 0x80;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    width = 3U;
    value = static_cast<char32_t>(first & 0x0FU);
    minimum = 0x800;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    width = 4U;
    value = static_cast<char32_t>(first & 0x07U);
    minimum = 0x10000;
  } else {
    return std::nullopt;
  }

  if (width > text.size() - offset) {
    return std::nullopt;
  }
  for (std::size_t index = 1U; index < width; ++index) {
    const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
    if ((continuation & 0xC0U) != 0x80U) {
      return std::nullopt;
    }
    value = static_cast<char32_t>((value << 6U) | (continuation & 0x3FU));
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF)) {
    return std::nullopt;
  }
  return DecodedCodePoint{.value = value, .width = width};
}

std::optional<std::size_t> CountUtf8CodePoints(std::string_view text) noexcept {
  std::size_t count = 0U;
  for (std::size_t offset = 0U; offset < text.size();) {
    const auto decoded = DecodeUtf8CodePoint(text, offset);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    offset += decoded->width;
    ++count;
  }
  return count;
}

}  // namespace bf::domain
