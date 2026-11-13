#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_STREAM_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_STREAM_H_

/** @file native_x_stream.h @brief 有界读取原X文件头及原MSZIP字节流。 */

#include <cstdint>
#include <span>
#include <vector>

namespace bf::runtime {
/** @brief 已解压的原X对象令牌流；尚未解释模板、层级、几何或碰撞。 */
struct NativeXStream final {
  bool binary;  ///< true为原二进制令牌，false为原文本字节。
  std::vector<std::uint8_t> payload;  ///< 不包含16字节文件头的原对象流。
};

/**
 * @brief 读取0302/0303、0032的bin/txt/bzip/tzip，保留原令牌字节。
 * @note 原X的MSZIP原样块为LEN后直接跟字节，没有RFC1951的NLEN。
 *       最大输入及原声明解压长度128MiB；每块32KiB，严格校验输入与输出范围。
 *       不依赖Windows DLL或zlib，不执行模板、XML、渲染、帧变换或物理。
 * @throws protocol::ProtocolError 格式不支持、截断、非法编码或容量超限。
 */
[[nodiscard]] NativeXStream ReadNativeXStream(
    std::span<const std::uint8_t> bytes);
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_STREAM_H_
