#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_UTF8_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_UTF8_H_

/**
 * @file utf8.h
 * @brief 定义领域输入共用的严格 UTF-8 标量值解码工具。
 */

#include <cstddef>
#include <optional>
#include <string_view>

namespace bf::domain {

/** @brief 一个已验证的 Unicode 标量值及其在 UTF-8 输入中占用的字节数。 */
struct DecodedCodePoint final {
  char32_t value{0};     ///< Unicode 标量值；永不处于代理项区间。
  std::size_t width{0};  ///< 从当前位置消费的 UTF-8 字节数，范围为 1～4。
};

/**
 * @brief 从指定字节偏移严格解码一个 UTF-8 标量值。
 * @param text 完整输入；其生命周期只需覆盖本次同步调用。
 * @param offset 必须小于 text.size() 的代码点起始偏移。
 * @return 成功时返回值和宽度；截断、过长编码、代理项或越界值返回空。
 */
[[nodiscard]] std::optional<DecodedCodePoint> DecodeUtf8CodePoint(
    std::string_view text, std::size_t offset) noexcept;

/** @brief 严格校验完整 UTF-8 输入并返回 Unicode 标量值数量；非法输入返回空。 */
[[nodiscard]] std::optional<std::size_t> CountUtf8CodePoints(
    std::string_view text) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_UTF8_H_
