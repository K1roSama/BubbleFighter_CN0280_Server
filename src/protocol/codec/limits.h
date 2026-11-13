#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_LIMITS_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_LIMITS_H_

/**
 * @file limits.h
 * @brief 定义所有连接在进入协议解析前必须遵守的内存与包长边界。
 */

#include <cstddef>
#include <optional>

#include "application/error.h"

namespace bf::protocol {

/**
 * @brief 单连接协议资源上限。
 * @note 默认完整帧上限包含 5 字节帧头，并覆盖旧服务 C2S21 允许的最大
 *       524288 个 UTF-16 代码单元、4 字节实际长度字段和兼容帧头。
 */
struct ProtocolLimits final {
  std::size_t maximum_packet_bytes{
      1'048'585U};  ///< 单个完整帧允许的最大字节数。
  std::size_t maximum_send_queue_bytes{std::size_t{4} * 1024U *
                                       1024U};  ///< 单连接待发送缓冲总上限。
};

/**
 * @brief 校验协议边界能否形成有界且可执行的连接配置。
 * @param limits 待校验的包长和发送队列上限。
 * @return 合法时为空；否则返回不含配置原值的安全错误。
 */
[[nodiscard]] std::optional<application::ServiceError> ValidateProtocolLimits(
    const ProtocolLimits& limits);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_LIMITS_H_
