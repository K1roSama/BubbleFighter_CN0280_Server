#include "protocol/codec/text_codec.h"

#include <cstdint>

#include "application/utf8.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::uint32_t kHighSurrogateFirst = 0xD800U;
constexpr std::uint32_t kHighSurrogateLast = 0xDBFFU;
constexpr std::uint32_t kLowSurrogateFirst = 0xDC00U;
constexpr std::uint32_t kLowSurrogateLast = 0xDFFFU;

void AppendUtf8(std::string& output, std::uint32_t value) {
  if (value <= 0x7FU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

}  // namespace

std::string Utf16ToUtf8(std::u16string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0U; index < text.size(); ++index) {
    const auto first = static_cast<std::uint32_t>(text[index]);
    std::uint32_t value = first;
    if (first >= kHighSurrogateFirst && first <= kHighSurrogateLast) {
      if (index + 1U >= text.size()) {
        throw ProtocolError{ProtocolErrorCode::kInvalidUtf16,
                            "UTF-16 high surrogate is not paired"};
      }
      const auto second = static_cast<std::uint32_t>(text[++index]);
      if (second < kLowSurrogateFirst || second > kLowSurrogateLast) {
        throw ProtocolError{
            ProtocolErrorCode::kInvalidUtf16,
            "UTF-16 high surrogate is not followed by low surrogate"};
      }
      value = 0x10000U + ((first - kHighSurrogateFirst) << 10U) +
              (second - kLowSurrogateFirst);
    } else if (first >= kLowSurrogateFirst && first <= kLowSurrogateLast) {
      throw ProtocolError{ProtocolErrorCode::kInvalidUtf16,
                          "UTF-16 low surrogate has no leading pair"};
    }
    AppendUtf8(result, value);
  }
  return result;
}

std::u16string Utf8ToUtf16(std::string_view text) {
  std::u16string result;
  result.reserve(text.size());
  for (std::size_t offset = 0U; offset < text.size();) {
    const auto decoded = domain::DecodeUtf8CodePoint(text, offset);
    if (!decoded.has_value()) {
      throw ProtocolError{ProtocolErrorCode::kInvalidUtf8,
                          "application text is not strict UTF-8"};
    }
    const auto value = static_cast<std::uint32_t>(decoded->value);
    if (value <= 0xFFFFU) {
      result.push_back(static_cast<char16_t>(value));
    } else {
      const auto adjusted = value - 0x10000U;
      result.push_back(
          static_cast<char16_t>(kHighSurrogateFirst + (adjusted >> 10U)));
      result.push_back(
          static_cast<char16_t>(kLowSurrogateFirst + (adjusted & 0x3FFU)));
    }
    offset += decoded->width;
  }
  return result;
}

}  // namespace bf::protocol
