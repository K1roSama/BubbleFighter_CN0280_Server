#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_TEXT_CODEC_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_TEXT_CODEC_H_

/**
 * @file text_codec.h
 * @brief 定义协议 UTF-16LE 字段与应用 UTF-8 文本之间的严格 Unicode 转换。
 */

#include <string>
#include <string_view>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {

/** @brief 将已配对 UTF-16 代码单元严格转换为 UTF-8；孤立代理项抛出
 * ProtocolError。 */
[[nodiscard]] std::string Utf16ToUtf8(std::u16string_view text);

/** @brief 将严格 UTF-8 转换为 UTF-16；过长、过短或代理值编码抛出
 * ProtocolError。 */
[[nodiscard]] std::u16string Utf8ToUtf16(std::string_view text);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_TEXT_CODEC_H_
