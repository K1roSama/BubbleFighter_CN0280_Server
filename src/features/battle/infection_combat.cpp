#include <algorithm>

#include "features/battle/infection_match.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
// Correlation allowance, not a recovered native attack duration.
constexpr auto kImpactWindow = std::chrono::seconds{5};
constexpr std::size_t kMaximumDamagers = 8192U;
}  // namespace

void InfectionMatch::Observe(domain::PlayerId player,
                             const protocol::Packet& packet,
                             const CombatRelay& combat, Time now) {
  std::erase_if(melee_, [&](const auto& entry) {
    const auto weapon = combat.weapon_objects_.find(entry.first);
    return now < entry.second.created ||
           now - entry.second.created > kImpactWindow ||
           weapon == combat.weapon_objects_.end() ||
           weapon->second.created_at != entry.second.created;
  });
  std::erase_if(blasts_, [&](const auto& permit) {
    const auto mother = combat.actors_.find(mother_);
    const auto target = combat.actors_.find(permit.target);
    return now < permit.created || now - permit.created > kImpactWindow ||
           mother == combat.actors_.end() || target == combat.actors_.end() ||
           mother->second.life_generation != permit.mother_life ||
           target->second.life_generation != permit.target_life;
  });
  if (started_ && !winner_ && infected_.contains(player) &&
      packet.opcode == 33282U) {
    const auto hit =
        protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
    const auto weapon = combat.weapon_objects_.find(hit.cookie);
    const auto source = combat.actors_.find(player);
    const auto owner = combat.object_owners_.find(hit.target_object_id);
    if (source != combat.actors_.end() &&
        source->second.life_state == CombatLifeState::kAlive &&
        weapon != combat.weapon_objects_.end() &&
        weapon->second.owner_object_id ==
            source->second.participant.actor_object_id &&
        weapon->second.targets.contains(hit.target_object_id) &&
        owner != combat.object_owners_.end() && owner->second != mother_ &&
        owner->second != player && infected_.contains(owner->second)) {
      const auto pending = respawns_.find(owner->second);
      const auto& target = combat.actors_.at(owner->second);
      if (pending != respawns_.end() &&
          target.life_state == CombatLifeState::kDefeated &&
          target.death_stage == 2U &&
          pending->second.generation == target.life_generation)
        // A7DD40 changes a dead child's local timer using untransmitted damage.
        // Preserve the fallback deadline; allow only this life to claim expiry.
        pending->second.native_damage_seen = true;
    }
  }
  if (!started_ || winner_ || player != mother_ ||
      (packet.opcode != 33281U && packet.opcode != 33282U))
    return;
  const auto mother = combat.actors_.find(mother_);
  if (mother == combat.actors_.end() ||
      mother->second.life_state != CombatLifeState::kAlive)
    return;
  const auto request =
      protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
  const auto weapon = combat.weapon_objects_.find(request.cookie);
  if (weapon == combat.weapon_objects_.end() ||
      weapon->second.owner_object_id !=
          mother->second.participant.actor_object_id)
    return;
  if (packet.opcode == 33281U) {
    if (request.template_id == 7200U || request.template_id == 7220U ||
        (FeverActive() &&
         (request.template_id == 7202U || request.template_id == 7205U ||
          request.template_id == 7207U || request.template_id == 7222U)))
      melee_[request.cookie] = {weapon->second.created_at,
                                mother->second.life_generation};
    return;
  }
  const auto source = melee_.find(request.cookie);
  const auto owner = combat.object_owners_.find(request.target_object_id);
  if (source == melee_.end() ||
      source->second.life != mother->second.life_generation ||
      owner == combat.object_owners_.end() || owner->second == mother_ ||
      !weapon->second.targets.contains(request.target_object_id))
    return;
  const auto& target = combat.actors_.at(owner->second);
  // A7DD40 creates5200 at the hit character's position after a mother claw hit.
  // The caller invokes this only after CombatRelay consumes the impact cookie.
  if (blasts_.size() < kMaximumDamagers)
    blasts_.push_back(
        {now, owner->second, target.life_generation,
         mother->second.life_generation,
         static_cast<std::uint16_t>(FeverActive() ? 5201U : 5200U)});
}

application::ServiceResult<CombatRelayResult> InfectionMatch::HandleBlast(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  const auto request =
      protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
  if (request.kind != protocol::NativeWeaponObjectKind::kBlast ||
      (request.template_id != 5200U && request.template_id != 5201U))
    return combat.Handle(player, packet, now);
  const auto reject = [](CombatRejectReason reason) {
    return CombatRelayResult{.reject_reason = reason};
  };
  const auto found = combat.actors_.find(player);
  if (found == combat.actors_.end())
    return reject(CombatRejectReason::kUnknownRelatedActor);
  auto& actor = found->second;
  if (request.actor_object_id != actor.participant.actor_object_id)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "infection blast belongs to another participant"};
  if (!started_ || winner_ || player != mother_ ||
      actor.life_state != CombatLifeState::kAlive)
    return reject(CombatRejectReason::kInvalidLifecycleState);
  if (request.attack_variant != 34)
    return reject(CombatRejectReason::kModeWeaponRestricted);
  protocol::PacketReader vectors{std::span{packet.body}.subspan(19U, 24U)};
  // 7C8180 ->7EE3F0: one BYTE team, zero velocity and the native up vector.
  for (unsigned axis = 0; axis < 6; ++axis) {
    if (vectors.ReadF32() != (axis == 4U ? 1.0F : 0.0F))
      return reject(CombatRejectReason::kModeWeaponRestricted);
  }
  combat.PruneProjectiles(now);
  if (combat.ContainsDamagerCookie(request.cookie))
    return reject(CombatRejectReason::kDuplicateProjectile);
  std::erase_if(blasts_, [&](const auto& permit) {
    const auto target = combat.actors_.find(permit.target);
    return now < permit.created || now - permit.created > kImpactWindow ||
           permit.mother_life != actor.life_generation ||
           target == combat.actors_.end() ||
           target->second.life_generation != permit.target_life;
  });
  const bool transform = request.template_id == 5201U && initial_blast_pending_;
  const bool fever_blast = request.template_id == 5201U && FeverActive() &&
                           fever_->blast_pending && now < fever_->until &&
                           fever_->life == actor.life_generation;
  auto permit = std::ranges::find_if(blasts_, [&](const auto& candidate) {
    return candidate.type == request.template_id;
  });
  if (!transform && !fever_blast && permit == blasts_.end())
    return reject(CombatRejectReason::kInvalidLifecycleState);
  if (combat.weapon_objects_.size() + combat.storms_.size() >= kMaximumDamagers)
    return reject(CombatRejectReason::kProjectileLimit);
  if (!combat.ConsumeActionQuota(actor, now))
    return reject(CombatRejectReason::kRateLimited);
  // Use the same cookie namespace and impact/deletion path as native GoDamager.
  auto& weapon = combat.weapon_objects_[request.cookie];
  weapon.owner_object_id = request.actor_object_id;
  weapon.created_at = now;
  weapon.weapon_bar = 2U;
  if (transform)
    initial_blast_pending_ = false;
  else if (fever_blast)
    fever_->blast_pending = false;
  else
    blasts_.erase(permit);
  return combat.AcceptedResult(player, packet);
}
}  // namespace bf::runtime
