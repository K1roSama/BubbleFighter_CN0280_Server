#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_PACKET_IO_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_PACKET_IO_H_

/**
 * @file packet_io.h
 * @brief 定义 BubbleFighter 明文负载的小端读写器与 UTF-16LE 字段约束。
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bf::protocol {

/**
 * @brief 协议解析失败的稳定分类。
 * @note 分类只描述不可信字节的问题；业务状态错误使用
 * application::ServiceError。
 */
enum class ProtocolErrorCode : std::uint8_t {
  kTruncatedPayload,
  kTrailingPayload,
  kLengthLimit,
  kInvalidUtf16,
  kChecksumMismatch,
  kInvalidFrame,
  kReceiveBufferLimit,
  kInvalidUtf8,
  kInvalidValue,
};

/** @brief 返回可安全写入结构化日志的稳定协议错误名。 */
[[nodiscard]] std::string_view ProtocolErrorCodeName(
    ProtocolErrorCode code) noexcept;

/**
 * @brief 携带安全诊断信息的协议异常。
 * @note 网关捕获后只记录错误分类、opcode 和关联
 * ID，不记录密码、票据或完整负载。
 */
class ProtocolError final : public std::runtime_error {
 public:
  /**
   * @brief 构造一个协议错误。
   * @param code 稳定错误分类。
   * @param message 不包含客户端秘密或原始负载的诊断消息。
   */
  ProtocolError(ProtocolErrorCode code, std::string_view message);

  /** @brief 返回稳定错误分类。 */
  [[nodiscard]] ProtocolErrorCode Code() const noexcept;

 private:
  ProtocolErrorCode code_;
};

/**
 * @brief 在借用负载上顺序读取固定宽度小端字段。
 * @note Reader 不拥有字节，生命周期不得超过传入
 * span；不得跨异步边界保存本对象。
 */
class PacketReader final {
 public:
  /** @brief 借用完整消息负载并从偏移零开始读取。 */
  explicit PacketReader(std::span<const std::uint8_t> payload) noexcept;

  /** @brief 读取一个无符号字节。 */
  [[nodiscard]] std::uint8_t ReadU8();
  /** @brief 读取一个有符号字节，保留原始二进制位。 */
  [[nodiscard]] std::int8_t ReadI8();
  /** @brief 读取一个小端 uint16。 */
  [[nodiscard]] std::uint16_t ReadU16();
  /** @brief 读取一个小端 int16，保留补码位模式。 */
  [[nodiscard]] std::int16_t ReadI16();
  /** @brief 读取一个小端 uint32。 */
  [[nodiscard]] std::uint32_t ReadU32();
  /** @brief 读取一个小端 int32，保留补码位模式。 */
  [[nodiscard]] std::int32_t ReadI32();
  /** @brief 读取一个小端 uint64。 */
  [[nodiscard]] std::uint64_t ReadU64();
  /** @brief 读取一个 IEEE-754 小端 32 位浮点字段。 */
  [[nodiscard]] float ReadF32();
  /** @brief 读取一个 IEEE-754 小端 64 位浮点字段。 */
  [[nodiscard]] double ReadF64();

  /**
   * @brief 读取 vector 的 uint32 元素数并立即执行容量检查。
   * @param maximum_elements 该具体协议字段允许的最大元素数。
   */
  [[nodiscard]] std::uint32_t ReadCount(std::size_t maximum_elements);

  /**
   * @brief 读取以 uint32 UTF-16 代码单元数开头的 UTF-16LE 字符串。
   * @param maximum_code_units 字段允许的最大 UTF-16 代码单元数，不是 Unicode
   * 码点数。
   * @return 已验证代理项配对的 UTF-16 字符串。
   */
  [[nodiscard]] std::u16string ReadUtf16(
      std::size_t maximum_code_units = 4096U);

  /**
   * @brief 借用接下来的指定字节并推进游标。
   * @param count 要读取的字节数。
   * @return 指向原负载的同步借用视图。
   */
  [[nodiscard]] std::span<const std::uint8_t> ReadBytes(std::size_t count);

  /** @brief 要求负载恰好读完，拒绝未解释尾随字段。 */
  void RequireEnd() const;

  /** @brief 返回尚未读取的字节数。 */
  [[nodiscard]] std::size_t RemainingBytes() const noexcept;

 private:
  std::span<const std::uint8_t> payload_;
  std::size_t position_{0U};
};

/**
 * @brief 以固定小端和 UTF-16LE 规则构造一条消息负载。
 * @note Writer 独占内部 vector，调用 release
 * 后转移所有权，适合进入异步发送队列。
 */
class PacketWriter final {
 public:
  /** @brief 追加一个无符号字节。 */
  void WriteU8(std::uint8_t value);
  /** @brief 追加一个有符号字节的原始位模式。 */
  void WriteI8(std::int8_t value);
  /** @brief 追加一个小端 uint16。 */
  void WriteU16(std::uint16_t value);
  /** @brief 追加一个小端 int16 的补码位模式。 */
  void WriteI16(std::int16_t value);
  /** @brief 追加一个小端 uint32。 */
  void WriteU32(std::uint32_t value);
  /** @brief 追加一个小端 int32 的补码位模式。 */
  void WriteI32(std::int32_t value);
  /** @brief 追加一个小端 uint64。 */
  void WriteU64(std::uint64_t value);
  /** @brief 追加一个 IEEE-754 小端 32 位浮点字段。 */
  void WriteF32(float value);
  /** @brief 追加一个 IEEE-754 小端 64 位浮点字段。 */
  void WriteF64(double value);

  /**
   * @brief 写入 uint32 代码单元数和 UTF-16LE 内容。
   * @param value 必须包含合法的代理项配对；长度按代码单元计算。
   */
  void WriteUtf16(std::u16string_view value);

  /** @brief 复制追加原始字段字节。 */
  void WriteBytes(std::span<const std::uint8_t> value);

  /** @brief 只读查看当前负载；视图在下一次写入后可能失效。 */
  [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept;

  /** @brief 转移已构造负载的所有权并清空 Writer。 */
  [[nodiscard]] std::vector<std::uint8_t> Release() noexcept;

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_PACKET_IO_H_
