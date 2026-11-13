#include "features/battle/captain_assignments.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
constexpr std::uint32_t kMaximumFever =
    70'000U;  // Both installed FeverSpecCaptain sheets.
constexpr auto kApplicationWindow =
    std::chrono::seconds{10};  // Server transport policy.
constexpr auto kFeverDuration =
    std::chrono::milliseconds{20'000};  // ItemSpecCaptain104.
}  // namespace

std::uint32_t CaptainAssignments::BoostedWeapon(std::uint32_t base) noexcept {
  // 5608B0 / 561340: alternate forms use PE constant125B144=7222.
  return base == 7211U ? 7212U : base == 7213U ? 7214U : 7222U;
}

std::optional<CaptainAssignments::FeverProgress>
CaptainAssignments::GetFeverProgress(domain::PlayerId player) const {
  const auto found = roles_.find(player);
  if (stopped_ || found == roles_.end() || found->second.retired ||
      !found->second.temporary_weapon)
    return std::nullopt;
  const auto& role = found->second;
  const auto& fever = role.fever;
  return FeverProgress{role.assignment,
                       fever.charge,
                       fever.ready,
                       fever.requested.has_value(),
                       fever.until.has_value() && !fever.ended,
                       role.fever_life};
}

void CaptainAssignments::SyncFeverLife(Role& role,
                                       std::uint64_t life) noexcept {
  if (role.fever_life == life) return;
  role.fever_life = life;
  // 827F50 -> event51 -> 55F760 resets charge and readiness, not attribute11.
  // Pending transport authorization also belongs to the previous life.
  role.fever.charge = 0U;
  role.fever.ready = false;
  role.fever.requested.reset();
}

bool CaptainAssignments::CreditFever(domain::PlayerId player,
                                     FeverIncrement event,
                                     const CombatRelay& combat, Time now) {
  const auto found = roles_.find(player);
  const auto actor = combat.actors_.find(player);
  if (!initialized_ || stopped_ || found == roles_.end() ||
      actor == combat.actors_.end())
    return false;
  auto& role = found->second;
  auto& fever = role.fever;
  if (role.retired || !role.temporary_weapon || !role.expires ||
      now >= *role.expires || now < role.ready ||
      role.assignment != event.assignment || event.serial == 0U ||
      event.serial <= fever.serial ||
      event.life != actor->second.life_generation || actor->second.controller ||
      actor->second.life_state != CombatLifeState::kAlive ||
      actor->second.performance.deaths != role.nominated_deaths)
    return false;
  SyncFeverLife(role, actor->second.life_generation);
  fever.serial = event.serial;
  if (fever.ready || fever.requested || fever.until) return false;
  const auto sum = fever.charge + event.amount;
  // 91DB30 dispatches321 to562190, which adds then tests strictly greater.
  // CaptainModeMgr has no92 callback: 4AACB0 returns false and91E847 adds
  // again WITHOUT a readiness check. Thus even a zero next frame can be ready.
  fever.ready = sum > kMaximumFever;
  fever.charge = (fever.ready ? kMaximumFever : sum) +
                 static_cast<std::uint64_t>(event.amount);
  return true;
}

std::vector<CombatDelivery> CaptainAssignments::UpdateFever(
    domain::PlayerId player, Role& role, CombatRelay& combat, Time now) {
  auto& fever = role.fever;
  auto& actor = combat.actors_.at(player);
  SyncFeverLife(role, actor.life_generation);
  if (fever.requested &&
      (role.retired || now >= *fever.requested + kApplicationWindow ||
       actor.life_generation != fever.request_life)) {
    fever = Fever{.serial = fever.serial};
  }
  if (!fever.until) return {};
  if (!fever.ended && (role.retired || now >= *fever.until)) {
    fever.ended = true;
    fever.charge = 0U;
    fever.ready = false;
    actor.captain_blast.reset();
    if (!role.retired) {
      actor.participant.weapon_templates[4U] = role.temporary_weapon;
      actor.captain_weapon = role.temporary_weapon;
      actor.last_fire.reset();
    }
    const auto object = actor.participant.actor_object_id;
    const auto temporary = [object](const auto& entry) {
      return entry.second.owner_object_id == object &&
             entry.second.weapon_bar == 4U;
    };
    std::erase_if(combat.weapon_objects_, temporary);
    std::erase_if(combat.projectiles_, temporary);
  }
  if (!fever.ended || !fever.cleanup_reported) return {};
  protocol::PacketWriter body;
  body.WriteU32(actor.participant.actor_object_id);
  body.WriteU32(11U);
  // A reported end cannot remove a later activation. Wait for this receipt
  // before accepting credits for a new cycle; the wire has no activation ID.
  fever = Fever{.serial = fever.serial};
  return combat.AcceptedResult(player, {32812U, body.Release()}).deliveries;
}

CombatRelayResult CaptainAssignments::HandleFever(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  const auto type = reader.ReadU32();
  reader.RequireEnd();
  const bool request = packet.opcode == 707U;
  const bool apply = packet.opcode == 32894U;
  const bool cleanup = packet.opcode == 32812U;
  if ((request && (object != 0xFFFFFFFFU || type != 104U)) ||
      (apply && type != 104U) || (cleanup && type != 11U) ||
      (!request && !apply && !cleanup))
    return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end() || actor->second.controller)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  if (!request && object != actor->second.participant.actor_object_id) {
    if (cleanup && combat.host_ == player &&
        combat.object_owners_.contains(object))
      return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "captain fever belongs to another participant"};
  }
  const auto found = roles_.find(player);
  if (!initialized_ || stopped_)
    return {.reject_reason = CombatRejectReason::kInactiveMatchPhase};
  if (found == roles_.end() || !found->second.temporary_weapon)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  auto& role = found->second;
  auto& fever = role.fever;
  SyncFeverLife(role, actor->second.life_generation);
  if (cleanup) {
    if (!fever.until)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    if (fever.cleanup_reported)
      return {.reject_reason = CombatRejectReason::kDuplicateAction};
    fever.cleanup_reported = true;
    return {.accepted = true,
            .deliveries = UpdateFever(player, role, combat, now)};
  }
  if (role.retired || selected_[role.team - 1U] != player || !role.expires ||
      now >= *role.expires ||
      actor->second.life_state != CombatLifeState::kAlive ||
      actor->second.performance.deaths != role.nominated_deaths)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  if (request) {
    if (!fever.ready || fever.requested || fever.until)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    fever.ready = false;
    fever.requested = now;
    fever.request_life = actor->second.life_generation;
    protocol::PacketWriter body;
    body.WriteU32(actor->second.participant.seat_index);
    body.WriteU32(104U);
    body.WriteU32(0U);
    const protocol::Packet effect{715U, body.Release()};
    auto result = combat.AcceptedResult(player, effect);
    // 7D02E0 -> 840840: only the controlling client executes and reports32894.
    result.deliveries.push_back({player, {effect.opcode, effect.body}});
    return result;
  }
  if (!fever.requested || fever.until || now < *fever.requested ||
      now >= *fever.requested + kApplicationWindow ||
      actor->second.life_generation != fever.request_life)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  fever.requested.reset();
  fever.until = now + kFeverDuration;
  const auto weapon = BoostedWeapon(*role.temporary_weapon);
  actor->second.participant.weapon_templates[4U] = weapon;
  actor->second.captain_weapon = weapon;
  actor->second.captain_blast = {
      static_cast<std::uint16_t>(role.team == 1U ? 5211U : 5213U), *fever.until,
      false};
  actor->second.last_fire.reset();
  return combat.AcceptedResult(player, packet);
}
}  // namespace bf::runtime
