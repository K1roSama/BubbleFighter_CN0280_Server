#include "network/room_rpc_client.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bf::infrastructure::network {
namespace asio = boost::asio;
using protocol::internal::RoomEnvelope;

RoomRpcClient::RoomRpcClient(asio::io_context& io, const RoomTlsOptions& tls)
    : resolver_(io), socket_(io), timer_(io), peer_name_(tls.peer_name) {
  ConfigureRoomTls(tls_, tls);
}

void RoomRpcClient::Connect(std::string host, std::uint16_t port,
                            std::function<void(bool)> ready, Event event,
                            std::function<void()> disconnected) {
  if (started_ || !port || host.empty())
    throw std::invalid_argument{"invalid internal RPC connection"};
  started_ = true;
  ready_ = std::move(ready);
  event_ = std::move(event);
  disconnected_ = std::move(disconnected);
  connect_deadline_ =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  Schedule();
  resolver_.async_resolve(
      std::move(host), std::to_string(port),
      [self = shared_from_this()](auto error, auto endpoints) {
        if (error || self->closed_) {
          self->Close();
          return;
        }
        asio::async_connect(
            self->socket_, endpoints, [self](auto connect_error, auto) {
              if (connect_error || self->closed_) {
                self->Close();
                return;
              }
              self->channel_ = std::make_shared<RoomTlsChannel>(
                  std::move(self->socket_), self->tls_, self->peer_name_);
              std::weak_ptr<RoomRpcClient> weak = self;
              self->channel_->StartClient(
                  [weak](RoomEnvelope message) {
                    if (auto client = weak.lock())
                      client->Receive(std::move(message));
                  },
                  [weak] {
                    if (auto client = weak.lock()) {
                      client->connected_ = true;
                      client->Schedule();
                      if (auto callback = std::move(client->ready_))
                        callback(true);
                    }
                  },
                  [weak] {
                    if (auto client = weak.lock()) client->Close();
                  });
            });
      });
}

bool RoomRpcClient::Request(RoomEnvelope message, Reply reply,
                            std::chrono::milliseconds timeout) {
  using protocol::internal::RoomMessageKind;
  if (message.payload.size() > protocol::internal::kMaximumRoomPayloadBytes ||
      (message.kind != RoomMessageKind::kAuthenticate &&
       message.kind != RoomMessageKind::kBind &&
       message.kind != RoomMessageKind::kCommand &&
       message.kind != RoomMessageKind::kHeartbeat &&
       message.kind != RoomMessageKind::kStop))
    return false;
  const auto bytes =
      protocol::internal::kRoomEnvelopeHeaderBytes + message.payload.size();
  if (!connected_ || closed_ || !reply ||
      timeout <= std::chrono::milliseconds::zero() ||
      timeout > std::chrono::seconds{30} || pending_.size() >= 128U ||
      bytes > 4U * 1024U * 1024U - pending_bytes_ ||
      serial_ == std::numeric_limits<std::uint64_t>::max())
    return false;
  message.request_id = ++serial_;
  pending_.emplace(message.request_id,
                   Pending{std::move(reply),
                           std::chrono::steady_clock::now() + timeout, bytes});
  pending_bytes_ += bytes;
  // Channel closure completes all requests, including this accepted request.
  if (!channel_->Send(std::move(message)))
    Close();
  else
    Schedule();
  return true;
}

void RoomRpcClient::Receive(RoomEnvelope message) {
  if (closed_) return;
  if (message.kind == protocol::internal::RoomMessageKind::kEvent) {
    if (auto callback = event_) callback(std::move(message));
    return;
  }
  if (message.kind != protocol::internal::RoomMessageKind::kReply &&
      message.kind != protocol::internal::RoomMessageKind::kError) {
    Close();
    return;
  }
  auto found = pending_.find(message.request_id);
  if (found == pending_.end()) {
    Close();
    return;
  }
  auto callback = std::move(found->second.reply);
  pending_bytes_ -= found->second.bytes;
  pending_.erase(found);
  Schedule();
  callback(std::move(message));
}

void RoomRpcClient::Schedule() {
  timer_.cancel();
  if (closed_) return;
  auto deadline = connected_ ? std::chrono::steady_clock::time_point::max()
                             : connect_deadline_;
  for (const auto& [id, pending] : pending_) {
    static_cast<void>(id);
    deadline = std::min(deadline, pending.deadline);
  }
  if (deadline == std::chrono::steady_clock::time_point::max()) return;
  timer_.expires_at(deadline);
  timer_.async_wait([self = shared_from_this()](auto error) {
    if (!error) self->Close();
  });
}

void RoomRpcClient::Close() {
  if (closed_) return;
  closed_ = true;
  connected_ = false;
  event_ = {};
  timer_.cancel();
  resolver_.cancel();
  boost::system::error_code ignored;
  socket_.close(ignored);
  if (channel_) channel_->Close();
  auto pending = std::move(pending_);
  pending_bytes_ = 0U;
  if (auto callback = std::move(ready_)) callback(false);
  for (auto& [id, item] : pending) {
    static_cast<void>(id);
    item.reply(application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "internal room connection closed or timed out",
        .is_retryable = true});
  }
  if (auto callback = std::move(disconnected_)) callback();
}
}  // namespace bf::infrastructure::network
