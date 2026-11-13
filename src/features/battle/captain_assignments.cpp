#include "features/battle/captain_assignments.h"

#include <algorithm>
#include <stdexcept>

#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
CaptainAssignments::CaptainAssignments(std::uint32_t seed) : random_(seed) {}

std::optional<domain::PlayerId> CaptainAssignments::Selected(
    std::uint8_t team) const noexcept {
  return team == 1U || team == 2U ? selected_[team - 1U] : std::nullopt;
}

void CaptainAssignments::Retire(domain::PlayerId player, Role& role,
                                CombatRelay& combat) {
  if (role.retired) return;
  role.retired = true;
  if (selected_[role.team - 1U] == player) selected_[role.team - 1U].reset();
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end() || !role.temporary_weapon) return;
  actor->second.participant.weapon_templates[4U] = role.previous_weapon;
  actor->second.captain_weapon.reset();
  actor->second.captain_blast.reset();
  actor->second.last_fire.reset();
  retired_weapons_[player] = {*role.temporary_weapon,
                              BoostedWeapon(*role.temporary_weapon)};
  const auto object = actor->second.participant.actor_object_id;
  const auto temporary = [object](const auto& entry) {
    return entry.second.owner_object_id == object &&
           entry.second.weapon_bar == 4U;
  };
  std::erase_if(combat.weapon_objects_, temporary);
  std::erase_if(combat.projectiles_, temporary);
}

std::vector<CombatDelivery> CaptainAssignments::ForwardCleanup(
    domain::PlayerId player, Role& role, CombatRelay& combat) {
  if (!role.retired || !role.attribute_reported || role.attribute_forwarded)
    return {};
  protocol::PacketWriter body;
  body.WriteU32(combat.actors_.at(player).participant.actor_object_id);
  body.WriteU32(29U);
  role.attribute_forwarded = true;
  return combat
      .AcceptedResult(player, {.opcode = 32812U, .body = body.Release()})
      .deliveries;
}

CaptainAssignments::Update CaptainAssignments::Tick(CombatRelay& combat,
                                                    Time now) {
  if (stopped_) return {};
  if (!initialized_) {
    std::array<unsigned, 2U> teams{};
    for (const auto& [id, actor] : combat.actors_) {
      static_cast<void>(id);
      const auto& member = actor.participant;
      if (actor.controller || member.seat_index >= 16U ||
          (member.team_code != 1U && member.team_code != 2U))
        throw std::invalid_argument{
            "captain assignments require native player teams"};
      ++teams[member.team_code - 1U];
    }
    if (teams[0] == 0U || teams[1] == 0U || combat.individual_scoring_)
      throw std::invalid_argument{"captain assignments require two teams"};
    initialized_ = true;
  }
  Update update;
  // Capture the transformed role before retirement or the two cleanup receipts
  // erase it.
  update.defeats = CollectDefeats(combat, now);
  for (auto it = roles_.begin(); it != roles_.end();) {
    const auto actor = combat.actors_.find(it->first);
    if (actor == combat.actors_.end()) {
      Retire(it->first, it->second, combat);
      retired_weapons_.erase(it->first);
      it = roles_.erase(it);
      continue;
    }
    auto& role = it->second;
    if (!role.retired &&
        (actor->second.life_state == CombatLifeState::kDefeated ||
         actor->second.performance.deaths != role.nominated_deaths ||
         (role.expires && now >= *role.expires))) {
      Retire(it->first, role, combat);
    }
    auto deliveries = UpdateFever(it->first, role, combat, now);
    update.deliveries.insert(update.deliveries.end(), deliveries.begin(),
                             deliveries.end());
    deliveries = ForwardCleanup(it->first, role, combat);
    update.deliveries.insert(update.deliveries.end(), deliveries.begin(),
                             deliveries.end());
    if (role.retired && (!role.temporary_weapon ||
                         (role.restore_reported && role.attribute_forwarded))) {
      it = roles_.erase(it);
      continue;
    }
    ++it;
  }
  std::erase_if(retired_weapons_, [&](const auto& entry) {
    return !combat.actors_.contains(entry.first);
  });
  for (std::uint8_t team = 1U; team <= 2U; ++team) {
    if (selected_[team - 1U]) continue;
    std::vector<domain::PlayerId> candidates;
    for (const auto& [id, actor] : combat.actors_) {
      if (!actor.controller && actor.participant.team_code == team &&
          actor.life_state == CombatLifeState::kAlive && !roles_.contains(id))
        candidates.push_back(id);
    }
    if (candidates.empty()) continue;
    const auto index = std::uniform_int_distribution<std::size_t>{
        0U, candidates.size() - 1U}(random_);
    const auto player = candidates[index];
    const auto& actor = combat.actors_.at(player);
    roles_.emplace(
        player, Role{.team = team,
                     .nominated_deaths = actor.performance.deaths,
                     .ready = now + std::chrono::seconds{2},
                     .previous_weapon = actor.participant.weapon_templates[4U],
                     .assignment = ++next_assignment_,
                     .fever_life = actor.life_generation});
    selected_[team - 1U] = player;
    update.broadcasts.push_back({988U, protocol::EncodeCaptainSkillResponse(
                                           actor.participant.seat_index)});
  }
  return update;
}

