#include "sessions/session_transport_core.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace bf::runtime {
namespace {

constexpr std::size_t kServerHelloBytes = 258U;
constexpr std::size_t kFrameHeaderBytes = 5U;
constexpr std::uint16_t kExtendedChunkOpcode = 65'000U;
constexpr std::size_t kExtendedChunkHeaderBytes = 14U;
constexpr std::size_t kExtendedChunkBodyBytes = 60'000U;

[[nodiscard]] bool SupportsExtendedPayload(std::uint16_t opcode) noexcept {
  return opcode == 800U || opcode == 801U || opcode == 802U;
}

[[nodiscard]] std::size_t EncodedMessageBytes(const OutboundMessage& message) {
  if (message.body.size() <= std::numeric_limits<std::uint16_t>::max()) {
    return kFrameHeaderBytes + message.body.size();
  }
  if (!SupportsExtendedPayload(message.opcode) ||
      message.body.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kLengthLimit,
        "outgoing frame exceeds supported payload length"};
  }
  const auto chunk_count =
      (message.body.size() + kExtendedChunkBodyBytes - 1U) /
      kExtendedChunkBodyBytes;
  constexpr auto overhead = kFrameHeaderBytes + kExtendedChunkHeaderBytes;
  if (chunk_count >
      (std::numeric_limits<std::size_t>::max() - message.body.size()) /
          overhead) {
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                  "extended outgoing payload size overflows"};
  }
  return message.body.size() + chunk_count * overhead;
}

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

}  // namespace

SessionTransportCore::SessionTransportCore(
    protocol::ProtocolLimits limits, protocol::FrameNamespace message_namespace)
    : limits_(limits),
      decoder_(limits.maximum_packet_bytes, 524'288U, message_namespace),
      outbound_cipher_(protocol::LegacyCipher::ForEndpoint(
          protocol::CipherEndpoint::kServerToClient)) {
  if (ValidateProtocolLimits(limits_).has_value() ||
      limits_.maximum_send_queue_bytes < kServerHelloBytes) {
    throw std::invalid_argument{"invalid session transport limits"};
  }
  const auto hello = protocol::MakeServerHello();
  send_queue_.push_back(QueuedBuffer{.sequence = next_sequence_,
                                     .bytes = {hello.begin(), hello.end()}});
  ++next_sequence_;
  queued_bytes_ = hello.size();
}

application::ServiceResult<std::vector<protocol::Packet>>
SessionTransportCore::FeedReceived(
    std::span<const std::uint8_t> encrypted_bytes) {
  if (state_ != SessionTransportState::kOpen) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "连接关闭流程中不能继续接收协议字节");
  }
  try {
    return decoder_.Feed(encrypted_bytes);
  } catch (const protocol::ProtocolError&) {
    BeginClose(SessionCloseMode::kAbort);
    throw;
  }
}

