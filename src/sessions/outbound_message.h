#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_OUTBOUND_MESSAGE_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_OUTBOUND_MESSAGE_H_

/**
 * @file outbound_message.h
 * @brief 定义运行时交给会话顺序写队列的独占出站消息值。
 */

#include <cstdint>
#include <vector>

namespace bf::runtime {

/** @brief 已完成业务映射、等待 Session 顺序写队列编码成 frame 的消息。 */
struct OutboundMessage final {
  std::uint16_t opcode{0U};        ///< 原版客户端 S2C opcode。
  std::vector<std::uint8_t> body;  ///< 独占负载，可安全移动到异步写队列。

  /** @brief 按 opcode 和完整拥有型正文比较。 */
  bool operator==(const OutboundMessage&) const = default;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_OUTBOUND_MESSAGE_H_
