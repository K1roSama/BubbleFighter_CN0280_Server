#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_DS_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_DS_SERVICE_H_

/**
 * @file room_ds_service.h
 * @brief 定义单房间进程的异步监听、租约期限及持久化值消息桥接。
 */
#include "network/room_tls_channel.h"
#include "network/room_value_codec.h"
#include "sessions/latency_histogram.h"

namespace bf::infrastructure::network {
/** @brief 独立 DS 监听配置；未绑定进程有启动期限，绑定后需可信网关续租。 */
struct RoomDsOptions final {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0U};
  RoomTlsOptions tls;
  runtime::RoomMatchTiming timing;
  std::chrono::milliseconds bind_timeout{std::chrono::seconds{30}};
  std::chrono::milliseconds lease_duration{std::chrono::seconds{10}};
};

/** @brief 不创建线程和数据库连接的单房间应用事件循环。 */
class RoomDsService final {
 public:
  /** @brief 装配并打开监听；配置和证书错误直接失败。 */
  RoomDsService(boost::asio::io_context& io, RoomDsOptions options);
  /** @brief 开始异步 accept 和单调期限检查。 */
  void Start();
  /** @brief 停止本进程监听和连接，幂等。 */
  void Stop();
  /** @brief 返回实际监听端口，允许节点使用端口零预热。 */
  [[nodiscard]] std::uint16_t Port() const;

 private:
  void Receive(protocol::internal::RoomEnvelope message);
  [[nodiscard]] nlohmann::json Execute(
      const protocol::internal::RoomEnvelope& message,
      const nlohmann::json& payload);
  void Respond(protocol::internal::RoomEnvelope envelope, nlohmann::json value,
               protocol::internal::RoomMessageKind kind);
  void Schedule();
  [[nodiscard]] nlohmann::json StartRequest();
  [[nodiscard]] bool CanExit() const;
  boost::asio::io_context& io_;
  RoomDsOptions options_;
  boost::asio::ssl::context tls_{boost::asio::ssl::context::tls_server};
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::steady_timer timer_;
  std::shared_ptr<RoomTlsChannel> channel_;
  std::unique_ptr<runtime::DedicatedRoom> room_;
  std::optional<runtime::RoomCoordinatorResult> pending_start_;
  std::optional<domain::MatchId> installed_match_;
  std::chrono::steady_clock::time_point lease_deadline_;
  std::uint64_t control_sequence_{0U};
  bool stopped_{false};
  bool terminal_pending_{false};
  bool business_paused_{false};
  std::optional<application::UseItemCommand> pending_respawn_;
  runtime::LatencyHistogram timer_lateness_;
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_DS_SERVICE_H_