application::ServiceResult<std::size_t> SessionTransportCore::Enqueue(
    std::span<const OutboundMessage> messages) {
  if (state_ != SessionTransportState::kOpen) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "连接关闭流程中不能加入出站消息");
  }
  if (messages.empty()) {
    return queued_bytes_;
  }

  // Validate and split the batch before changing queue state. Movement is a
  // latest-state stream; all other messages retain the existing reliable FIFO.
  std::vector<OutboundMessage> reliable;
  reliable.reserve(messages.size());
  std::size_t reliable_bytes = 0U;
  std::map<MovementKey, OutboundMessage> movement_updates;
  for (const auto& message : messages) {
    const auto frame_bytes = EncodedMessageBytes(message);
    if (const auto key = GetMovementKey(message); key.has_value()) {
      movement_updates.insert_or_assign(*key, message);
      continue;
    }
    if (frame_bytes >
        std::numeric_limits<std::size_t>::max() - reliable_bytes) {
      BeginClose(SessionCloseMode::kAbort);
      return MakeError(application::ErrorCode::kOverloaded,
                       "单连接发送批次字节数溢出，连接已关闭");
    }
    reliable_bytes += frame_bytes;
    reliable.push_back(message);
  }

  const auto max_sequence = std::numeric_limits<std::uint64_t>::max();
  const auto needs_movement_buffer =
      !pending_movement_.empty() || !movement_updates.empty();
  const auto buffers_needed =
      (needs_movement_buffer ? 1U : 0U) + (reliable.empty() ? 0U : 1U);
  const auto last_allocatable_sequence = max_sequence - 1U;
  if (buffers_needed != 0U &&
      next_sequence_ > last_allocatable_sequence -
                           static_cast<std::uint64_t>(buffers_needed - 1U)) {
    BeginClose(SessionCloseMode::kAbort);
    return MakeError(application::ErrorCode::kInternal, "连接写序号已经耗尽");
  }

  if (!reliable.empty()) {
    // Pending movement is disposable. Free it first so an accepted hit/death/
    // room-state batch never loses its reliable path to stale transforms.
    if (reliable_bytes >
        limits_.maximum_send_queue_bytes -
            std::min(queued_bytes_, limits_.maximum_send_queue_bytes)) {
      DiscardPendingMovement();
    }
    if (reliable_bytes >
        limits_.maximum_send_queue_bytes -
            std::min(queued_bytes_, limits_.maximum_send_queue_bytes)) {
      BeginClose(SessionCloseMode::kAbort);
      return MakeError(application::ErrorCode::kOverloaded,
                       "单连接可靠发送队列超过配置上限，连接已关闭");
    }
  }

  for (auto& [key, message] : movement_updates) {
    static_cast<void>(key);
    QueueMovement(std::move(message), reliable_bytes);
  }
  if (!reliable.empty()) {
    // Encode the latest movement snapshot before the reliable batch. This
    // keeps cipher order deterministic while allowing later movement packets
    // to replace it when the socket is already busy.
    FlushPendingMovement();
    auto bytes = EncodeBatch(reliable);
    const auto sequence = next_sequence_;
    send_queue_.push_back(
        QueuedBuffer{.sequence = sequence, .bytes = std::move(bytes)});
    outbound_cipher_.Encrypt(send_queue_.back().bytes);
    ++next_sequence_;
    queued_bytes_ += send_queue_.back().bytes.size();
  }
  return queued_bytes_;
}

application::ServiceResult<std::size_t> SessionTransportCore::Enqueue(
    const OutboundMessage& message) {
  // 直接借用单条消息组成 span，避免为单包发送复制整份正文。
  return Enqueue(std::span<const OutboundMessage>{&message, 1U});
}

std::optional<SessionWriteView> SessionTransportCore::BeginWrite() {
  if (!active_sequence_.has_value() && !pending_movement_.empty()) {
    FlushPendingMovement();
  }
  if (active_sequence_.has_value() || send_queue_.empty()) {
    return std::nullopt;
  }
  active_sequence_ = send_queue_.front().sequence;
  return SessionWriteView{.sequence = send_queue_.front().sequence,
                          .bytes = send_queue_.front().bytes};
}

std::optional<application::ServiceError> SessionTransportCore::CompleteWrite(
    SessionWriteCompletion completion) {
  if (auto error = ValidateCompletion(completion.sequence); error.has_value()) {
    return error;
  }
  if (completion.transferred != send_queue_.front().bytes.size()) {
    send_queue_.clear();
    pending_movement_.clear();
    pending_movement_bytes_ = 0U;
    queued_bytes_ = 0U;
    active_sequence_.reset();
    state_ = SessionTransportState::kClosed;
    return MakeError(application::ErrorCode::kInternal,
                     "异步写 completion 未覆盖完整队首 buffer");
  }

  queued_bytes_ -= send_queue_.front().bytes.size();
  send_queue_.pop_front();
  active_sequence_.reset();
  if (state_ == SessionTransportState::kClosing && send_queue_.empty()) {
    state_ = SessionTransportState::kClosed;
  }
  return std::nullopt;
}

