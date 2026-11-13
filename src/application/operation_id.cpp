#include "application/operation_id.h"

#include <algorithm>
#include <array>

namespace bf::domain {

OperationId::OperationId(ByteArray bytes) noexcept : bytes_(bytes) {}

std::optional<OperationId> OperationId::FromBytes(ByteArray bytes) noexcept {
  if (std::ranges::all_of(bytes,
                          [](std::uint8_t value) { return value == 0U; })) {
    return std::nullopt;
  }
  return OperationId{bytes};
}

const OperationId::ByteArray& OperationId::Bytes() const noexcept {
  return bytes_;
}

std::string OperationId::ToUuidString() const {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5',
                                         '6', '7', '8', '9', 'a', 'b',
                                         'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(36U);
  for (std::size_t index = 0U; index < bytes_.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      result.push_back('-');
    }
    result.push_back(digits[bytes_[index] >> 4U]);
    result.push_back(digits[bytes_[index] & 0x0FU]);
  }
  return result;
}

}  // namespace bf::domain
