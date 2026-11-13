#include "network/room_tls_channel.h"

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <stdexcept>

namespace bf::infrastructure::network {
namespace asio = boost::asio;

void ConfigureRoomTls(asio::ssl::context& context,
                      const RoomTlsOptions& options) {
  if (options.peer_name.empty())
    throw std::invalid_argument{"internal peer SAN is required"};
  context.set_options(
      asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
      asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
  if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) !=
      1)
    throw std::runtime_error{"cannot set internal TLS minimum version"};
  context.use_certificate_chain_file(options.certificate);
  context.use_private_key_file(options.private_key, asio::ssl::context::pem);
  if (SSL_CTX_check_private_key(context.native_handle()) != 1)
    throw std::runtime_error{"internal TLS certificate/key mismatch"};
  context.load_verify_file(options.authority);
  context.set_verify_mode(asio::ssl::verify_peer |
                          asio::ssl::verify_fail_if_no_peer_cert);
}

RoomTlsChannel::RoomTlsChannel(asio::ip::tcp::socket socket,
                               asio::ssl::context& context,
                               std::string peer_name)
    : stream_(std::move(socket), context),
      handshake_deadline_(stream_.get_executor()),
      peer_name_(std::move(peer_name)) {}

void RoomTlsChannel::Start(MessageHandler handler, std::function<void()> opened,
                           std::function<void()> closed) {
  Begin(asio::ssl::stream_base::server, std::move(handler), std::move(opened),
        std::move(closed));
}

void RoomTlsChannel::StartClient(MessageHandler handler,
                                 std::function<void()> opened,
                                 std::function<void()> closed) {
  Begin(asio::ssl::stream_base::client, std::move(handler), std::move(opened),
        std::move(closed));
}

void RoomTlsChannel::Begin(asio::ssl::stream_base::handshake_type role,
                           MessageHandler handler, std::function<void()> opened,
                           std::function<void()> closed) {
  handler_ = std::move(handler);
  closed_ = std::move(closed);
  stream_.set_verify_callback(
      [name = peer_name_](bool verified, asio::ssl::verify_context& context) {
        if (!verified) return false;
        auto* store = context.native_handle();
        if (X509_STORE_CTX_get_error_depth(store) != 0) return true;
        auto* cert = X509_STORE_CTX_get_current_cert(store);
        return X509_check_host(cert, name.c_str(), name.size(),
                               X509_CHECK_FLAG_NEVER_CHECK_SUBJECT,
                               nullptr) == 1;
      });
  handshake_deadline_.expires_after(std::chrono::seconds{5});
  handshake_deadline_.async_wait([self = shared_from_this()](auto error) {
    if (!error) self->Close();
  });
  stream_.async_handshake(role, [self = shared_from_this(),
                                 opened = std::move(opened)](auto error) {
    self->handshake_deadline_.cancel();
    if (error || self->closed_flag_) {
      self->Close();
      return;
    }
    self->stream_.lowest_layer().set_option(asio::ip::tcp::no_delay{true},
                                            error);
    if (error) {
      self->Close();
      return;
    }
    if (opened) opened();
    self->ReadPrefix();
  });
}

void RoomTlsChannel::ReadPrefix() {
  if (closed_flag_) return;
  asio::async_read(
      stream_, asio::buffer(prefix_),
      [self = shared_from_this()](auto error, auto) {
        if (error) {
          self->Close();
          return;
        }
        std::uint32_t size = 0U;
        for (unsigned index = 0U; index < 4U; ++index)
          size |= static_cast<std::uint32_t>(self->prefix_[index])
                  << (index * 8U);
        if (size < protocol::internal::kRoomEnvelopeHeaderBytes - 4U ||
            size > protocol::internal::kRoomEnvelopeHeaderBytes - 4U +
                       protocol::internal::kMaximumRoomPayloadBytes) {
          self->Close();
          return;
        }
        self->ReadBody(size);
      });
}

void RoomTlsChannel::ReadBody(std::size_t bytes) {
  incoming_.resize(bytes + 4U);
  std::ranges::copy(prefix_, incoming_.begin());
  asio::async_read(stream_, asio::buffer(incoming_.data() + 4U, bytes),
                   [self = shared_from_this()](auto error, auto) {
                     if (error) {
                       self->Close();
                       return;
                     }
                     try {
                       self->handler_(protocol::internal::DecodeRoomEnvelope(
                           self->incoming_));
                     } catch (...) {
                       self->Close();
                       return;
                     }
                     self->ReadPrefix();
                   });
}

bool RoomTlsChannel::Send(protocol::internal::RoomEnvelope message) {
  if (closed_flag_) return false;
  auto data = std::make_shared<const std::vector<std::uint8_t>>(
      protocol::internal::EncodeRoomEnvelope(message));
  if (outgoing_.size() >= 256U ||
      data->size() > 4U * 1024U * 1024U - queued_bytes_) {
    Close();
    return false;
  }
  const bool idle = outgoing_.empty();
  queued_bytes_ += data->size();
  outgoing_.push_back(std::move(data));
  if (idle) WriteNext();
  return true;
}

void RoomTlsChannel::WriteNext() {
  if (closed_flag_ || outgoing_.empty()) return;
  auto data = outgoing_.front();
  asio::async_write(stream_, asio::buffer(*data),
                    [self = shared_from_this(), data](auto error, auto) {
                      if (error || self->closed_flag_) {
                        self->Close();
                        return;
                      }
                      self->queued_bytes_ -= data->size();
                      self->outgoing_.pop_front();
                      self->WriteNext();
                    });
}

void RoomTlsChannel::Close() {
  if (closed_flag_) return;
  closed_flag_ = true;
  handshake_deadline_.cancel();
  boost::system::error_code ignored;
  stream_.lowest_layer().cancel(ignored);
  stream_.lowest_layer().close(ignored);
  outgoing_.clear();
  queued_bytes_ = 0U;
  if (auto callback = std::move(closed_)) callback();
}

std::size_t RoomTlsChannel::QueuedBytes() const noexcept {
  return queued_bytes_;
}
}  // namespace bf::infrastructure::network
