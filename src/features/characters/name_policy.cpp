#include "features/characters/name_policy.h"

#include <algorithm>
#include <array>
#include <string>

namespace bf::domain {
namespace {
#include "features/characters/generated/name_policy_words.inc"
}  // namespace

bool IsRestrictedName(std::string_view name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (const char value : name) {
    if (value != '_') {
      normalized.push_back(value >= 'A' && value <= 'Z'
                               ? static_cast<char>(value + ('a' - 'A'))
                               : value);
    }
  }
  if (normalized.empty()) {
    return true;
  }
  for (const auto prefix : kReservedNamePrefixes) {
    if (normalized.starts_with(prefix)) {
      return true;
    }
  }
  const std::string_view text{normalized};
  for (std::size_t start = 0U; start < text.size(); ++start) {
    if ((static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) {
      continue;
    }
    for (std::size_t end = start + 1U; end <= text.size(); ++end) {
      if (end < text.size() &&
          (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        continue;
      }
      const auto candidate = text.substr(start, end - start);
      if (candidate.size() < 4U &&
          std::ranges::all_of(candidate,
                              [](unsigned char c) { return c < 0x80U; }) &&
          (start != 0U || end != text.size())) {
        continue;
      }
      if (std::binary_search(std::begin(kBlockedNameWords),
                             std::end(kBlockedNameWords), candidate)) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace bf::domain
