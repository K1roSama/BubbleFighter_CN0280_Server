#include "features/battle/combat_relay.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {

bool CombatRelay::CanEquip(const ActorState& actor, std::uint32_t bar,
                           std::uint32_t weapon) const noexcept {
  if (EquippedTemplate(actor, bar) == weapon) return true;
  if ((mode_id_ != 23U && mode_id_ != 25U) || actor.controller ||
      actor.life_state != CombatLifeState::kAlive)
    return false;
  if (bar == 0U) return actor.survival_primary_weapons.contains(weapon);
  if (bar != 3U) return false;
  return actor.initial_weapons[3U] == weapon ||
         (weapon == 5921U && actor.survival_supplies.contains(560U) &&
          actor.survival_supplies.at(560U) > 0U);
}

CombatRelayResult CombatRelay::HandleSurvivalTreatment(
    ActorState& actor, const protocol::Packet& packet,
    const protocol::NativeSurvivalTreatment& request,
    std::chrono::steady_clock::time_point now) {
  using namespace std::chrono_literals;
  if (mode_id_ != 23U && mode_id_ != 25U)
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  if (actor.controller || actor.life_state != CombatLifeState::kAlive)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  const auto invalid = [] {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  };
  const auto duplicate = [] {
    return RejectedResult(CombatRejectReason::kDuplicateAction);
  };
  const bool has_medicine = EquippedTemplate(actor, 4U) == 5920U &&
                            actor.selected_weapon_bar == 4U &&
                            actor.survival_medkits != 0U;
  switch (packet.opcode) {
    case 33270U:
      if (actor.survival_treatment_start) return duplicate();
      if (!has_medicine) return invalid();
      break;
    case 33271U:
      if (!actor.survival_treatment_start) return duplicate();
      break;
    case 33272U:
      if (request.stage == 0U) {
        if (actor.survival_used_effects.contains(request.effect_id))
          return duplicate();
        if (!has_medicine || !actor.survival_treatment_start ||
            actor.survival_heals.size() >= 256U ||
            now < *actor.survival_treatment_start)
          return invalid();
        // 57ECA0's hold timer is 2000 - a secured character attribute. Until
        // that attribute is frozen here, do not reject legitimate accelerated
        // medicine. A real initial or collected kit is required and consumed
        // exactly once.
      } else {
        if (!actor.survival_heals.contains(request.effect_id)) return invalid();
        const auto& heal = actor.survival_heals.at(request.effect_id);
        if (request.stage <= heal.stage) return duplicate();
        // 531E00 catches up one stage per frame: compare against effect
        // creation, not the preceding packet. 250 ms accommodates receive-time
        // jitter.
        if (request.stage != heal.stage + 1U ||
            now + 250ms < heal.created + std::chrono::seconds{request.stage})
          return invalid();
      }
      break;
    case 33269U:
      for (const auto id : request.expired_effects) {
        if (!actor.survival_heals.contains(id) ||
            actor.survival_heals.at(id).stage != 5U ||
            now + 250ms < actor.survival_heals.at(id).created + 6s)
          return invalid();
      }
      break;
    default:
      return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  }
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  switch (packet.opcode) {
    case 33270U:
      actor.survival_treatment_start = now;
      break;
    case 33271U:
      actor.survival_treatment_start.reset();
      break;
    case 33272U:
      if (request.stage == 0U) {
        actor.survival_used_effects.insert(request.effect_id);
        --actor.survival_medkits;
        actor.survival_treatment_start.reset();
        actor.survival_heals.emplace(
            request.effect_id,
            ActorState::SurvivalHeal{.id = request.effect_id, .created = now});
      } else {
        actor.survival_heals.at(request.effect_id).stage = request.stage;
      }
      break;
    case 33269U:
      for (const auto id : request.expired_effects)
        actor.survival_heals.erase(id);
      break;
    default:
      break;
  }
  auto result = AcceptedResult(actor.participant.player_id, packet);
  // 530070/531E80 execute the native, capped 36/18/9/6/3 HP increments on
  // every client, including the owner. No fabricated heal amount/HP is sent.
  result.deliveries.push_back(
      {.recipient = actor.participant.player_id,
       .message = {.opcode = packet.opcode, .body = packet.body}});
  return result;
}

void CombatRelay::CancelSurvivalTreatment(ActorState& actor,
                                          CombatRelayResult& result,
                                          bool clear_effect) {
  auto cancel = [&](std::uint16_t opcode, std::vector<std::uint8_t> body) {
    protocol::Packet message{.opcode = opcode, .body = std::move(body)};
    auto cleanup = AcceptedResult(actor.participant.player_id, message);
    cleanup.deliveries.push_back(
        {.recipient = actor.participant.player_id,
         .message = {.opcode = opcode, .body = message.body}});
    result.deliveries.insert(result.deliveries.begin(),
                             cleanup.deliveries.begin(),
                             cleanup.deliveries.end());
  };
  if (actor.survival_treatment_start) {
    protocol::PacketWriter writer;
    writer.WriteU32(actor.participant.actor_object_id);
    cancel(33271U, writer.Release());
    actor.survival_treatment_start.reset();
  }
  if (clear_effect && !actor.survival_heals.empty()) {
    protocol::PacketWriter writer;
    writer.WriteU32(actor.participant.actor_object_id);
    writer.WriteU32(static_cast<std::uint32_t>(actor.survival_heals.size()));
    for (const auto& [id, heal] : actor.survival_heals) {
      static_cast<void>(heal);
      writer.WriteU16(id);
    }
    cancel(33269U, writer.Release());
    actor.survival_heals.clear();
  }
}

}  // namespace bf::runtime
