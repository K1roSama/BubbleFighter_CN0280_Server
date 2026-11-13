#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_ITEM_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_ITEM_MESSAGES_H_

#include "features/rooms/room_item_policy.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {
inline constexpr std::uint16_t kRoomItemRequest = 65090U,
                               kRoomItemResponse = 65091U;
inline constexpr std::uint16_t kLegacyRoomItemRequest = 65082U,
                               kLegacyRoomItemResponse = 65083U;

// Released r4 requests are DWORD-aligned (32 + 4*N); every hide-seek v1
// request is 21, 25, 33, 34 or 46 bytes. Decode/validate after dispatch.
inline bool LegacyRoomItemLayout(std::span<const std::uint8_t> body) noexcept {
  return body.size() >= 32U && body.size() % 4U == 0U;
}

struct RoomItemRequest {
  std::uint32_t action{}, room{};
  std::uint64_t generation{}, revision{};
  std::vector<std::uint32_t> items;
};

inline RoomItemRequest DecodeRoomItemRequest(
    std::span<const std::uint8_t> body) {
  PacketReader r{body};
  RoomItemRequest q;
  if (r.ReadU32() != 1U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unsupported room item version"};
  q.action = r.ReadU32();
  q.room = r.ReadU32();
  q.generation = r.ReadU64();
  q.revision = r.ReadU64();
  const auto n = r.ReadU32();
  if (q.action > 1U || q.room > 65535U || n > domain::kMaxRoomRestrictedItems ||
      (!q.action && n))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid room item request"};
  for (std::uint32_t i = 0; i < n; ++i) q.items.push_back(r.ReadU32());
  r.RequireEnd();
  if (!domain::ValidRoomItems(q.items))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown or duplicate room items"};
  return q;
}

inline std::vector<std::uint8_t> EncodeRoomItemState(
    std::uint32_t room, std::uint64_t generation, std::uint64_t revision,
    bool editable, std::uint32_t status, std::span<const std::uint32_t> items) {
  PacketWriter w;
  w.WriteU32(1U);
  w.WriteU32(room);
  w.WriteU64(generation);
  w.WriteU64(revision);
  w.WriteU32(editable ? 1U : 0U);
  w.WriteU32(status);
  w.WriteU32(static_cast<std::uint32_t>(items.size()));
  for (auto id : items) w.WriteU32(id);
  return w.Release();
}
}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_ITEM_MESSAGES_H_
