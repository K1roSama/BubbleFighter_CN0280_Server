#include "features/battle/match_id.h"

#include <algorithm>

namespace bf::domain {

MatchId::MatchId(ByteArray bytes) noexcept : bytes_(bytes) {}

std::optional<MatchId> MatchId::FromBytes(ByteArray bytes) noexcept {
  if (std::ranges::all_of(bytes,
                          [](std::uint8_t value) { return value == 0U; })) {
    return std::nullopt;
  }
  return MatchId{bytes};
}

std::optional<MatchId> MatchId::FromUuidString(std::string_view text) noexcept {
  if (text.size() != 36U) return std::nullopt;
  const auto digit = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  ByteArray bytes{};
  std::size_t offset = 0U;
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U)
      if (text[offset++] != '-') return std::nullopt;
    const auto high = digit(text[offset++]);
    const auto low = digit(text[offset++]);
    if (high < 0 || low < 0) return std::nullopt;
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return FromBytes(bytes);
}

const MatchId::ByteArray& MatchId::Bytes() const noexcept { return bytes_; }

std::string MatchId::ToUuidString() const {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5',
                                         '6', '7', '8', '9', 'a', 'b',
                                         'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(36U);
  for (std::size_t index = 0; index < bytes_.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      result.push_back('-');
    }
    result.push_back(digits[bytes_[index] >> 4U]);
    result.push_back(digits[bytes_[index] & 0x0FU]);
  }
  return result;
}

}  // namespace bf::domain
