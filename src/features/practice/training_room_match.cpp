#include "features/practice/training_room_match.h"

#include <algorithm>
#include <stdexcept>

#include "features/practice/training_dummy_data.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
TrainingRoomMatch::TrainingRoomMatch(domain::PlayerId owner,
                                     std::uint32_t actor, CombatRelay& combat)
    : owner_(owner), actor_(actor) {
  const auto own = combat.actors_.find(owner);
  if (combat.mode_id_ != kTrainingMode || combat.actors_.size() != 1 ||
      own == combat.actors_.end() || own->second.controller ||
      own->second.participant.actor_object_id != actor || actor != 1000000U)
    throw std::invalid_argument{
        "training requires its authenticated sole seat"};
  for (std::size_t index = 0; index < kTrainingDummies.size(); ++index)
    Bind(index, combat);
}

void TrainingRoomMatch::Bind(std::size_t index, CombatRelay& combat,
                             bool force_bubble) {
  const auto object = actor_ + 1U + static_cast<std::uint32_t>(index);
  const domain::PlayerId synthetic{(std::uint64_t{1} << 63U) | object};
  const auto& config = kTrainingDummies.at(index);
  const auto prior = combat.actors_.find(synthetic);
  const auto generation =
      prior == combat.actors_.end() ? 0U : prior->second.life_generation + 1U;
  CombatRelay::ActorState state;
  state.participant = {.player_id = synthetic,
                       .actor_object_id = object,
                       .team_code = config.team,
                       .weapon_templates = {},
                       .ordinary_respawn_allowed = false,
                       .fighter = 105U};
  state.controller = owner_;
  state.training_dummy = true;
  state.position = config.position;
  state.life_state = (config.locked || force_bubble) ? CombatLifeState::kBubbled
                                                     : CombatLifeState::kAlive;
  state.life_settled = config.locked || force_bubble;
  state.life_generation = generation;
  combat.rescues_.erase(object);
  combat.actors_.insert_or_assign(synthetic, std::move(state));
  combat.object_ids_.insert(object);
  combat.object_owners_.insert_or_assign(object, synthetic);
}

CombatRelayResult TrainingRoomMatch::Reset(domain::PlayerId sender,
                                           std::uint32_t object,
                                           CombatRelay& combat) {
  if (sender != owner_ || combat.mode_id_ != kTrainingMode ||
      object <= actor_ || object > actor_ + kTrainingDummies.size())
    return {.accepted = false,
            .reject_reason = CombatRejectReason::kUnknownTarget,
            .deliveries = {}};
  // Reference4A73A0 sends38281 after respawn for every training dummy,
  // including the initially unlocked target. Its response always enters301.
  Bind(object - actor_ - 1, combat, true);
  protocol::PacketWriter response;
  response.WriteU32(object);
  return {.accepted = true,
          .reject_reason = std::nullopt,
          .deliveries = {{.recipient = owner_,
                          .message = {.opcode = kTrainingResetOpcode,
                                      .body = response.Release()}}}};
}

CombatRelayResult TrainingRoomMatch::Equip(domain::PlayerId sender,
                                           std::uint32_t slot,
                                           std::uint32_t item,
                                           CombatRelay& combat) {
  const auto own = combat.actors_.find(owner_);
  if (sender != owner_ || combat.mode_id_ != kTrainingMode ||
      own == combat.actors_.end() || own->second.controller ||
      own->second.participant.actor_object_id != actor_)
    return {.accepted = false,
            .reject_reason = CombatRejectReason::kUnknownTarget,
            .deliveries = {}};
  const bool restore = slot == 0U && item == 0U;
  if (!restore &&
      !std::ranges::any_of(kTrainingEquipment, [&](const auto& option) {
        return option.slot == slot && option.item == item;
      }))
    return {.accepted = false,
            .reject_reason = CombatRejectReason::kModeWeaponRestricted,
            .deliveries = {}};
  auto& actor = own->second;
  // Accessories are applied by the native costume/buff system. Only the
  // selected grenade changes this match's authenticated weapon permission.
  // Keep existing projectile cookies: an already thrown grenade may land after
  // the player changes equipment. No inventory or saved preset is touched.
  if (restore)
    actor.participant.weapon_templates = actor.initial_weapons;
  else if (slot == 21U)
    actor.participant.weapon_templates[3] = item - 200000U;
  protocol::PacketWriter response;
  response.WriteU32(slot);
  response.WriteU32(item);
  return {.accepted = true,
          .reject_reason = std::nullopt,
          .deliveries = {{.recipient = owner_,
                          .message = {.opcode = kTrainingEquipmentOpcode,
                                      .body = response.Release()}}}};
}
}  // namespace bf::runtime
