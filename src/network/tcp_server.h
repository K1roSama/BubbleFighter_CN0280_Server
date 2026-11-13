#ifndef BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_

/**
 * @file tcp_server.h
 * @brief 定义 Boost.Asio TCP 监听、Session strand 与业务处理器之间的正式边界。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/event_logger.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/codec/limits.h"
#include "sessions/outbound_message.h"
#include "sessions/session_registry.h"

namespace bf::infrastructure::network {

/** @brief 一条入站消息完成后返回响应批次或脱敏错误的回调。 */
using PacketCompletion = std::function<void(
    application::ServiceResult<std::vector<runtime::OutboundMessage>>)>;

/** @brief Schedule one bounded delayed continuation on the owning Session
 * strand. */
using SessionWorkScheduler =
    std::function<std::optional<application::ServiceError>(
        std::chrono::milliseconds, std::function<void()>)>;

/** @brief 创建单会话处理器时移交的连接身份与在线玩家注册端口。 */
struct SessionHandlerContext final {
  runtime::SessionHandle handle;  ///< 进程内稳定 Session 句柄。
  std::unique_ptr<runtime::OnlinePlayerRegistry>
      online_players;        ///< 仅能为本 Session 认领玩家。
  std::string peer_address;  ///< 仅含对端
                             ///< IP、不含端口；用于认证限流，禁止当作可信身份。
  SessionWorkScheduler schedule_after{};  ///< One pending continuation;
                                          ///< cancelled on Session close.
};

/**
 * @brief 单个 Session 的异步协议/业务驱动器。
 * @note `async_handle` 在 Session strand 上发起；实现可将阻塞数据库工作投递到
 *       有界工作池，但必须且只能调用 completion 一次。TCP 层会把完成结果
 *       重新 post 回原 Session strand，因此不要保存裸 Session 指针。
 */
class SessionPacketHandler {
 public:
  virtual ~SessionPacketHandler() = default;

  /** @brief 顺序处理一条已校验明文包；完成前该 Session 不再读取新字节。 */
  virtual void AsyncHandle(protocol::Packet packet,
                           PacketCompletion completion) = 0;

  /** @brief 协议处理器可将已认证旧连接的正常收尾记录为 info；普通拒绝默认
   * warning。 */
  [[nodiscard]] virtual application::LogLevel RejectionLogLevel(
      const application::ServiceError&) const noexcept {
    return application::LogLevel::kWarning;
  }

  /** @brief 在 Session strand 上原子应用 registry 投递附带的阶段状态。 */
  [[nodiscard]] virtual std::optional<application::ServiceError> ApplyDispatch(
      const runtime::SessionDispatch& dispatch) = 0;

  /** @brief 幂等释放在线租约、计时器和业务侧资源；不得抛异常。 */
  virtual void BeginClose() noexcept = 0;
};

/** @brief 为每条已登记连接创建互不共享的 Session 处理器。 */
class SessionPacketHandlerFactory {
 public:
  virtual ~SessionPacketHandlerFactory() = default;

  /** @brief 接收 Session 上下文并返回非空处理器；失败时连接在发送业务包前关闭。
   */
  [[nodiscard]] virtual application::ServiceResult<
      std::unique_ptr<SessionPacketHandler>>
  Create(SessionHandlerContext context) = 0;
};

/** @brief TCP 监听、I/O 线程和每连接资源上限。 */
struct TcpServerOptions final {
  std::string bind_address;             ///< 数字 IPv4/IPv6 监听地址。
  std::uint16_t port{0U};               ///< 监听端口；0 只供隔离测试自动分配。
  std::size_t io_threads{0U};           ///< `io_context` 工作线程数。
  std::size_t maximum_connections{0U};  ///< 活动 TCP Session 硬上限。
  protocol::ProtocolLimits protocol_limits;  ///< 拆包与发送队列字节上限。
  protocol::FrameNamespace frame_namespace{
      protocol::FrameNamespace::kGateway};  ///< 独立消息空间。
};

/**
 * @brief 拥有 acceptor、`io_context`、I/O 线程和全部活动 Session 的 TCP
 * 服务器。
 * @note handler factory、registry 及 logger
 * 均由组装层拥有，必须比本对象存活更久。 `stop` 必须由非 I/O
 * 控制线程调用，它先停止 accept，再关闭 Session 并合并线程。
 */
class TcpServer final {
 public:
  /** @brief 创建未启动服务器；参数在 `start` 时统一校验。 */
  TcpServer(TcpServerOptions options, runtime::SessionRegistry& registry,
            SessionPacketHandlerFactory& handler_factory,
            application::EventLogger& logger);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;
  TcpServer(TcpServer&&) = delete;
  TcpServer& operator=(TcpServer&&) = delete;

  /** @brief 绑定端点并启动 accept/I/O 线程；对象只允许启动一次。 */
  [[nodiscard]] std::optional<application::ServiceError> Start();

  /** @brief 幂等停止 accept，请求全部 Session 关闭，最多等待 grace 后终止 I/O。
   */
  void Stop(std::chrono::milliseconds grace) noexcept;

  /** @brief 返回绑定后的实际端口；启动前为 0。 */
  [[nodiscard]] std::uint16_t LocalPort() const noexcept;

  /** @brief 返回尚未完成关闭的 Session 数量快照。 */
  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;

  /** @brief 返回所有 Session mailbox/传输队列累计替换旧移动状态的次数。 */
  [[nodiscard]] std::uint64_t MovementCoalescedTotal() const noexcept;

  /** @brief 返回所有 Session mailbox/传输队列累计主动丢弃移动状态的次数。 */
  [[nodiscard]] std::uint64_t MovementDroppedTotal() const noexcept;

 private:
  /** @brief 隔离 Boost.Asio 类型和活动 Session 容器的实现对象。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_TCP_SERVER_H_
