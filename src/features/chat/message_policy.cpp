#include "features/chat/message_policy.h"

#include <algorithm>
#include <array>
#include <string>

#include "application/utf8.h"

namespace bf::domain {
namespace {
#include "features/chat/generated/message_policy_words.inc"

void AppendUtf8(std::string& result, char32_t c) {
  if (c < 0x80)
    result.push_back(static_cast<char>(c));
  else if (c < 0x800) {
    result.push_back(static_cast<char>(0xC0 | (c >> 6)));
    result.push_back(static_cast<char>(0x80 | (c & 63)));
  } else if (c < 0x10000) {
    result.push_back(static_cast<char>(0xE0 | (c >> 12)));
    result.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    result.push_back(static_cast<char>(0x80 | (c & 63)));
  } else {
    result.push_back(static_cast<char>(0xF0 | (c >> 18)));
    result.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
    result.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    result.push_back(static_cast<char>(0x80 | (c & 63)));
  }
}

bool AsciiWord(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
}  // namespace

bool IsRestrictedMessage(std::string_view utf8) {
  if (utf8.size() > 4096U) return true;
  std::string compact, spaced;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto decoded = DecodeUtf8CodePoint(utf8, i);
    if (!decoded) return true;
    i += decoded->width;
    auto c = decoded->value;
    if (c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;
    if (c >= U'A' && c <= U'Z') c += U'a' - U'A';
    if ((c < 128 && AsciiWord(static_cast<unsigned char>(c))) ||
        (c >= 0x3400 && c <= 0x9FFF)) {
      AppendUtf8(compact, c);
      AppendUtf8(spaced, c);
    } else
      spaced.push_back(' ');
  }
  // Short Latin abbreviations need token boundaries, avoiding matches inside
  // ordinary words.
  for (const auto word : kShortMessageWords) {
    for (auto pos = spaced.find(word); pos != std::string::npos;
         pos = spaced.find(word, pos + 1)) {
      if ((pos == 0 ||
           !AsciiWord(static_cast<unsigned char>(spaced[pos - 1]))) &&
          (pos + word.size() == spaced.size() ||
           !AsciiWord(static_cast<unsigned char>(spaced[pos + word.size()]))))
        return true;
    }
  }
  const std::string_view text{compact};
  for (std::size_t start = 0; start < text.size(); ++start) {
    if ((static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) continue;
    for (std::size_t end = start + 1;
         end <= std::min(text.size(), start + kMaximumMessageWordBytes);
         ++end) {
      if (end < text.size() &&
          (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
        continue;
      if (std::binary_search(std::begin(kBlockedMessageWords),
                             std::end(kBlockedMessageWords),
                             text.substr(start, end - start)))
        return true;
    }
  }
  return false;
}

bool IsRestrictedMessage(std::u16string_view text) {
  if (text.size() > 1024U) return true;
  std::string utf8;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char32_t c = text[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 == text.size() || text[i + 1] < 0xDC00 || text[i + 1] > 0xDFFF)
        return true;
      c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
    } else if (c >= 0xDC00 && c <= 0xDFFF)
      return true;
    AppendUtf8(utf8, c);
  }
  return IsRestrictedMessage(utf8);
}
}  // namespace bf::domain
