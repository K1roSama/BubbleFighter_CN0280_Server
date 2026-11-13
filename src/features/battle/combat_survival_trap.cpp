#include <algorithm>
#include <cmath>

#include "features/battle/combat_relay.h"
#include "features/battle/survival_loot.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
double DistanceSquared(const std::array<float, 3U>& a,
                       const std::array<float, 3U>& b) {
  double result = 0.0;
  for (unsigned i = 0; i < 3U; ++i) {
    const double delta = static_cast<double>(a[i]) - b[i];
    result += delta * delta;
  }
  return result;
}
}  // namespace

CombatRelayResult CombatRelay::HandleSurvivalTrap(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (mode_id_ != 23U && mode_id_ != 25U)
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  if (actor.controller || actor.life_state != CombatLifeState::kAlive)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  protocol::PacketReader reader{packet.body};
  static_cast<void>(
      reader.ReadU32());  // handle_action authenticated this actor.
  protocol::Packet response;
  if (packet.opcode == 751U) {
    const auto type = reader.ReadU32();
    const auto stage = reader.ReadU32();
    std::array<float, 3U> position{};
    for (auto& axis : position) axis = reader.ReadF32();
    if (type != 560U || (stage != 1U && stage != 3U) || !actor.position ||
        actor.selected_weapon_bar != 3U || EquippedTemplate(actor, 3U) != 5921U)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    const auto held = actor.survival_supplies.find(560U);
    // A25E90 requires two rods before the first placement, one before the
    // second.
    if (held == actor.survival_supplies.end() ||
        held->second < (stage == 1U ? 2U : 1U))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (survival_traps_.size() >= 4096U)
      return RejectedResult(CombatRejectReason::kProjectileLimit);
    std::uint32_t first = 0U;
    for (const auto& [id, rod] : survival_traps_) {
      if (rod.owner != actor.participant.player_id ||
          rod.life != actor.life_generation)
        continue;
      // 751 has no request nonce. Keep exact placement receipts for this life;
      // a repeated position/stage cannot spend another rod after
      // linking/cancel.
      if (rod.stage == stage && rod.position == position)
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      if (!rod.removed && !rod.linked && rod.stage == 1U) first = id;
    }
    if ((stage == 1U && first != 0U) || (stage == 3U && first == 0U))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // Native radius is 4.5 * 15 = 67.5 world units. The extra 25 covers
    // asynchronous movement samples; this does not validate the surface
    // raycast.
    if (DistanceSquared(*actor.position, position) > 92.5 * 92.5)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (first != 0U) {
      const auto& rod = survival_traps_.at(first);
      const auto separation = DistanceSquared(rod.position, position);
      if (rod.peer != 0U || separation <= 0.2 * 0.2 ||
          separation >= 67.5 * 67.5)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    std::array<SurvivalLoot, 1U> identity{};
    AllocateSurvivalLootIds(identity);
    const auto id = identity[0].id;
    survival_traps_.emplace(
        id, SurvivalTrapRod{.owner = actor.participant.player_id,
                            .life = actor.life_generation,
                            .position = position,
                            .stage = stage,
                            .peer = first});
    if (first != 0U) survival_traps_.at(first).peer = id;
    --held->second;
    if (held->second == 0U) actor.survival_supplies.erase(held);
    protocol::PacketWriter writer;
    writer.WriteU32(actor.participant.actor_object_id);
    writer.WriteU32(id);
    writer.WriteU32(560U);
    writer.WriteU32(stage);
    for (auto axis : position) writer.WriteF32(axis);
    response = {.opcode = 760U, .body = writer.Release()};
  } else {
    const auto first = reader.ReadU32();
    const auto second = reader.ReadU32();
    const auto weapon = reader.ReadU32();
    const auto a = survival_traps_.find(first),
               b = survival_traps_.find(second);
    if (weapon != 5921U || a == survival_traps_.end() ||
        b == survival_traps_.end() ||
        a->second.owner != actor.participant.player_id ||
        b->second.owner != actor.participant.player_id ||
        a->second.life != actor.life_generation ||
        b->second.life != actor.life_generation || a->second.stage != 1U ||
        b->second.stage != 3U || a->second.peer != second ||
        b->second.peer != first || a->second.removed || b->second.removed)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (a->second.linked || b->second.linked)
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    a->second.linked = b->second.linked = true;
    response =
        packet;  // 7C25D0 -> 7DDBC0 creates the real GoBoobyTrap on every peer.
  }
  auto result = AcceptedResult(actor.participant.player_id, response);
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = response.opcode, .body = response.body}});
  return result;
}