std::optional<application::ServiceError> SessionTransportCore::FailWrite(
    std::uint64_t sequence) {
  if (auto error = ValidateCompletion(sequence); error.has_value()) {
    return error;
  }
  send_queue_.clear();
  pending_movement_.clear();
  pending_movement_bytes_ = 0U;
  queued_bytes_ = 0U;
  active_sequence_.reset();
  state_ = SessionTransportState::kClosed;
  return std::nullopt;
}

void SessionTransportCore::BeginClose(SessionCloseMode mode) {
  if (state_ == SessionTransportState::kClosed) {
    return;
  }
  state_ = SessionTransportState::kClosing;
  if (mode == SessionCloseMode::kAbort) {
    DiscardAfterActive();
  } else {
    FlushPendingMovement();
  }
  if (!active_sequence_.has_value() && send_queue_.empty()) {
    state_ = SessionTransportState::kClosed;
  }
}

SessionTransportState SessionTransportCore::State() const noexcept {
  return state_;
}

std::size_t SessionTransportCore::QueuedBytes() const noexcept {
  return queued_bytes_;
}

std::size_t SessionTransportCore::QueuedBuffers() const noexcept {
  return send_queue_.size() + (pending_movement_.empty() ? 0U : 1U);
}

std::size_t SessionTransportCore::BufferedReceiveBytes() const noexcept {
  return decoder_.BufferedBytes();
}

bool SessionTransportCore::WriteInProgress() const noexcept {
  return active_sequence_.has_value();
}

std::size_t SessionTransportCore::MovementCoalescedCount() const noexcept {
  return movement_coalesced_count_;
}

std::size_t SessionTransportCore::MovementDroppedCount() const noexcept {
  return movement_dropped_count_;
}

std::size_t SessionTransportCore::PendingMovementCount() const noexcept {
  return pending_movement_.size();
}

