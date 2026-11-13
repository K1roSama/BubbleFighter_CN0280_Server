#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_TRANSPORT_CORE_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_TRANSPORT_CORE_H_

/**
 * @file session_transport_core.h
 * @brief 定义独立于 Asio 的单连接拆包、顺序加密、有界写队列与关闭状态核心。
 */

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/codec/limits.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/** @brief 连接传输核心是否仍接受输入、正在收尾或已不可再使用。 */
enum class SessionTransportState : std::uint8_t {
  kOpen,
  kClosing,
  kClosed,
};

/** @brief 关闭时排空已接受消息，或在当前在途写结束后立即丢弃其余消息。 */
enum class SessionCloseMode : std::uint8_t {
  kDrain,
  kAbort,
};

/**
 * @brief 一次异步写启动所需的稳定序号和借用字节视图。
 * @warning bytes 只在对应 `complete_write`、`fail_write`
 * 或核心析构前有效；异步回调必须 捕获 sequence，不能捕获队列元素引用。
 */
struct SessionWriteView final {
  std::uint64_t sequence{0U};  ///< 用于拒绝重复或过期 completion 的单调序号。
  std::span<const std::uint8_t>
      bytes{};  ///< 已决定是否加密、可直接交给 socket 的连续字节。
};

/** @brief `async_write` 成功 completion 的不可混淆值参数。 */
struct SessionWriteCompletion final {
  std::uint64_t sequence{0U};   ///< 启动写入时取得的队首序号。
  std::size_t transferred{0U};  ///< completion 报告的完整传输字节数。
};

/**
 * @brief 单个 Session strand 独占的传输状态核心。
 * @note 构造时自动把未加密 258 字节 hello
 * 放入队列。业务消息按批次先完整编码，再原子入队并 使用唯一 S2C cipher
 * 顺序加密；800/801/802 的超长正文按已部署客户端适配器协议切成 65000 分块，其他
 * opcode 仍受普通 u16 帧限制。可靠消息批次不会只入队一半；32769/32885
 *       移动状态在尚未开始 socket
 * 写入前按对象保留最新值，队列压力下优先丢弃旧移动状态。类内 不执行 socket
 * I/O，Asio 适配层只能按 `begin_write`/completion 契约驱动它。
 */
class SessionTransportCore final {
 public:
  /**
   * @brief 使用协议资源上限创建连接；非法上限抛出 `std::invalid_argument`。
   * @note 发送队列上限必须额外容纳固定 hello；接收帧上限由同一配置传给增量
   * decoder。
   */
  explicit SessionTransportCore(
      protocol::ProtocolLimits limits = protocol::ProtocolLimits{},
      protocol::FrameNamespace message_namespace =
          protocol::FrameNamespace::kGateway);

  SessionTransportCore(const SessionTransportCore&) = delete;
  SessionTransportCore& operator=(const SessionTransportCore&) = delete;

  /**
   * @brief 消费一段 C2S 密文字节并按线序返回完整消息。
   * @throws protocol::ProtocolError 畸形输入；抛出前核心会进入 abort 关闭流程。
   * @return 非 open 状态返回 `kInvalidState`，不会继续推进 cipher。
   */
  [[nodiscard]] application::ServiceResult<std::vector<protocol::Packet>>
  FeedReceived(std::span<const std::uint8_t> encrypted_bytes);

  /**
   * @brief 把一批业务响应编码、顺序加密并放入有界队列。
   * @return 成功时为包含在途 buffer
   * 和待编码最新移动状态的当前队列字节数；可靠消息超过上限 时返回 `kOverloaded`
   * 并自动转为 abort 关闭，移动状态超过上限时丢弃并返回当前字节数。
   * @throws protocol::ProtocolError 任一非 800/801/802 负载超过普通 u16
   * 帧长，或扩展正文无法用 u32 表示时抛出。
   */
  [[nodiscard]] application::ServiceResult<std::size_t> Enqueue(
      std::span<const OutboundMessage> messages);

  /** @brief 单消息便利入口；语义与批量入口相同。 */
  [[nodiscard]] application::ServiceResult<std::size_t> Enqueue(
      const OutboundMessage& message);

  /**
   * @brief 若没有在途写且队列非空，锁定队首并返回写视图；否则返回空。
   * @note 同一时刻最多一项在途，保证 TCP 字节顺序与 S2C cipher 推进顺序一致。
   */
  [[nodiscard]] std::optional<SessionWriteView> BeginWrite();

