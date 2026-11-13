#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_UTF8_PATH_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_UTF8_PATH_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace bf::infrastructure {

/** @brief Convert a UTF-8 path using the C++20 char8_t filesystem interface. */
[[nodiscard]] inline std::filesystem::path Utf8Path(std::string_view value) {
  return std::filesystem::path{std::u8string{value.begin(), value.end()}};
}

}  // namespace bf::infrastructure

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_UTF8_PATH_H_