std::optional<SessionTransportCore::MovementKey>
SessionTransportCore::GetMovementKey(const OutboundMessage& message) noexcept {
  if ((message.opcode != 32'769U && message.opcode != 32'885U) ||
      message.body.size() < 4U) {
    return std::nullopt;
  }
  const auto read_u32 = [&message](std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(message.body[offset]) |
           (static_cast<std::uint32_t>(message.body[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(message.body[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(message.body[offset + 3U]) << 24U);
  };
  return MovementKey{.opcode = message.opcode, .actor_object_id = read_u32(0U)};
}

void SessionTransportCore::QueueMovement(OutboundMessage message,
                                         std::size_t reserved_bytes) {
  const auto key = GetMovementKey(message);
  if (!key.has_value()) {
    return;
  }
  const auto wire_bytes = EncodedMessageBytes(message);
  const auto existing = pending_movement_.find(*key);
  const auto replaced_bytes = existing == pending_movement_.end()
                                  ? 0U
                                  : EncodedMessageBytes(existing->second);
  const auto base_bytes = queued_bytes_ >= pending_movement_bytes_
                              ? queued_bytes_ - pending_movement_bytes_
                              : 0U;
  const auto pending_without_replaced =
      pending_movement_bytes_ >= replaced_bytes
          ? pending_movement_bytes_ - replaced_bytes
          : 0U;
  const auto max_bytes = limits_.maximum_send_queue_bytes;
  const bool fits =
      base_bytes <= max_bytes &&
      pending_without_replaced <= max_bytes - base_bytes &&
      reserved_bytes <= max_bytes - base_bytes - pending_without_replaced &&
      wire_bytes <=
          max_bytes - base_bytes - pending_without_replaced - reserved_bytes;
  if (!fits) {
    ++movement_dropped_count_;
    return;
  }
  if (existing != pending_movement_.end()) {
    pending_movement_bytes_ = pending_without_replaced;
    queued_bytes_ = base_bytes + pending_movement_bytes_;
    ++movement_coalesced_count_;
  }
  pending_movement_.insert_or_assign(*key, std::move(message));
  pending_movement_bytes_ += wire_bytes;
  queued_bytes_ = base_bytes + pending_movement_bytes_;
}

void SessionTransportCore::DiscardPendingMovement() noexcept {
  if (!pending_movement_.empty()) {
    movement_dropped_count_ += pending_movement_.size();
  }
  if (queued_bytes_ >= pending_movement_bytes_) {
    queued_bytes_ -= pending_movement_bytes_;
  } else {
    queued_bytes_ = 0U;
  }
  pending_movement_.clear();
  pending_movement_bytes_ = 0U;
}

void SessionTransportCore::FlushPendingMovement() {
  if (pending_movement_.empty()) {
    return;
  }
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    DiscardPendingMovement();
    BeginClose(SessionCloseMode::kAbort);
    return;
  }
  std::vector<OutboundMessage> messages;
  messages.reserve(pending_movement_.size());
  for (auto& [key, message] : pending_movement_) {
    static_cast<void>(key);
    messages.push_back(std::move(message));
  }
  const auto pending_bytes = pending_movement_bytes_;
  pending_movement_.clear();
  pending_movement_bytes_ = 0U;
  queued_bytes_ =
      queued_bytes_ >= pending_bytes ? queued_bytes_ - pending_bytes : 0U;
  auto bytes = EncodeBatch(messages);
  const auto sequence = next_sequence_;
  send_queue_.push_back(
      QueuedBuffer{.sequence = sequence, .bytes = std::move(bytes)});
  outbound_cipher_.Encrypt(send_queue_.back().bytes);
  ++next_sequence_;
  queued_bytes_ += send_queue_.back().bytes.size();
}

std::vector<std::uint8_t> SessionTransportCore::EncodeBatch(
    std::span<const OutboundMessage> messages) {
  std::size_t total_bytes = 0U;
  for (const auto& message : messages) {
    total_bytes += EncodedMessageBytes(message);
  }

  std::vector<std::uint8_t> batch;
  batch.reserve(total_bytes);
  for (const auto& message : messages) {
    if (message.body.size() <= std::numeric_limits<std::uint16_t>::max()) {
      auto frame = protocol::EncodeFrame(message.opcode, message.body);
      batch.insert(batch.end(), frame.begin(), frame.end());
      continue;
    }

    const auto transfer_id = next_extended_transfer_id_;
    ++next_extended_transfer_id_;
    if (next_extended_transfer_id_ == 0U) {
      next_extended_transfer_id_ = 1U;
    }
    const auto total_size = static_cast<std::uint32_t>(message.body.size());
    for (std::size_t offset = 0U; offset < message.body.size();
         offset += kExtendedChunkBodyBytes) {
      const auto chunk_size =
          std::min(kExtendedChunkBodyBytes, message.body.size() - offset);
      protocol::PacketWriter writer;
      writer.WriteU32(transfer_id);
      writer.WriteU16(message.opcode);
      writer.WriteU32(total_size);
      writer.WriteU32(static_cast<std::uint32_t>(offset));
      writer.WriteBytes(std::span<const std::uint8_t>{message.body}.subspan(
          offset, chunk_size));
      auto frame = protocol::EncodeFrame(kExtendedChunkOpcode, writer.Bytes());
      batch.insert(batch.end(), frame.begin(), frame.end());
    }
  }
  return batch;
}

std::optional<application::ServiceError>
SessionTransportCore::ValidateCompletion(std::uint64_t sequence) const {
  if (!active_sequence_.has_value() || send_queue_.empty() ||
      *active_sequence_ != sequence ||
      send_queue_.front().sequence != sequence) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "异步写 completion 与当前队首序号不一致");
  }
  return std::nullopt;
}

void SessionTransportCore::DiscardAfterActive() {
  DiscardPendingMovement();
  if (!active_sequence_.has_value()) {
    send_queue_.clear();
    queued_bytes_ = 0U;
    return;
  }
  const auto active_bytes = send_queue_.front().bytes.size();
  while (send_queue_.size() > 1U) {
    send_queue_.pop_back();
  }
  queued_bytes_ = active_bytes;
}

const char* SessionTransportStateName(SessionTransportState state) noexcept {
  switch (state) {
    case SessionTransportState::kOpen:
      return "open";
    case SessionTransportState::kClosing:
      return "closing";
    case SessionTransportState::kClosed:
      return "closed";
  }
  return "closed";
}

}  // namespace bf::runtime
