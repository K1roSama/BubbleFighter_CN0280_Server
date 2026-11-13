#include "network/tcp_server.h"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "sessions/session_transport_core.h"

namespace bf::infrastructure::network {
namespace {

using boost::asio::ip::tcp;

constexpr std::size_t kReadBufferBytes = std::size_t{16U} * 1024U;
constexpr std::size_t kFrameHeaderBytes = 5U;

struct PendingPacket final {
  protocol::Packet packet;
  std::chrono::steady_clock::time_point decoded_at;
};

struct PendingMovementKey final {
  std::uint16_t opcode{0U};
  std::uint32_t actor_object_id{0U};

  auto operator<=>(const PendingMovementKey&) const = default;
};

/** @brief 显式消费仅通过 error_code 报错的 Asio 操作返回值。 */
template <typename Result>
void ObserveIoResult(Result&& /*result*/) noexcept {}

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = retryable};
}

[[nodiscard]] std::optional<PendingMovementKey> GetPendingMovementKey(
    const runtime::OutboundMessage& message) noexcept {
  if ((message.opcode != 32'769U && message.opcode != 32'885U) ||
      message.body.size() < 4U) {
    return std::nullopt;
  }
  const auto actor_object_id =
      static_cast<std::uint32_t>(message.body[0U]) |
      (static_cast<std::uint32_t>(message.body[1U]) << 8U) |
      (static_cast<std::uint32_t>(message.body[2U]) << 16U) |
      (static_cast<std::uint32_t>(message.body[3U]) << 24U);
  return PendingMovementKey{.opcode = message.opcode,
                            .actor_object_id = actor_object_id};
}

[[nodiscard]] std::optional<std::size_t> OutboundWireBytes(
    const runtime::OutboundMessage& message) noexcept {
  if (message.body.size() >
      std::numeric_limits<std::size_t>::max() - kFrameHeaderBytes) {
    return std::nullopt;
  }
  return kFrameHeaderBytes + message.body.size();
}

[[nodiscard]] std::optional<std::size_t> DispatchWireBytes(
    const runtime::SessionDispatch& dispatch) {
  std::size_t total = 0U;
  for (const auto& message : dispatch.messages) {
    if (GetPendingMovementKey(message).has_value()) {
      continue;
    }
    const auto message_bytes = OutboundWireBytes(message);
    if (!message_bytes.has_value() ||
        total > std::numeric_limits<std::size_t>::max() - *message_bytes) {
      return std::nullopt;
    }
    total += *message_bytes;
  }
  return total;
}