  /**
   * @brief 提交一次成功的 `async_write` completion，并释放对应队首所有权。
   * @param completion 序号必须来自
   * `begin_write`，字节数必须等于视图完整长度；短写视为
   *                   传输契约失败并关闭连接。
   */
  [[nodiscard]] std::optional<application::ServiceError> CompleteWrite(
      SessionWriteCompletion completion);

  /** @brief 提交一次 socket 写失败；校验序号后清空队列并进入 closed。 */
  [[nodiscard]] std::optional<application::ServiceError> FailWrite(
      std::uint64_t sequence);

  /**
   * @brief 幂等开始关闭；drain 排空已接受消息，abort 只保留当前在途 buffer 直到
   * completion。
   * @note 调用后立即拒绝新收包和新出站消息；无在途/待写数据时直接进入 closed。
   */
  void BeginClose(SessionCloseMode mode);

  /** @brief 返回当前传输生命周期状态。 */
  [[nodiscard]] SessionTransportState State() const noexcept;
  /** @brief 返回含当前在途 buffer 的待发送总字节数。 */
  [[nodiscard]] std::size_t QueuedBytes() const noexcept;
  /** @brief 返回待写 buffer 数；一个批次可包含多帧，待编码移动快照计为一个
   * buffer。 */
  [[nodiscard]] std::size_t QueuedBuffers() const noexcept;
  /** @brief 返回 decoder 当前保存的明文残包字节数。 */
  [[nodiscard]] std::size_t BufferedReceiveBytes() const noexcept;
  /** @brief 返回当前是否已有一个异步写在途。 */
  [[nodiscard]] bool WriteInProgress() const noexcept;

  /** @brief 返回尚未编码的移动包中，被后续状态替换的次数。 */
  [[nodiscard]] std::size_t MovementCoalescedCount() const noexcept;
  /** @brief 返回因发送预算不足或关闭流程而被主动丢弃的移动包次数。 */
  [[nodiscard]] std::size_t MovementDroppedCount() const noexcept;
  /** @brief 返回当前仍等待编码的不同移动对象数量。 */
  [[nodiscard]] std::size_t PendingMovementCount() const noexcept;

 private:
  /** @brief 移动状态以协议变体和对象号区分，避免压缩/非压缩帧互相覆盖。 */
  struct MovementKey final {
    std::uint16_t opcode{0U};
    std::uint32_t actor_object_id{0U};

    auto operator<=>(const MovementKey&) const = default;
  };

  /** @brief 队列元素拥有 socket completion 前必须保持存活的完整字节。 */
  struct QueuedBuffer final {
    std::uint64_t sequence{0U};
    std::vector<std::uint8_t> bytes;
  };

  [[nodiscard]] static std::optional<MovementKey> GetMovementKey(
      const OutboundMessage& message) noexcept;
  void QueueMovement(OutboundMessage message, std::size_t reserved_bytes = 0U);
  void DiscardPendingMovement() noexcept;
  void FlushPendingMovement();
  [[nodiscard]] std::vector<std::uint8_t> EncodeBatch(
      std::span<const OutboundMessage> messages);
  [[nodiscard]] std::optional<application::ServiceError> ValidateCompletion(
      std::uint64_t sequence) const;
  void DiscardAfterActive();

  protocol::ProtocolLimits limits_;
  protocol::FrameDecoder decoder_;
  protocol::LegacyCipher outbound_cipher_;
  std::deque<QueuedBuffer> send_queue_;
  /**
   * @brief 尚未开始 socket 写入的最新移动状态。
   * @note 只在本 Session strand
   * 上访问；保持原始消息而不是已加密字节，才能安全替换旧状态。
   */
  std::map<MovementKey, OutboundMessage> pending_movement_;
  std::size_t pending_movement_bytes_{0U};
  std::size_t queued_bytes_{0U};
  std::optional<std::uint64_t> active_sequence_;
  std::uint64_t next_sequence_{1U};
  std::uint32_t next_extended_transfer_id_{
      1U};  ///< 仅关联同连接内连续的 65000 分块。
  std::size_t movement_coalesced_count_{0U};
  std::size_t movement_dropped_count_{0U};
  SessionTransportState state_{SessionTransportState::kOpen};
};

/** @brief 返回传输状态的稳定英文日志名。 */
[[nodiscard]] const char* SessionTransportStateName(
    SessionTransportState state) noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_TRANSPORT_CORE_H_
