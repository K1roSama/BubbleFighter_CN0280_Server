#include <algorithm>

#include "features/battle/combat_relay.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
// Native 8AFDA0 uses (dx*dx + dy*dy) + dz*dz <= 2025, inclusive.
bool WithinMeleeRange(const std::array<float, 3U>& source,
                      const std::optional<std::array<float, 3U>>& target) {
  // Spawn has no mandatory movement sample. The target's native 8AFDA0 still
  // checks its actual position before confirming; absence is not a zero vector.
  if (!target) return true;
  const float dx = source[0] - (*target)[0];
  const float dy = source[1] - (*target)[1];
  const float dz = source[2] - (*target)[2];
  return (dx * dx + dy * dy) + dz * dz <= 2025.0F;
}
}  // namespace

CombatRelayResult CombatRelay::HandleSurvivalMelee(
    ActorState& actor, const protocol::Packet& packet,
    const protocol::NativeSurvivalMelee& request,
    std::chrono::steady_clock::time_point now) {
  using namespace std::chrono_literals;
  if (mode_id_ != 23U && mode_id_ != 25U)
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  if (!object_owners_.contains(request.attacker) ||
      !object_owners_.contains(request.victim))
    return RejectedResult(CombatRejectReason::kUnknownTarget);
  auto& source = actors_.at(object_owners_.at(request.attacker));
  auto& target = actors_.at(object_owners_.at(request.victim));
  const auto invalid = [] {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  };
  if (&source == &target || source.controller || target.controller ||
      source.soccer_ball || target.soccer_ball ||
      source.life_state != CombatLifeState::kAlive ||
      (request.confirmation &&
       target.life_state == CombatLifeState::kDefeated) ||
      (mode_id_ == 25U &&
       source.participant.team_code == target.participant.team_code))
    return invalid();
  if (!request.confirmation) {
    if (source.survival_alliance_pose || source.survival_treatment_start)
      return invalid();
    // 8AFD20 sets waiting flag +11 before sending. Only the 33276 echo clears
    // it in 8CB620. A known target that moved out of range or died must still
    // let the source enter 122 and take its native no-confirmation fallback.
    if (source.survival_melee && !source.survival_melee->ended &&
        source.survival_melee->source_life == source.life_generation &&
        now <= source.survival_melee->started + 5s)
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    source.survival_melee =
        ActorState::SurvivalMelee{.victim = request.victim,
                                  .source_life = source.life_generation,
                                  .victim_life = target.life_generation,
                                  .source_position = request.vector,
                                  .started = now};
  } else {
    if (!source.survival_melee) return invalid();
    auto& pending = *source.survival_melee;
    if (pending.confirmed || pending.ended)
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    if (pending.victim != request.victim ||
        pending.source_life != source.life_generation ||
        pending.victim_life != target.life_generation ||
        now < pending.started || now > pending.started + 5s ||
        !WithinMeleeRange(pending.source_position, target.position))
      return invalid();
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    pending.confirmed = true;
    pending.confirmed_at = now;
    target.life_state = CombatLifeState::kDefeated;
    ++target.life_generation;
    target.defeated =
        DefeatEvent{request.attacker, target.life_generation, now};
    target.life_settled = false;
    target.death_stage = 0U;
    target.respawn_point.reset();
    target.pending_kick.reset();
    target.bubble_impacts.clear();
    target.active_kick.reset();
    target.survival_melee.reset();
    rescues_.erase(request.victim);
    RecordLifecycleStatistics(target,
                              {.kind = protocol::NativeLifecycleKind::kDefeat,
                               .actor_object_id = request.victim,
                               .related_object_id = request.attacker,
                               .direct_defeat = true});
  }
  auto result = AcceptedResult(actor.participant.player_id, packet);
  // Both senders only queue a request; 7DD750/7DD850 apply state 122/406.
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = packet.opcode, .body = packet.body}});
  if (request.confirmation) {
    CancelSurvivalTreatment(target, result, true);
    if (target.survival_alliance_pose) {
      protocol::PacketWriter writer;
      writer.WriteU32(request.victim);
      protocol::Packet cancel{.opcode = 33274U, .body = writer.Release()};
      auto cleanup = AcceptedResult(target.participant.player_id, cancel);
      cleanup.deliveries.push_back(
          {.recipient = target.participant.player_id,
           .message = {.opcode = cancel.opcode, .body = cancel.body}});
      result.deliveries.insert(result.deliveries.begin(),
                               cleanup.deliveries.begin(),
                               cleanup.deliveries.end());
      target.survival_alliance_pose = false;
    }
  }
  return result;
}

CombatRelayResult CombatRelay::HandleSurvivalMeleeEnd(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  using namespace std::chrono_literals;
  auto& pending = *actor.survival_melee;
  if (pending.ended)
    return RejectedResult(CombatRejectReason::kDuplicateAction);
  // Native phases: 100 ms + confirmation (1000 ms fallback) + 1 ms + 1600 ms.
  // Receive timestamps are not synchronized client clocks: permit 250 ms
  // jitter.
  const auto earliest =
      pending.confirmed
          ? std::max(pending.started + 100ms, pending.confirmed_at) + 1350ms
          : pending.started + 2350ms;
  if (actor.life_state != CombatLifeState::kAlive ||
      pending.source_life != actor.life_generation || now < earliest)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  pending.ended = true;
  auto result = AcceptedResult(actor.participant.player_id, packet);
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = packet.opcode, .body = packet.body}});
  return result;
}
}  // namespace bf::runtime
