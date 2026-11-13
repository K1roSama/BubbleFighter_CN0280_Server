#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "features/rooms/room_command_flow.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
struct MapRegion final {
  std::string_view map;
  std::uint16_t region;
  std::uint32_t kind;
  std::array<float, 6U> bounds;
  std::array<float, 3U> destination;
};

constexpr MapRegion kRegions[]{
#include "features/scenes/generated/map_regions.inc"
};

bool Contains(const MapRegion& region, const std::array<float, 3U>& position) {
  for (std::size_t axis = 0; axis < 3U; ++axis) {
    if (position[axis] < region.bounds[axis] ||
        position[axis] > region.bounds[axis + 3U])
      return false;
  }
  return true;
}
}  // namespace

RoomCommandResult RoomCommandFlow::HandleMapRegion(
    domain::PlayerId player, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{packet.body};
  const auto member = room_->GetMember(player).value();
  const auto object = packet.opcode == 32922U ? reader.ReadU32() : 0U;
  const std::array<float, 3U> position{reader.ReadF32(), reader.ReadF32(),
                                       reader.ReadF32()};
  const auto region_id =
      packet.opcode == 950U ? reader.ReadU16() : std::uint16_t{0U};
  reader.RequireEnd();
  if (!std::ranges::all_of(position,
                           [](float value) { return std::isfinite(value); })) {
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "map region position is nonfinite"};
  }
  auto result = MakeResult(RoomEvent::kNoChange);
  const bool playing = room_->Phase() == RoomPhase::kPlaying && combat_ &&
                       (!objectives_ || !objectives_->BetweenRounds());
  const auto state = combat_ ? combat_->LifeState(player) : std::nullopt;
  const bool eligible = playing && member.object_id && state &&
                        *state != CombatLifeState::kDefeated;
  if (packet.opcode == 32922U) {
    // 8CAEF0 -> 7C6760: the local portal recipient requests its destination;
    // 7DECD0 applies it even to the local actor, so an owner echo is required.
    if (member.object_id && object != *member.object_id)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kInvalidValue,
          "portal confirmation belongs to another character"};
    auto pending = map_region_pending_.find(player);
    if (!eligible || pending == map_region_pending_.end() ||
        pending->second.completed || pending->second.kind != 0U ||
        now - pending->second.accepted_at > std::chrono::seconds{3} ||
        combat_->LifeGeneration(player) != pending->second.life) {
      result.combat_reject_reason = CombatRejectReason::kInvalidLifecycleState;
      return result;
    }
    for (std::size_t axis = 0; axis < 3U; ++axis) {
      if (std::abs(position[axis] - pending->second.destination[axis]) >
          0.01F) {
        result.combat_reject_reason = CombatRejectReason::kUnknownTarget;
        return result;
      }
    }
    auto& actor = combat_->actors_.at(player);
    if (!combat_->ConsumeActionQuota(actor, now)) {
      result.combat_reject_reason = CombatRejectReason::kRateLimited;
      return result;
    }
    pending->second.completed = true;
    actor.position = position;
    if (crazy_racing_)
      crazy_racing_->ObservePosition(player, position, *combat_, now);
    for (const auto& target : room_->Members())
      result.deliveries.push_back(
          {target.player_id, {packet.opcode, packet.body}});
    return result;
  }
  std::string map = scene_.configuration.map_key;
  std::ranges::transform(map, map.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  const auto match = std::ranges::find_if(kRegions, [&](const auto& region) {
    return region.map == map && region.region == region_id &&
           Contains(region, position);
  });
  auto previous = map_region_pending_.find(player);
  bool accepted =
      eligible && match != std::end(kRegions) &&
      (previous == map_region_pending_.end() ||
       combat_->LifeGeneration(player) != previous->second.life ||
       now - previous->second.accepted_at >= std::chrono::milliseconds{100});
  if (accepted) {
    accepted = combat_->ConsumeActionQuota(combat_->actors_.at(player), now);
    if (accepted)
      map_region_pending_[player] = {*combat_->LifeGeneration(player),
                                     region_id,
                                     match->kind,
                                     match->destination,
                                     now,
                                     false};
  }
  // B7F600/S2C960: result DWORD (-1 rejects), region WORD, player SLOT DWORD.
  // Even a rejected request must clear the local Character's pending byte
  // 31600.
  protocol::PacketWriter writer;
  writer.WriteU32(accepted ? 0U : 0xFFFFFFFFU);
  writer.WriteU16(region_id);
  writer.WriteU32(member.seat);
  const OutboundMessage response{960U, writer.Release()};
  for (const auto& target : room_->Members()) {
    if (accepted || target.player_id == player)
      result.deliveries.push_back({target.player_id, response});
  }
  if (!accepted)
    result.combat_reject_reason =
        playing ? CombatRejectReason::kInvalidLifecycleState
                : CombatRejectReason::kInactiveMatchPhase;
  return result;
}
}  // namespace bf::runtime
