#include "features/battle/combat_relay.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
CombatRelayResult CombatRelay::HandleSurvivalWater(
    ActorState& actor, const protocol::Packet& packet,
    const protocol::NativeWeaponObjectRequest& request,
    std::chrono::steady_clock::time_point now) {
  if ((mode_id_ != 23U && mode_id_ != 25U) || actor.controller ||
      actor.life_state != CombatLifeState::kAlive ||
      request.actor_object_id != actor.participant.actor_object_id ||
      request.attack_variant != actor.participant.team_code)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  // 581010 -> 7C8180/7EE3F0: local position, zero vector, fixed (0,1,0).
  // The client checks submerged depth; this report can damage only its owner.
  protocol::PacketReader vectors{std::span{packet.body}.subspan(19U, 24U)};
  for (unsigned index = 0U; index < 6U; ++index)
    if (vectors.ReadF32() != (index == 4U ? 1.0F : 0.0F))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (ContainsDamagerCookie(request.cookie))
    return RejectedResult(CombatRejectReason::kDuplicateProjectile);
  // GameOption area32WaterDamageCoolTime = 2000ms; 100ms transport tolerance.
  if (actor.survival_water_report &&
      now - *actor.survival_water_report < std::chrono::milliseconds{1900})
    return RejectedResult(CombatRejectReason::kRateLimited);
  if (weapon_objects_.size() + storms_.size() >= 8192U)
    return RejectedResult(CombatRejectReason::kProjectileLimit);
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  actor.survival_water_report = now;
  weapon_objects_.emplace(
      request.cookie,
      ProjectileState{.owner_object_id = actor.participant.actor_object_id,
                      .created_at = now,
                      .self_only = true});
  auto result = AcceptedResult(actor.participant.player_id, packet);
  // The native sender queues the object; the owner needs its damager too.
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = packet.opcode, .body = packet.body}});
  return result;
}
}  // namespace bf::runtime
