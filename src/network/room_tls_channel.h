#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_TLS_CHANNEL_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_TLS_CHANNEL_H_

/**
 * @file room_tls_channel.h
 * @brief 定义单事件循环中的双向证书认证和有界内部消息连接。
 */
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "protocol/internal/room_envelope.h"

namespace bf::infrastructure::network {

/** @brief 内部节点证书配置；对端证书必须属于此 CA 并匹配明确的 SAN 名称。 */
struct RoomTlsOptions final {
  std::string certificate;
  std::string private_key;
  std::string authority;
  std::string peer_name;
};

/** @brief 加载服务器证书与信任 CA，要求 TLS 1.2 以上及客户端证书。 */
void ConfigureRoomTls(boost::asio::ssl::context& context,
                      const RoomTlsOptions& options);

/** @brief 单线程异步 TLS 通道；解析前限制长度，出站按消息数与字节数限额。 */
class RoomTlsChannel final
    : public std::enable_shared_from_this<RoomTlsChannel> {
 public:
  using MessageHandler = std::function<void(protocol::internal::RoomEnvelope)>;
  /** @brief 移交 socket 所有权；handler 均在其 io_context 中调用。 */
  RoomTlsChannel(boost::asio::ip::tcp::socket socket,
                 boost::asio::ssl::context& context, std::string peer_name);
  /** @brief 完成有期限的双向认证后才调用 opened 和消息 handler。 */
  void Start(MessageHandler handler, std::function<void()> opened,
             std::function<void()> closed);
  /** @brief Performs the gateway side of the same mutually authenticated
   * channel. */
  void StartClient(MessageHandler handler, std::function<void()> opened,
                   std::function<void()> closed);
  /** @brief 非阻塞有界入队；失败关闭本通道，绝不扩大队列。 */
  [[nodiscard]] bool Send(protocol::internal::RoomEnvelope message);
  /** @brief 幂等关闭并取消全部计时器和 I/O。 */
  void Close();
  /** @brief 返回有界发送队列当前字节数。 */
  [[nodiscard]] std::size_t QueuedBytes() const noexcept;

 private:
  void Begin(boost::asio::ssl::stream_base::handshake_type role,
             MessageHandler handler, std::function<void()> opened,
             std::function<void()> closed);
  void ReadPrefix();
  void ReadBody(std::size_t bytes);
  void WriteNext();
  boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream_;
  boost::asio::steady_timer handshake_deadline_;
  std::string peer_name_;
  MessageHandler handler_;
  std::function<void()> closed_;
  std::array<std::uint8_t, 4U> prefix_{};
  std::vector<std::uint8_t> incoming_;
  std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> outgoing_;
  std::size_t queued_bytes_{0U};
  bool closed_flag_{false};
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_TLS_CHANNEL_H_