class TcpSession final : public runtime::SessionMailbox,
                         public std::enable_shared_from_this<TcpSession> {
 public:
  using ClosedCallback = std::function<void(std::uint64_t)>;
  using MovementPressureCallback =
      std::function<void(std::uint64_t, std::uint64_t)>;

  TcpSession(std::uint64_t connection_id, tcp::socket socket,
             protocol::ProtocolLimits limits,
             runtime::SessionRegistry& registry,
             SessionPacketHandlerFactory& handler_factory,
             application::EventLogger& logger, ClosedCallback closed_callback,
             protocol::FrameNamespace frame_namespace,
             MovementPressureCallback movement_pressure_callback)
      : connection_id_(connection_id),
        socket_(std::move(socket)),
        handler_timer_(socket_.get_executor()),
        disconnect_timer_(socket_.get_executor()),
        transport_(limits, frame_namespace),
        kMaximumPendingDispatchBytes(limits.maximum_send_queue_bytes),
        registry_(registry),
        handler_factory_(handler_factory),
        logger_(logger),
        closed_callback_(std::move(closed_callback)),
        movement_pressure_callback_(std::move(movement_pressure_callback)) {}

  void Start() {
    boost::asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
      self->InvokeGuarded("TCP session start callback failed",
                          [&self] { self->StartOnStrand(); });
    });
  }

  void RequestStop() noexcept {
    accepting_dispatches_.store(false, std::memory_order_release);
    try {
      boost::asio::post(socket_.get_executor(),
                        [self = shared_from_this()] { self->Finalize(); });
    } catch (...) {
      logger_.Log(application::LogLevel::kWarning,
                  "TCP session stop request could not be posted",
                  std::to_string(connection_id_));
    }
  }

  std::optional<application::ServiceError> Post(
      runtime::SessionDispatch dispatch) override {
    if (!accepting_dispatches_.load(std::memory_order_acquire)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Session 正在关闭");
    }
    runtime::SessionDispatch reliable_dispatch;
    reliable_dispatch.room_stage = dispatch.room_stage;
    reliable_dispatch.plaza_stage = dispatch.plaza_stage;
    reliable_dispatch.world_phase = dispatch.world_phase;
    reliable_dispatch.channel = dispatch.channel;
    reliable_dispatch.disconnect = dispatch.disconnect;
    reliable_dispatch.flush_before_disconnect =
        dispatch.flush_before_disconnect;
    std::map<PendingMovementKey, runtime::OutboundMessage> movement_updates;
    std::uint64_t mailbox_coalesced = 0U;
    std::uint64_t mailbox_dropped = 0U;
    for (auto& message : dispatch.messages) {
      if (const auto key = GetPendingMovementKey(message); key.has_value()) {
        if (movement_updates.contains(*key)) {
          ++mailbox_coalesced;
        }
        movement_updates.insert_or_assign(*key, std::move(message));
      } else {
        reliable_dispatch.messages.push_back(std::move(message));
      }
    }
    const auto wire_bytes = DispatchWireBytes(reliable_dispatch);
    if (!wire_bytes.has_value() || *wire_bytes > kMaximumPendingDispatchBytes) {
      return MakeError(application::ErrorCode::kOverloaded,
                       "Session mailbox 批次超过字节上限");
    }

    const auto has_reliable_dispatch =
        !reliable_dispatch.messages.empty() ||
        reliable_dispatch.room_stage.has_value() ||
        reliable_dispatch.plaza_stage.has_value() ||
        reliable_dispatch.world_phase.has_value() ||
        reliable_dispatch.channel.has_value() || reliable_dispatch.disconnect;
    bool schedule_drain = false;
    bool mailbox_overloaded = false;
    {
      std::scoped_lock lock{pending_dispatch_mutex_};
      if (!accepting_dispatches_.load(std::memory_order_acquire)) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "Session 正在关闭");
      }
      if (pending_dispatch_bytes_ >
          kMaximumPendingDispatchBytes - *wire_bytes) {
        // A reliable event has priority over a still-unposted movement
        // snapshot. Release that disposable tail before deciding whether the
        // mailbox is truly full.
        mailbox_dropped +=
            static_cast<std::uint64_t>(pending_movement_dispatches_.size());
        for (const auto& [key, message] : pending_movement_dispatches_) {
          static_cast<void>(key);
          const auto bytes = OutboundWireBytes(message).value_or(0U);
          pending_dispatch_bytes_ = pending_dispatch_bytes_ >= bytes
                                        ? pending_dispatch_bytes_ - bytes
                                        : 0U;
        }
        pending_movement_dispatches_.clear();
        mailbox_overloaded = pending_dispatch_bytes_ >
                             kMaximumPendingDispatchBytes - *wire_bytes;
      }
      if (!mailbox_overloaded) {
        pending_dispatch_bytes_ += *wire_bytes;
        if (has_reliable_dispatch &&
            !(dispatch.disconnect && !dispatch.flush_before_disconnect)) {
          FlushPendingMovementLocked();
        }
      }
      for (auto& [key, message] : movement_updates) {
        if (mailbox_overloaded) {
          break;
        }
        static_cast<void>(key);
        const auto new_bytes = OutboundWireBytes(message);
        if (!new_bytes.has_value() ||
            *new_bytes > kMaximumPendingDispatchBytes) {
          ++mailbox_dropped;
          continue;
        }
        const auto existing = pending_movement_dispatches_.find(key);
        const auto old_bytes =
            existing == pending_movement_dispatches_.end()
                ? 0U
                : OutboundWireBytes(existing->second).value_or(0U);
        const auto bytes_without_old = pending_dispatch_bytes_ >= old_bytes
                                           ? pending_dispatch_bytes_ - old_bytes
                                           : 0U;
        if (bytes_without_old > kMaximumPendingDispatchBytes - *new_bytes) {
          ++mailbox_dropped;
          continue;
        }
        if (existing != pending_movement_dispatches_.end()) {
          ++mailbox_coalesced;
        }
        pending_dispatch_bytes_ = bytes_without_old + *new_bytes;
        pending_movement_dispatches_.insert_or_assign(key, std::move(message));
      }
      if (!mailbox_overloaded && has_reliable_dispatch) {
        pending_dispatches_.push_back(std::move(reliable_dispatch));
      }
      if (!mailbox_overloaded && !dispatch_posted_) {
        dispatch_posted_ = true;
        schedule_drain = true;
      }
    }
    if (movement_pressure_callback_ &&
        (mailbox_coalesced != 0U || mailbox_dropped != 0U)) {
      movement_pressure_callback_(mailbox_coalesced, mailbox_dropped);
    }
    if (mailbox_overloaded) {
      return MakeError(application::ErrorCode::kOverloaded,
                       "Session mailbox 已达字节上限");
    }
    if (!schedule_drain) {
      return std::nullopt;
    }
    try {
      boost::asio::post(socket_.get_executor(), [self = shared_from_this()] {
        self->InvokeGuarded("TCP session mailbox callback failed",
                            [&] { self->DrainDispatches(); });
      });
    } catch (...) {
      std::scoped_lock lock{pending_dispatch_mutex_};
      pending_dispatches_.clear();
      pending_movement_dispatches_.clear();
      pending_dispatch_bytes_ = 0U;
      dispatch_posted_ = false;
      accepting_dispatches_.store(false, std::memory_order_release);
      return MakeError(application::ErrorCode::kOverloaded,
                       "Session mailbox 无法接受新任务", true);
    }
    return std::nullopt;
  }

 private:
  void FlushPendingMovementLocked() {
    if (pending_movement_dispatches_.empty()) {
      return;
    }
    runtime::SessionDispatch movement_dispatch;
    movement_dispatch.messages.reserve(pending_movement_dispatches_.size());
    for (auto& [key, message] : pending_movement_dispatches_) {
      static_cast<void>(key);
      movement_dispatch.messages.push_back(std::move(message));
    }
    pending_movement_dispatches_.clear();
    pending_dispatches_.push_back(std::move(movement_dispatch));
  }

  void DrainDispatches() {
    std::deque<runtime::SessionDispatch> dispatches;
    std::map<PendingMovementKey, runtime::OutboundMessage> movements;
    {
      std::scoped_lock lock{pending_dispatch_mutex_};
      dispatches.swap(pending_dispatches_);
      movements.swap(pending_movement_dispatches_);
      pending_dispatch_bytes_ = 0U;
      dispatch_posted_ = false;
    }

    for (auto& dispatch : dispatches) {
      if (finalized_) {
        return;
      }
      ApplyDispatch(std::move(dispatch));
    }
    if (finalized_ || disconnect_draining_ || movements.empty()) {
      return;
    }
    runtime::SessionDispatch movement_dispatch;
    movement_dispatch.messages.reserve(movements.size());
    for (auto& [key, message] : movements) {
      static_cast<void>(key);
      movement_dispatch.messages.push_back(std::move(message));
    }
    ApplyDispatch(std::move(movement_dispatch));
  }

  std::optional<application::ServiceError> ScheduleHandlerWork(
      std::chrono::milliseconds delay, std::function<void()> work) {
    if (!work || delay <= std::chrono::milliseconds{0} ||
        delay > std::chrono::seconds{1}) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "Invalid Session continuation");
    }
    if (!accepting_dispatches_.load(std::memory_order_acquire)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Session is closing");
    }
    bool expected = false;
    if (!handler_work_pending_.compare_exchange_strong(expected, true)) {
      return MakeError(application::ErrorCode::kOverloaded,
                       "Session continuation is already pending");
    }
    try {
      boost::asio::post(
          socket_.get_executor(),
          [self = shared_from_this(), delay, work = std::move(work)]() mutable {
            self->InvokeGuarded("Session continuation callback failed", [&] {
              if (self->finalized_) {
                self->handler_work_pending_.store(false);
                return;
              }
              self->handler_timer_.expires_after(delay);
              self->handler_timer_.async_wait(
                  [self, work = std::move(work)](
                      const boost::system::error_code& error) mutable {
                    self->handler_work_pending_.store(false);
                    if (!error && !self->finalized_ &&
                        !self->disconnect_draining_) {
                      self->InvokeGuarded("Session delayed work failed",
                                          [&] { work(); });
                    }
                  });
            });
          });
    } catch (...) {
      handler_work_pending_.store(false);
      return MakeError(application::ErrorCode::kOverloaded,
                       "Session continuation cannot be posted");
    }
    return std::nullopt;
  }

  void StartOnStrand() {
    // Native movement frames are small and must not wait for Nagle coalescing.
    socket_.set_option(tcp::no_delay{true});
    auto registration_result = registry_.RegisterSession(shared_from_this());
    if (const auto* error =
            std::get_if<application::ServiceError>(&registration_result)) {
      logger_.Log(application::LogLevel::kWarning, error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    registration_ =
        std::get<runtime::SessionRegistration>(std::move(registration_result));
    boost::system::error_code endpoint_error;
    const auto endpoint = socket_.remote_endpoint(endpoint_error);
    std::weak_ptr<TcpSession> weak_self = shared_from_this();
    SessionHandlerContext context{
        .handle = registration_.Handle(),
        .online_players = registration_.MakeOnlinePlayerRegistry(),
        .peer_address = endpoint_error ? std::string{"unknown"}
                                       : endpoint.address().to_string(),
        .schedule_after =
            [weak_self](std::chrono::milliseconds delay,
                        std::function<void()> work) {
              const auto self = weak_self.lock();
              return self ? self->ScheduleHandlerWork(delay, std::move(work))
                          : std::optional{
                                MakeError(application::ErrorCode::kInvalidState,
                                          "Session has closed")};
            },
    };
    auto handler_result = handler_factory_.Create(std::move(context));
    if (const auto* error =
            std::get_if<application::ServiceError>(&handler_result)) {
      logger_.Log(application::LogLevel::kWarning, error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    handler_ = std::get<std::unique_ptr<SessionPacketHandler>>(
        std::move(handler_result));
    if (!handler_) {
      logger_.Log(application::LogLevel::kError,
                  "Session handler factory 返回空对象",
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    logger_.Log(application::LogLevel::kDebug, "TCP session accepted",
                std::to_string(connection_id_));
    StartWrite();
  }

  void StartRead() {
    if (finalized_ || disconnect_draining_ || read_in_progress_ ||
        handling_packet_) {
      return;
    }
    read_in_progress_ = true;
    socket_.async_read_some(
        boost::asio::buffer(read_buffer_),
        [self = shared_from_this()](const boost::system::error_code& error,
                                    std::size_t transferred) {
          self->InvokeGuarded("TCP session read callback failed", [&] {
            self->read_in_progress_ = false;
            self->HandleRead(error, transferred);
          });
        });
  }

  void HandleRead(const boost::system::error_code& error,
                  std::size_t transferred) {
    if (finalized_ || disconnect_draining_) return;
    if (error || transferred == 0U) {
      Finalize();
      return;
    }
    try {
      auto result = transport_.FeedReceived(
          std::span<const std::uint8_t>{read_buffer_.data(), transferred});
      if (const auto* service_error =
              std::get_if<application::ServiceError>(&result)) {
        logger_.Log(application::LogLevel::kWarning,
                    service_error->safe_message,
                    std::to_string(connection_id_));
        Finalize();
        return;
      }
      auto packets = std::get<std::vector<protocol::Packet>>(std::move(result));
      const auto decoded_at = std::chrono::steady_clock::now();
      for (auto& packet : packets) {
        pending_packets_.push_back(
            {.packet = std::move(packet), .decoded_at = decoded_at});
      }
      ProcessNextPacket();
    } catch (const protocol::ProtocolError&) {
      logger_.Log(application::LogLevel::kWarning,
                  "TCP session rejected malformed frame",
                  std::to_string(connection_id_));
      Finalize();
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "TCP session receive path failed",
                  std::to_string(connection_id_));
      Finalize();
    }
  }

  void ProcessNextPacket() {
    if (finalized_ || disconnect_draining_ || handling_packet_) {
      return;
    }
    if (pending_packets_.empty()) {
      StartRead();
      return;
    }
    handling_packet_ = true;
    auto pending = std::move(pending_packets_.front());
    pending_packets_.pop_front();
    active_opcode_ = pending.packet.opcode;
    active_decoded_at_ = pending.decoded_at;
    active_started_at_ = std::chrono::steady_clock::now();
    try {
      std::weak_ptr<TcpSession> weak_self = shared_from_this();
      handler_->AsyncHandle(
          std::move(pending.packet),
          [weak_self](
              application::ServiceResult<std::vector<runtime::OutboundMessage>>
                  result) mutable {
            if (const auto self = weak_self.lock()) {
              try {
                boost::asio::post(
                    self->socket_.get_executor(),
                    [self, result = std::move(result)]() mutable {
                      self->InvokeGuarded(
                          "TCP session packet completion callback failed", [&] {
                            self->HandlePacketCompletion(std::move(result));
                          });
                    });
              } catch (...) {
                self->RequestStop();
              }
            }
          });
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "Session handler 发起异步处理失败",
                  std::to_string(connection_id_));
      Finalize();
    }
  }

  void HandlePacketCompletion(
      application::ServiceResult<std::vector<runtime::OutboundMessage>>
          result) {
    if (finalized_ || disconnect_draining_) {
      return;
    }
    LogMovementLatency();
    handling_packet_ = false;
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      logger_.Log(handler_->RejectionLogLevel(*error), error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    auto responses =
        std::get<std::vector<runtime::OutboundMessage>>(std::move(result));
    const auto enqueue_result = transport_.Enqueue(responses);
    if (const auto* error =
            std::get_if<application::ServiceError>(&enqueue_result)) {
      logger_.Log(application::LogLevel::kWarning, error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    LogMovementTransportPressure();
    StartWrite();
    ProcessNextPacket();
  }

  /** @brief Observe slow movement handling without changing ordering or logging
   * payloads. */
  void LogMovementLatency() {
    if (active_opcode_ != 32769U && active_opcode_ != 32885U) {
      return;
    }
    const auto completed_at = std::chrono::steady_clock::now();
    if (completed_at - active_decoded_at_ < std::chrono::milliseconds{25} ||
        completed_at - last_movement_latency_log_ < std::chrono::seconds{5}) {
      return;
    }
    last_movement_latency_log_ = completed_at;
    const auto queue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              active_started_at_ - active_decoded_at_)
                              .count();
    const auto handling_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            completed_at - active_started_at_)
            .count();
    logger_.Log(
        application::LogLevel::kWarning,
        "native movement latency: opcode=" + std::to_string(active_opcode_) +
            " session_queue_ms=" + std::to_string(queue_ms) +
            " handling_ms=" + std::to_string(handling_ms) +
            " pending_packets=" + std::to_string(pending_packets_.size()) +
            " transport_queued_bytes=" +
            std::to_string(transport_.QueuedBytes()) +
            " transport_queued_buffers=" +
            std::to_string(transport_.QueuedBuffers()) + " pending_movement=" +
            std::to_string(transport_.PendingMovementCount()) +
            " movement_coalesced=" +
            std::to_string(transport_.MovementCoalescedCount()) +
            " movement_dropped=" +
            std::to_string(transport_.MovementDroppedCount()),
        std::to_string(connection_id_));
  }

  /** @brief Observe stale movement replacement and egress queue pressure per
   * Session. */
  void LogMovementTransportPressure() {
    const auto now = std::chrono::steady_clock::now();
    const auto coalesced = transport_.MovementCoalescedCount();
    const auto dropped = transport_.MovementDroppedCount();
    const auto coalesced_delta = coalesced - last_reported_movement_coalesced_;
    const auto dropped_delta = dropped - last_reported_movement_dropped_;
    if ((coalesced_delta != 0U || dropped_delta != 0U) &&
        movement_pressure_callback_) {
      movement_pressure_callback_(static_cast<std::uint64_t>(coalesced_delta),
                                  static_cast<std::uint64_t>(dropped_delta));
    }
    last_reported_movement_coalesced_ = coalesced;
    last_reported_movement_dropped_ = dropped;
    if (now - last_movement_transport_log_ < std::chrono::seconds{5} ||
        (coalesced == last_logged_movement_coalesced_ &&
         dropped == last_logged_movement_dropped_ &&
         transport_.PendingMovementCount() == 0U)) {
      return;
    }
    last_movement_transport_log_ = now;
    last_logged_movement_coalesced_ = coalesced;
    last_logged_movement_dropped_ = dropped;
    logger_.Log(
        dropped != 0U ? application::LogLevel::kWarning
                      : application::LogLevel::kInfo,
        "native movement egress pressure: queued_bytes=" +
            std::to_string(transport_.QueuedBytes()) + " queued_buffers=" +
            std::to_string(transport_.QueuedBuffers()) + " pending_movement=" +
            std::to_string(transport_.PendingMovementCount()) +
            " coalesced_total=" + std::to_string(coalesced) +
            " dropped_total=" + std::to_string(dropped),
        std::to_string(connection_id_));
  }

  void ApplyDispatch(runtime::SessionDispatch dispatch) {
    if (finalized_ || disconnect_draining_ || !handler_) {
      return;
    }
    if (dispatch.disconnect) {
      if (!dispatch.flush_before_disconnect || dispatch.messages.empty()) {
        logger_.Log(application::LogLevel::kInfo,
                    "Account moderation closed the session.",
                    std::to_string(connection_id_));
        Finalize();
        return;
      }
      // Admit only the final notice. Incoming work and late completions cannot
      // add anything once closure has been requested.
      accepting_dispatches_.store(false, std::memory_order_release);
      disconnect_draining_ = true;
      pending_packets_.clear();
      handler_->BeginClose();
      const auto enqueued = transport_.Enqueue(dispatch.messages);
      if (std::holds_alternative<application::ServiceError>(enqueued)) {
        Finalize();
        return;
      }
      transport_.BeginClose(runtime::SessionCloseMode::kDrain);
      disconnect_timer_.expires_after(std::chrono::milliseconds{500});
      disconnect_timer_.async_wait(
          [self = shared_from_this()](const boost::system::error_code& error) {
            if (!error) self->Finalize();
          });
      StartWrite();
      if (transport_.State() == runtime::SessionTransportState::kClosed)
        Finalize();
      return;
    }
    if (const auto error = handler_->ApplyDispatch(dispatch);
        error.has_value()) {
      logger_.Log(application::LogLevel::kError, error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    const auto enqueue_result = transport_.Enqueue(dispatch.messages);
    if (const auto* error =
            std::get_if<application::ServiceError>(&enqueue_result)) {
      logger_.Log(application::LogLevel::kWarning, error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    LogMovementTransportPressure();
    StartWrite();
  }

  void StartWrite() {
    if (finalized_) {
      return;
    }
    const auto next = transport_.BeginWrite();
    if (!next.has_value()) {
      return;
    }
    boost::asio::async_write(
        socket_, boost::asio::buffer(next->bytes.data(), next->bytes.size()),
        [self = shared_from_this(), sequence = next->sequence](
            const boost::system::error_code& error, std::size_t transferred) {
          self->InvokeGuarded("TCP session write callback failed", [&] {
            self->HandleWrite(error, sequence, transferred);
          });
        });
  }

  /**
   * @brief 在单 Session
   * 边界拦截遗漏的回调异常，记录具体阶段和异常文本后关闭该连接。
   * @note Asio 会把 handler 异常传播出
   * `io_context::run`；若不在这里隔离，一个坏连接会永久 杀死共享 I/O
   * worker，并使会话停在已连接但不再读写的状态。
   */
  template <typename Action>
  void InvokeGuarded(std::string_view failure_message,
                     Action&& action) noexcept {
    try {
      std::forward<Action>(action)();
    } catch (const std::exception& error) {
      logger_.Log(application::LogLevel::kError, failure_message,
                  std::to_string(connection_id_));
      logger_.Log(application::LogLevel::kError, error.what(),
                  std::to_string(connection_id_));
      Finalize();
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical, failure_message,
                  std::to_string(connection_id_));
      Finalize();
    }
  }

  void HandleWrite(const boost::system::error_code& error,
                   std::uint64_t sequence, std::size_t transferred) {
    if (finalized_) {
      return;
    }
    if (error) {
      static_cast<void>(transport_.FailWrite(sequence));
      Finalize();
      return;
    }
    if (const auto completion_error = transport_.CompleteWrite(
            {.sequence = sequence, .transferred = transferred});
        completion_error.has_value()) {
      logger_.Log(application::LogLevel::kError, completion_error->safe_message,
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    if (!reading_started_) {
      reading_started_ = true;
      StartRead();
    }
    if (transport_.State() == runtime::SessionTransportState::kClosed) {
      Finalize();
      return;
    }
    StartWrite();
  }

  void Finalize() noexcept {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    accepting_dispatches_.store(false, std::memory_order_release);
    {
      std::scoped_lock lock{pending_dispatch_mutex_};
      pending_dispatches_.clear();
      pending_movement_dispatches_.clear();
      pending_dispatch_bytes_ = 0U;
      dispatch_posted_ = false;
    }
    try {
      if (handler_) {
        handler_->BeginClose();
      }
      transport_.BeginClose(runtime::SessionCloseMode::kAbort);
      boost::system::error_code ignored;
      ObserveIoResult(handler_timer_.cancel());
      ObserveIoResult(disconnect_timer_.cancel());
      ObserveIoResult(socket_.cancel(ignored));
      ObserveIoResult(socket_.shutdown(tcp::socket::shutdown_both, ignored));
      ObserveIoResult(socket_.close(ignored));
      handler_.reset();
      registration_ = runtime::SessionRegistration{};
      logger_.Log(application::LogLevel::kDebug, "TCP session closed",
                  std::to_string(connection_id_));
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "TCP session cleanup failed", std::to_string(connection_id_));
    }
    try {
      closed_callback_(connection_id_);
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "TCP session close callback failed",
                  std::to_string(connection_id_));
    }
  }

  std::uint64_t connection_id_{0U};
  tcp::socket socket_;
  boost::asio::steady_timer handler_timer_;
  boost::asio::steady_timer disconnect_timer_;
  runtime::SessionTransportCore transport_;
  const std::size_t kMaximumPendingDispatchBytes;
  runtime::SessionRegistry& registry_;
  SessionPacketHandlerFactory& handler_factory_;
  application::EventLogger& logger_;
  ClosedCallback closed_callback_;
  MovementPressureCallback movement_pressure_callback_;
  runtime::SessionRegistration registration_;
  std::unique_ptr<SessionPacketHandler> handler_;
  std::array<std::uint8_t, kReadBufferBytes> read_buffer_{};
  std::deque<PendingPacket> pending_packets_;
  std::uint16_t active_opcode_{0U};
  std::chrono::steady_clock::time_point active_decoded_at_{};
  std::chrono::steady_clock::time_point active_started_at_{};
  std::chrono::steady_clock::time_point last_movement_latency_log_{};
  std::chrono::steady_clock::time_point last_movement_transport_log_{};
  std::size_t last_reported_movement_coalesced_{0U};
  std::size_t last_reported_movement_dropped_{0U};
  std::size_t last_logged_movement_coalesced_{0U};
  std::size_t last_logged_movement_dropped_{0U};
  std::mutex pending_dispatch_mutex_;
  std::deque<runtime::SessionDispatch> pending_dispatches_;
  std::map<PendingMovementKey, runtime::OutboundMessage>
      pending_movement_dispatches_;
  std::size_t pending_dispatch_bytes_{0U};
  bool dispatch_posted_{false};
  std::atomic<bool> accepting_dispatches_{true};
  std::atomic<bool> handler_work_pending_{false};
  bool read_in_progress_{false};
  bool reading_started_{false};
  bool handling_packet_{false};
  bool finalized_{false};
  bool disconnect_draining_{false};
};

}  // namespace

class TcpServer::Impl final {
 public:
  Impl(TcpServerOptions options, runtime::SessionRegistry& registry,
       SessionPacketHandlerFactory& handler_factory,
       application::EventLogger& logger)
      : options_(std::move(options)),
        registry_(registry),
        handler_factory_(handler_factory),
        logger_(logger),
        acceptor_(io_context_) {}

  std::optional<application::ServiceError> Start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "TCP 服务器只能启动一次");
    }
    if (options_.bind_address.empty() || options_.io_threads == 0U ||
        options_.maximum_connections == 0U ||
        protocol::ValidateProtocolLimits(options_.protocol_limits)
            .has_value()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "TCP 服务器配置无效");
    }
    try {
      const auto address = boost::asio::ip::make_address(options_.bind_address);
      const tcp::endpoint endpoint{address, options_.port};
      acceptor_.open(endpoint.protocol());
      acceptor_.set_option(tcp::acceptor::reuse_address{true});
      acceptor_.bind(endpoint);
      acceptor_.listen(boost::asio::socket_base::max_listen_connections);
      local_port_.store(acceptor_.local_endpoint().port(),
                        std::memory_order_release);
      running_.store(true, std::memory_order_release);
      AcceptNext();
      threads_.reserve(options_.io_threads);
      for (std::size_t index = 0; index < options_.io_threads; ++index) {
        threads_.emplace_back([this] {
          while (true) {
            try {
              io_context_.run();
              return;
            } catch (const std::exception& error) {
              logger_.Log(
                  application::LogLevel::kError,
                  "TCP I/O worker recovered from unhandled callback exception");
              logger_.Log(application::LogLevel::kError, error.what());
            } catch (...) {
              logger_.Log(
                  application::LogLevel::kCritical,
                  "TCP I/O worker recovered from unknown callback exception");
            }
          }
        });
      }
      logger_.Log(application::LogLevel::kDebug,
                  "TCP listener ready: address=" + options_.bind_address +
                      " port=" + std::to_string(LocalPort()));
      return std::nullopt;
    } catch (const boost::system::system_error&) {
      running_.store(false, std::memory_order_release);
      CloseAcceptor();
      JoinThreads();
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "TCP 监听端点打开或绑定失败", true);
    } catch (...) {
      running_.store(false, std::memory_order_release);
      CloseAcceptor();
      io_context_.stop();
      JoinThreads();
      return MakeError(application::ErrorCode::kInternal, "TCP 服务器启动失败");
    }
  }

  void Stop(std::chrono::milliseconds grace) noexcept {
    try {
      StopImpl(grace);
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "TCP server graceful stop failed; forcing shutdown");
      ForceStopAfterException();
    }
  }

  void StopImpl(std::chrono::milliseconds grace) {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
      JoinThreads();
      return;
    }
    try {
      boost::asio::post(io_context_, [this] {
        CloseAcceptor();
        std::vector<std::shared_ptr<TcpSession>> snapshot;
        {
          std::scoped_lock lock{sessions_mutex_};
          snapshot.reserve(sessions_.size());
          for (const auto& [id, session] : sessions_) {
            static_cast<void>(id);
            snapshot.push_back(session);
          }
        }
        for (const auto& session : snapshot) {
          session->RequestStop();
        }
      });
      {
        std::unique_lock lock{sessions_mutex_};
        sessions_closed_.wait_for(lock, grace,
                                  [this] { return sessions_.empty(); });
      }
    } catch (...) {
      logger_.Log(application::LogLevel::kWarning,
                  "TCP server graceful session drain failed");
    }
    io_context_.stop();
    JoinThreads();
    {
      std::scoped_lock lock{sessions_mutex_};
      sessions_.clear();
    }
    local_port_.store(0U, std::memory_order_release);
    logger_.Log(application::LogLevel::kDebug, "TCP listener stopped");
  }

  [[nodiscard]] std::uint16_t LocalPort() const noexcept {
    return local_port_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept {
    std::scoped_lock lock{sessions_mutex_};
    return sessions_.size();
  }

  [[nodiscard]] std::uint64_t MovementCoalescedTotal() const noexcept {
    return movement_coalesced_total_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t MovementDroppedTotal() const noexcept {
    return movement_dropped_total_.load(std::memory_order_acquire);
  }

 private:
  void AcceptNext() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    acceptor_.async_accept(
        boost::asio::make_strand(io_context_),
        [this](const boost::system::error_code& error, tcp::socket socket) {
          if (!error && running_.load(std::memory_order_acquire)) {
            AcceptSocket(std::move(socket));
          } else if (error != boost::asio::error::operation_aborted &&
                     running_.load(std::memory_order_acquire)) {
            logger_.Log(application::LogLevel::kWarning, "TCP accept failed");
          }
          AcceptNext();
        });
  }

  void AcceptSocket(tcp::socket socket) {
    std::shared_ptr<TcpSession> session;
    {
      std::scoped_lock lock{sessions_mutex_};
      if (sessions_.size() >= options_.maximum_connections) {
        boost::system::error_code ignored;
        ObserveIoResult(socket.close(ignored));
        logger_.Log(application::LogLevel::kWarning,
                    "TCP connection limit reached");
        return;
      }
      const auto connection_id = next_connection_id_++;
      session = std::make_shared<TcpSession>(
          connection_id, std::move(socket), options_.protocol_limits, registry_,
          handler_factory_, logger_,
          [this](std::uint64_t closed_id) { SessionClosed(closed_id); },
          options_.frame_namespace,
          [this](std::uint64_t coalesced, std::uint64_t dropped) noexcept {
            movement_coalesced_total_.fetch_add(coalesced,
                                                std::memory_order_relaxed);
            movement_dropped_total_.fetch_add(dropped,
                                              std::memory_order_relaxed);
          });
      sessions_.emplace(connection_id, session);
    }
    session->Start();
  }

  void SessionClosed(std::uint64_t connection_id) {
    {
      std::scoped_lock lock{sessions_mutex_};
      sessions_.erase(connection_id);
    }
    sessions_closed_.notify_all();
  }

  void CloseAcceptor() noexcept {
    boost::system::error_code ignored;
    ObserveIoResult(acceptor_.cancel(ignored));
    ObserveIoResult(acceptor_.close(ignored));
  }

  void JoinThreads() {
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  /** @brief 正常停机异常时收束
   * I/O、线程和会话；再次失败表示进程状态已不可恢复。 */
  void ForceStopAfterException() noexcept {
    try {
      io_context_.stop();
      JoinThreads();
      {
        std::scoped_lock lock{sessions_mutex_};
        sessions_.clear();
      }
      local_port_.store(0U, std::memory_order_release);
    } catch (...) {
      std::terminate();
    }
  }

  TcpServerOptions options_;
  runtime::SessionRegistry& registry_;
  SessionPacketHandlerFactory& handler_factory_;
  application::EventLogger& logger_;
  boost::asio::io_context io_context_;
  tcp::acceptor acceptor_;
  mutable std::mutex sessions_mutex_;
  std::condition_variable sessions_closed_;
  std::map<std::uint64_t, std::shared_ptr<TcpSession>> sessions_;
  std::vector<std::thread> threads_;
  std::atomic<bool> started_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint16_t> local_port_{0U};
  std::atomic<std::uint64_t> movement_coalesced_total_{0U};
  std::atomic<std::uint64_t> movement_dropped_total_{0U};
  std::uint64_t next_connection_id_{1U};
};

TcpServer::TcpServer(TcpServerOptions options,
                     runtime::SessionRegistry& registry,
                     SessionPacketHandlerFactory& handler_factory,
                     application::EventLogger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), registry,
                                   handler_factory, logger)) {}

TcpServer::~TcpServer() { impl_->Stop(std::chrono::milliseconds{0}); }

std::optional<application::ServiceError> TcpServer::Start() {
  return impl_->Start();
}

void TcpServer::Stop(std::chrono::milliseconds grace) noexcept {
  impl_->Stop(grace);
}

std::uint16_t TcpServer::LocalPort() const noexcept {
  return impl_->LocalPort();
}

std::size_t TcpServer::ActiveSessionCount() const noexcept {
  return impl_->ActiveSessionCount();
}

std::uint64_t TcpServer::MovementCoalescedTotal() const noexcept {
  return impl_->MovementCoalescedTotal();
}

std::uint64_t TcpServer::MovementDroppedTotal() const noexcept {
  return impl_->MovementDroppedTotal();
}

}  // namespace bf::infrastructure::network
