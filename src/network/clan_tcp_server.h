#ifndef BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_

/**
 * @file clan_tcp_server.h
 * @brief 定义独立 Nx 战队端口、主会话票据绑定和有界异步执行边界。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "application/background_task_executor.h"
#include "application/event_logger.h"
#include "application/system_clock.h"
#include "features/clans/clan_service.h"
#include "features/lobby/player_menu_snapshot.h"
#include "sessions/clan_ticket_broker.h"
#include "sessions/session_registry.h"

namespace bf::infrastructure::network {

/** @brief Nx 战队监听、连接数与单连接发送队列边界。 */
struct ClanTcpServerOptions final {
  std::string bind_address;                  ///< 数字 IPv4/IPv6 监听地址。
  std::uint16_t port{0U};                    ///< 配置端口；0 仅用于隔离测试。
  std::size_t io_threads{0U};                ///< Asio I/O 线程数。
  std::size_t maximum_connections{0U};       ///< 未认证与已认证连接合计上限。
  std::size_t maximum_send_queue_bytes{0U};  ///< 每连接已排队密文帧上限。
};

/**
 * @brief 拥有战队 acceptor、Session strand、在线子连接索引与 I/O 线程。
 * @note 所有阻塞 MySQL 用例均投递到共享有界
 * DatabaseTaskPool；同一子连接在上一条
 *       请求完成前不再读下一帧，以保持可观察顺序。
 */
class ClanTcpServer final {
 public:
  /** @brief 创建未启动的 Nx 服务；所有引用依赖必须覆盖 stop 和析构。 */
  ClanTcpServer(ClanTcpServerOptions options,
                application::BackgroundTaskExecutor& database_tasks,
                application::ClanExecutor& clans,
                application::PlayerMenuSnapshotLoader& menu_snapshots,
                runtime::ClanTicketBroker& tickets,
                runtime::SessionRegistry& gateway_sessions,
                const application::SystemClock& clock,
                application::EventLogger& logger);
  ~ClanTcpServer();

  ClanTcpServer(const ClanTcpServer&) = delete;
  ClanTcpServer& operator=(const ClanTcpServer&) = delete;
  ClanTcpServer(ClanTcpServer&&) = delete;
  ClanTcpServer& operator=(ClanTcpServer&&) = delete;

  /** @brief 绑定端点、开始 accept 并发送 S2C1002 challenge。 */
  [[nodiscard]] std::optional<application::ServiceError> Start();
  /** @brief 幂等停止 accept，请求关闭连接，并在 grace 内排空 I/O。 */
  void Stop(std::chrono::milliseconds grace) noexcept;
  /** @brief 返回实际绑定端口；未启动时为 0。 */
  [[nodiscard]] std::uint16_t LocalPort() const noexcept;
  /** @brief 返回尚未完成关闭的 Nx 连接数。 */
  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;

 private:
  /** @brief 隐藏 Asio acceptor、连接索引与工作线程的具体所有权。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_CLAN_TCP_SERVER_H_