CombatRelayResult CaptainAssignments::Handle(domain::PlayerId player,
                                             const protocol::Packet& packet,
                                             CombatRelay& combat, Time now) {
  if (packet.opcode == 710U) return HandleDefeat(player, packet, combat, now);
  if (packet.opcode == 707U) return HandleFever(player, packet, combat, now);
  if (packet.opcode == 32894U || packet.opcode == 32812U) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    const auto type = reader.ReadU32();
    reader.RequireEnd();
    if ((packet.opcode == 32894U && type == 104U) ||
        (packet.opcode == 32812U && type == 11U))
      return HandleFever(player, packet, combat, now);
  }
  // Decode before checking phase so malformed late packets still fail
  // structurally.
  std::optional<protocol::CaptainTransformRequest> transformation;
  std::optional<std::uint32_t> restore_seat;
  std::optional<std::uint32_t> cleanup_object;
  if (packet.opcode == 32894U)
    transformation = protocol::DecodeCaptainTransformRequest(packet.body);
  else if (packet.opcode == 2021U)
    restore_seat = protocol::DecodeCaptainRestoreRequest(packet.body);
  else if (packet.opcode == 32812U) {
    const auto action =
        protocol::DecodeNativeAction(packet.opcode, packet.body);
    protocol::PacketReader reader{packet.body};
    cleanup_object = action.actor_object_id;
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() != 29U)
      return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
  } else
    throw std::invalid_argument{"unsupported captain assignment packet"};
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end() || actor->second.controller)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  const auto& member = actor->second.participant;
  if (cleanup_object && *cleanup_object != member.actor_object_id &&
      combat.host_ == player && combat.object_owners_.contains(*cleanup_object))
    return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
  if ((transformation && transformation->object != member.actor_object_id) ||
      (cleanup_object && *cleanup_object != member.actor_object_id) ||
      (restore_seat && *restore_seat != member.seat_index))
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "captain request belongs to another participant"};
  if (!initialized_ || stopped_)
    return {.reject_reason = CombatRejectReason::kInactiveMatchPhase};
  const auto found = roles_.find(player);
  if (found == roles_.end())
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  auto& role = found->second;
  if (cleanup_object) {
    if (!role.temporary_weapon)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    if (role.attribute_reported)
      return {.reject_reason = CombatRejectReason::kDuplicateAction};
    role.attribute_reported = true;
    auto deliveries = ForwardCleanup(player, role, combat);
    if (role.restore_reported && role.attribute_forwarded) roles_.erase(found);
    return {.accepted = true, .deliveries = std::move(deliveries)};
  }
  if (restore_seat) {
    if (!role.temporary_weapon)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    if (role.restore_reported)
      return {.reject_reason = CombatRejectReason::kDuplicateAction};
    role.restore_reported = true;
    if (role.retired && role.attribute_forwarded) roles_.erase(found);
    // Receipt and victim-confirmed defeat can arrive in either order. An early
    // receipt is retained but cannot retire a living captain, grant points or
    // nominate.
    return {.accepted = true};
  }
  if (role.retired || selected_[role.team - 1U] != player ||
      actor->second.life_state != CombatLifeState::kAlive ||
      transformation->form.team != role.team || now < role.ready)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  if (role.temporary_weapon)
    return {.reject_reason = CombatRejectReason::kDuplicateAction};
  if (role.nominated_deaths != actor->second.performance.deaths)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  role.temporary_weapon = transformation->form.weapon;
  role.applied = now;
  role.expires = now + std::chrono::milliseconds{3'600'000};
  defeat_claims_.erase(player);
  actor->second.participant.weapon_templates[4U] = transformation->form.weapon;
  actor->second.captain_weapon = transformation->form.weapon;
  actor->second.last_fire.reset();
  retired_weapons_.erase(player);
  // 840790 executes the local prediction; only peers need the accepted item
  // application.
  return combat.AcceptedResult(player, packet);
}

bool CaptainAssignments::PrepareAction(domain::PlayerId player,
                                       const protocol::Packet& packet,
                                       const CombatRelay& combat) const {
  if (!protocol::IsNativeActionOpcode(packet.opcode)) return true;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end() ||
      request.actor_object_id != actor->second.participant.actor_object_id ||
      !request.weapon_switch || request.weapon_switch->bar_index != 4U)
    return true;
  const auto requested = request.weapon_switch->template_id;
  const auto expected = actor->second.participant.weapon_templates[4U];
  if (requested == expected) return true;
  const auto role = roles_.find(player);
  if (role != roles_.end()) {
    if (requested == role->second.previous_weapon) return false;
    if (role->second.temporary_weapon &&
        (requested == role->second.temporary_weapon ||
         requested == BoostedWeapon(*role->second.temporary_weapon)))
      return false;  // A late start/end switch cannot revive an inactive fever
                     // phase.
  }
  const auto retired = retired_weapons_.find(player);
  return retired == retired_weapons_.end() ||
         !retired->second.contains(requested);
}

void CaptainAssignments::Stop(CombatRelay& combat) {
  for (auto& [player, role] : roles_) Retire(player, role, combat);
  roles_.clear();
  defeat_claims_.clear();
  selected_ = {};
  stopped_ = true;
}
}  // namespace bf::runtime