bool CombatRelay::CollectSurvivalTrap(
    ActorState& actor, std::uint32_t id,
    std::chrono::steady_clock::time_point now) {
  auto& rod = survival_traps_.at(id);
  const auto held = actor.survival_supplies.find(560U);
  const auto quantity =
      held == actor.survival_supplies.end() ? 0U : held->second;
  // 5838A0 cancels the owner's first, unconnected rod when the cursor goes
  // beyond 150 units; no evidence yet grants pickup of an armed pair.
  if (rod.owner != actor.participant.player_id ||
      rod.life != actor.life_generation || rod.stage != 1U || rod.peer != 0U ||
      rod.linked || rod.removed || quantity >= 999U || !actor.position ||
      DistanceSquared(*actor.position, rod.position) > 300.0 * 300.0 ||
      (held == actor.survival_supplies.end() &&
       actor.survival_supplies.size() >= 10U) ||
      !ConsumeActionQuota(actor, now))
    return false;
  rod.removed = true;
  rod.removed_at = now;
  actor.survival_supplies[560U] = quantity + 1U;
  return true;
}

CombatRelayResult CombatRelay::DestroySurvivalTrap(
    ActorState& actor, const protocol::Packet& packet, std::uint32_t id,
    std::chrono::steady_clock::time_point now) {
  auto& rod = survival_traps_.at(id);
  if (rod.owner != actor.participant.player_id || !rod.linked)
    return RejectedResult(CombatRejectReason::kForeignProjectile);
  if (rod.removed) return RejectedResult(CombatRejectReason::kDuplicateAction);
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  rod.removed = true;
  rod.removed_at = now;
  auto result = AcceptedResult(actor.participant.player_id, packet);
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = packet.opcode, .body = packet.body}});
  return result;
}

std::optional<std::uint32_t> CombatRelay::SurvivalTrapBlast(
    const ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) const {
  using namespace std::chrono_literals;
  if ((mode_id_ != 23U && mode_id_ != 25U) || actor.controller)
    return std::nullopt;
  protocol::PacketReader reader{std::span{packet.body}.subspan(7U, 36U)};
  std::array<float, 3U> position{};
  for (auto& axis : position) axis = reader.ReadF32();
  for (unsigned i = 0U; i < 6U; ++i)
    if (reader.ReadF32() != (i == 4U ? 1.0F : 0.0F)) return std::nullopt;
  if (packet.body[6U] != actor.participant.team_code) return std::nullopt;
  for (const auto& [id, a] : survival_traps_) {
    if (a.owner != actor.participant.player_id || a.stage != 1U || !a.linked ||
        !a.removed || a.exploded || !a.removed_at || now < *a.removed_at ||
        now - *a.removed_at > 2s)
      continue;
    const auto& b = survival_traps_.at(a.peer);
    if (!b.removed || !b.removed_at || now < *b.removed_at ||
        now - *b.removed_at > 2s)
      continue;
    // 62D540 lifts both endpoints by 0.26 * 15. 7DDE70 returns a point
    // projected onto that segment. Two units cover float/serialization noise.
    double dot = 0.0, length = 0.0;
    for (unsigned i = 0; i < 3U; ++i) {
      const double d = b.position[i] - a.position[i];
      dot += (position[i] - a.position[i] - (i == 1U ? 3.9 : 0.0)) * d;
      length += d * d;
    }
    const double t = std::clamp(dot / length, 0.0, 1.0);
    std::array<float, 3U> projected{};
    for (unsigned i = 0; i < 3U; ++i)
      projected[i] = static_cast<float>(a.position[i] +
                                        t * (b.position[i] - a.position[i]) +
                                        (i == 1U ? 3.9 : 0.0));
    if (DistanceSquared(projected, position) <= 4.0) return id;
  }
  return std::nullopt;
}
}  // namespace bf::runtime
