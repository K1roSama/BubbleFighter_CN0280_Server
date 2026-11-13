#include "features/battle/combat_relay.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
using Time = std::chrono::steady_clock::time_point;
constexpr auto kApplicationWindow =
    std::chrono::seconds{10};  // Server transport policy.
constexpr auto kExpiryAllowance =
    std::chrono::milliseconds{250};  // Clock/transport tolerance.
}  // namespace

void CombatRelay::ClearDeathmatchFever(ActorState& actor,
                                       CombatRelayResult& result) {
  if (mode_id_ != 3U && mode_id_ != 13U && mode_id_ != 14U && mode_id_ != 18U)
    return;
  auto& fever = actor.deathmatch_fever;
  fever.requested.reset();
  fever.item_applied = false;
  fever.completed_life.reset();
  if (!fever.active_until) return;
  fever.active_until.reset();
  // Native bubble/death entry8B9900/8B6860 calls A1A580; its registered
  // callback83B2C0 sends32812 through7CA240. Confirming the transition here
  // also covers a client cleanup that arrived before its lifecycle packet.
  protocol::PacketWriter writer;
  writer.WriteU32(actor.participant.actor_object_id);
  writer.WriteU32(11U);
  protocol::Packet packet{.opcode = 32812U, .body = writer.Release()};
  auto cleanup = AcceptedResult(actor.participant.player_id, packet);
  cleanup.deliveries.push_back(
      {actor.participant.player_id, {packet.opcode, packet.body}});
  result.deliveries.insert(result.deliveries.begin(),
                           cleanup.deliveries.begin(),
                           cleanup.deliveries.end());
}

bool CombatRelay::DeathmatchFeverItem(domain::PlayerId player, bool apply,
                                      Time now) {
  if (mode_id_ != 3U && mode_id_ != 13U && mode_id_ != 14U && mode_id_ != 18U)
    return false;
  const auto found = actors_.find(player);
  if (found == actors_.end() || found->second.controller) return false;
  auto& actor = found->second;
  auto& fever = actor.deathmatch_fever;
  // Native32926 is predicted through660710, whereas707 is a direct TCP send.
  // Accept the two arrival orders only after this life received real707 respawn
  // authorization. The prediction cannot mint another confirmed respawn count.
  const bool completing = actor.life_state == CombatLifeState::kDefeated &&
                          actor.death_stage == 2U &&
                          actor.respawn_point.has_value();
  if (actor.life_state != CombatLifeState::kAlive && !completing) return false;
  // Bubble recovery also changes life_generation; it is not a respawn and
  // cannot renew a qualified character's entitlement.
  if (!completing && fever.completed_life != actor.life_generation)
    return false;
  if (apply) {
    if (fever.claimed_life != actor.life_generation || !fever.requested ||
        fever.item_applied || now < *fever.requested ||
        now >= *fever.requested + kApplicationWindow)
      return false;
    fever.item_applied = true;
    return true;
  }
  // 811440 starts the counter at zero;82C8F0 increments on respawn;828580
  // resets the killer on a confirmed pang (a bubble alone does not reset it).
  const auto count =
      static_cast<std::uint64_t>(fever.respawns_without_pang) + completing;
  const auto threshold = (mode_id_ == 3U || mode_id_ == 13U)
                             ? 3U
                             : 4U;  // GameOption NoPangDeadCount.
  if (count < threshold || fever.claimed_life == actor.life_generation ||
      (fever.active_until && now < *fever.active_until))
    return false;
  fever.claimed_life = actor.life_generation;
  fever.requested = now;
  fever.item_applied = false;
  fever.active_until.reset();
  return true;
}

CombatRelayResult CombatRelay::DeathmatchFeverAttribute(
    ActorState& actor, const protocol::Packet& packet, Time now) {
  auto& fever = actor.deathmatch_fever;
  protocol::PacketReader reader{packet.body};
  static_cast<void>(
      reader.ReadU32());  // Ownership was checked by handle_action.
  const auto attribute = reader.ReadU32();
  if (actor.controller || attribute != 11U)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (packet.opcode == 32812U) {
    reader.RequireEnd();
    if (!fever.active_until || (fever.active_life == actor.life_generation &&
                                now + kExpiryAllowance < *fever.active_until))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    fever.active_until.reset();
    auto result = AcceptedResult(actor.participant.player_id, packet);
    result.deliveries.push_back(
        {actor.participant.player_id, {packet.opcode, packet.body}});
    return result;
  }
  const auto type = reader.ReadU32();
  const auto duration = reader.ReadU32();
  reader.RequireEnd();
  // 93BFD0 applies5DE1C0: ItemSpecSinglePvp/TeamSurvival104's20000ms
  // multiplied by0.8/0.7. Neither profile installs a captain weapon.
  // Mode 3 uses ItemSpecItem104's unscaled 13000ms (5DE1C0 returns disabled).
  const auto native_duration = mode_id_ == 3U    ? 13'000U
                               : mode_id_ == 13U ? 16'000U
                                                 : 14'000U;
  const bool completing = actor.life_state == CombatLifeState::kDefeated &&
                          actor.death_stage == 2U &&
                          actor.respawn_point.has_value();
  if (type != 104U || duration != native_duration || !fever.item_applied ||
      !fever.requested || fever.claimed_life != actor.life_generation ||
      now < *fever.requested || now >= *fever.requested + kApplicationWindow ||
      fever.active_until ||
      (actor.life_state != CombatLifeState::kAlive && !completing))
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  fever.requested.reset();
  fever.active_life = actor.life_generation;
  fever.active_until = now + std::chrono::milliseconds{duration};
  return AcceptedResult(actor.participant.player_id, packet);
}
}  // namespace bf::runtime
