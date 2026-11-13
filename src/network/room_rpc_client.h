#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_RPC_CLIENT_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_RPC_CLIENT_H_

/**
 * @file room_rpc_client.h
 * @brief 定义网关到节点和房间的有界异步 RPC 客户端。
 */
#include <map>

#include "application/error.h"
#include "network/room_tls_channel.h"

namespace bf::infrastructure::network {
/** @brief 全部方法及回调运行在指定的单个 I/O 事件循环。 */
class RoomRpcClient final : public std::enable_shared_from_this<RoomRpcClient> {
 public:
  using Reply = std::function<void(
      application::ServiceResult<protocol::internal::RoomEnvelope>)>;
  using Event = std::function<void(protocol::internal::RoomEnvelope)>;
  RoomRpcClient(boost::asio::io_context& io, const RoomTlsOptions& tls);
  /** @brief Resolves, connects and authenticates within five seconds. */
  void Connect(std::string host, std::uint16_t port,
               std::function<void(bool)> ready, Event event,
               std::function<void()> disconnected);
  /** @brief Takes an owning request; false leaves completion ownership with the
   * caller. */
  [[nodiscard]] bool Request(
      protocol::internal::RoomEnvelope message, Reply reply,
      std::chrono::milliseconds timeout = std::chrono::seconds{5});
  /** @brief Fails every accepted request exactly once and closes all
   * outstanding I/O. */
  void Close();

 private:
  /** @brief 已接管回调、字节预算及超时期限的请求。 */
  struct Pending final {
    Reply reply;
    std::chrono::steady_clock::time_point deadline;
    std::size_t bytes;
  };

  void Receive(protocol::internal::RoomEnvelope message);
  void Schedule();
  boost::asio::ssl::context tls_{boost::asio::ssl::context::tls_client};
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket socket_;
  boost::asio::steady_timer timer_;
  std::string peer_name_;
  std::shared_ptr<RoomTlsChannel> channel_;
  std::map<std::uint64_t, Pending> pending_;
  std::function<void(bool)> ready_;
  Event event_;
  std::function<void()> disconnected_;
  std::chrono::steady_clock::time_point connect_deadline_;
  std::size_t pending_bytes_{0U};
  std::uint64_t serial_{0U};
  bool connected_{false};
  bool started_{false};
  bool closed_{false};
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_RPC_CLIENT_H_
