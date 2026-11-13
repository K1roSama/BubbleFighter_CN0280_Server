#include "protocol/internal/room_envelope.h"

#include <algorithm>
#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol::internal {
namespace {
constexpr std::uint32_t kMagic = 0x53444642U;

void ValidateKind(RoomMessageKind kind) {
  if (kind < RoomMessageKind::kAuthenticate || kind > RoomMessageKind::kError) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid internal message kind"};
  }
}
}  // namespace

bool RoomIdentity::IsValid() const noexcept {
  return room_id.IsValid() && room_id.Value() <= 32767U && owner_epoch != 0U &&
         owner_epoch <= static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()) &&
         std::ranges::any_of(instance_id, [](auto byte) { return byte != 0U; });
}

std::vector<std::uint8_t> EncodeRoomEnvelope(const RoomEnvelope& message) {
  ValidateKind(message.kind);
  if (message.payload.size() > kMaximumRoomPayloadBytes) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "internal payload too large"};
  }
  PacketWriter out;
  out.WriteU32(static_cast<std::uint32_t>(kRoomEnvelopeHeaderBytes - 4U +
                                          message.payload.size()));
  out.WriteU32(kMagic);
  out.WriteU16(kRoomProtocolVersion);
  out.WriteU16(static_cast<std::uint16_t>(message.kind));
  out.WriteU64(message.request_id);
  out.WriteU64(message.identity.room_id.Value());
  out.WriteBytes(message.identity.instance_id);
  out.WriteU64(message.identity.owner_epoch);
  out.WriteU64(message.room_generation);
  out.WriteBytes(message.match_id);
  out.WriteU64(message.player_id.Value());
  out.WriteU64(message.session_id);
  out.WriteU64(message.session_generation);
  out.WriteU64(message.sequence);
  out.WriteBytes(message.payload);
  return out.Release();
}

RoomEnvelope DecodeRoomEnvelope(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kRoomEnvelopeHeaderBytes ||
      bytes.size() - kRoomEnvelopeHeaderBytes > kMaximumRoomPayloadBytes) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid internal frame length"};
  }
  PacketReader in{bytes};
  if (in.ReadU32() != bytes.size() - 4U || in.ReadU32() != kMagic ||
      in.ReadU16() != kRoomProtocolVersion) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid internal frame header"};
  }
  RoomEnvelope message;
  message.kind = static_cast<RoomMessageKind>(in.ReadU16());
  ValidateKind(message.kind);
  message.request_id = in.ReadU64();
  message.identity.room_id = domain::RoomId{in.ReadU64()};
  auto instance = in.ReadBytes(message.identity.instance_id.size());
  std::ranges::copy(instance, message.identity.instance_id.begin());
  message.identity.owner_epoch = in.ReadU64();
  message.room_generation = in.ReadU64();
  auto match = in.ReadBytes(message.match_id.size());
  std::ranges::copy(match, message.match_id.begin());
  message.player_id = domain::PlayerId{in.ReadU64()};
  message.session_id = in.ReadU64();
  message.session_generation = in.ReadU64();
  message.sequence = in.ReadU64();
  auto payload = in.ReadBytes(bytes.size() - kRoomEnvelopeHeaderBytes);
  message.payload.assign(payload.begin(), payload.end());
  in.RequireEnd();
  return message;
}
}  // namespace bf::protocol::internal
