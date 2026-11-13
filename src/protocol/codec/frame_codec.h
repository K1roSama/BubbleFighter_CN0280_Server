#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_FRAME_CODEC_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_FRAME_CODEC_H_

/**
 * @file frame_codec.h
 * @brief 定义 0.2.80.1 客户端的 hello、旧 cipher、帧头和增量拆包契约。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {

/** @brief 客户端/服务端 cipher 状态的已验证方向。 */
enum class CipherEndpoint : std::uint8_t {
  kClientToServer,
  kServerToClient,
};

/** @brief 同一帧格式的两个消息空间；仅主网关 21 存在历史 UTF-16 长度溢出。 */
enum class FrameNamespace : std::uint8_t { kGateway, kRelay };

/**
 * @brief 实现客户端 6AA000/6AA070 使用的逐字节可逆变换。
 * @note 每条 TCP
 * 连接的两个方向必须各自持有一个实例；索引随字节连续推进，不能并发调用。
 * @warning 这是旧游戏协议的混淆算法，不提供现代传输机密性或认证能力。
 */
class LegacyCipher final {
 public:
  /**
   * @brief 使用明确索引建立 cipher 状态。
   * @param table_a_index A 表索引，范围 0..48。
   * @param table_b_index B 表索引，范围 0..23。
   */
  LegacyCipher(std::uint8_t table_a_index, std::uint8_t table_b_index);

  /** @brief 建立方向对应的已验证初始状态：C2S=0/1，S2C=5/3。 */
  [[nodiscard]] static LegacyCipher ForEndpoint(CipherEndpoint endpoint);

  /** @brief 原地加密并推进本方向状态；调用顺序必须与 socket 写入顺序相同。 */
  void Encrypt(std::span<std::uint8_t> bytes) noexcept;

  /** @brief 原地解密并推进本方向状态；调用顺序必须与 socket 读取顺序相同。 */
  void Decrypt(std::span<std::uint8_t> bytes) noexcept;

 private:
  void Transform(std::span<std::uint8_t> bytes, bool is_decrypting) noexcept;

  std::uint8_t table_a_index_;
  std::uint8_t table_b_index_;
};

/** @brief 一条已校验但尚未映射业务语义的客户端消息。 */
struct Packet final {
  std::uint16_t opcode;            ///< 当前连接命名空间内的消息编号。
  std::vector<std::uint8_t> body;  ///< 不包含 5 字节头的明文负载。

  /** @brief 按 opcode 和完整正文比较。 */
  bool operator==(const Packet&) const = default;
};

/** @brief 返回负载所有字节的 XOR 校验值。 */
[[nodiscard]] std::uint8_t PayloadChecksum(
    std::span<const std::uint8_t> body) noexcept;

/**
 * @brief 编码普通 5 字节明文帧。
 * @param opcode 当前连接命名空间内的消息编号。
 * @param body 不超过 uint16 长度的负载。
 * @return `u16 length + u16 opcode + u8 xor + body` 的独占缓冲。
 * @throws ProtocolError 当普通负载超过 65535 字节。
 * @note C2S21 是客户端历史溢出兼容，只允许 Decoder 接收，服务端不生成该异常帧。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeFrame(
    std::uint16_t opcode, std::span<const std::uint8_t> body = {});

/**
 * @brief 返回登录前固定 258 字节 hello：小端版本 2 与 256 个零字节。
 * @note 该 hello 未经过 LegacyCipher，必须在建立加密帧状态前发送。
 */
[[nodiscard]] std::array<std::uint8_t, 258U> MakeServerHello() noexcept;

/**
 * @brief 增量解密并拆分 TCP 字节流，支持头/负载分片与多帧粘包。
 * @note 单连接单线程/strand
 * 使用。发生错误后状态不可恢复，调用方应记录分类并断开连接。
 */
class FrameDecoder final {
 public:
  /**
   * @brief 建立 C2S decoder。
   * @param maximum_frame_bytes 完整明文帧（含 5 字节头）的上限。
   * @param maximum_environment_code_units C2S21 报告允许的 UTF-16
   * 代码单元上限。
   */
  explicit FrameDecoder(
      std::size_t maximum_frame_bytes = 1'048'585U,
      std::size_t maximum_environment_code_units = 524'288U,
      FrameNamespace message_namespace = FrameNamespace::kGateway);

  /**
   * @brief 消费一段按连接顺序到达的密文字节，并返回本次形成的所有完整包。
   * @param encrypted_bytes 借用的网络读取结果；函数返回后不保留该视图。
   * @return 按流顺序排列的完整消息。
   * @throws ProtocolError 校验、长度、历史 C2S21 结构或累计残包越界。
   */
  [[nodiscard]] std::vector<Packet> Feed(
      std::span<const std::uint8_t> encrypted_bytes);

  /** @brief 返回当前等待后续 TCP 字节的明文残包大小。 */
  [[nodiscard]] std::size_t BufferedBytes() const noexcept;

 private:
  [[nodiscard]] std::size_t FrameBodyBytes(
      std::span<const std::uint8_t> available) const;

  LegacyCipher cipher_;
  std::vector<std::uint8_t> buffer_;
  std::size_t maximum_frame_bytes_;
  std::size_t maximum_environment_code_units_;
  FrameNamespace message_namespace_;
};

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CODEC_FRAME_CODEC_H_
