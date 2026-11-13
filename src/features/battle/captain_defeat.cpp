#include <algorithm>

#include "features/battle/captain_assignments.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
// Server policy for cross-connection reordering, not a recovered native timer.
constexpr auto kReportWindow = std::chrono::seconds{5};
}  // namespace

void CaptainAssignments::SyncDefeats(const CombatRelay& combat, Time now) {
  for (const auto& [player, role] : roles_) {
    const auto actor = combat.actors_.find(player);
    if (role.retired || !role.applied || !role.temporary_weapon ||
        actor == combat.actors_.end())
      continue;
    const auto& victim = actor->second;
    const auto& death = victim.defeated;
    if (!death || victim.performance.deaths != role.nominated_deaths + 1U ||
        death->time < *role.applied || death->time >= *role.expires ||
        now < death->time || now - death->time > kReportWindow)
      continue;
    const auto owner = combat.object_owners_.find(death->attacker);
    if (owner == combat.object_owners_.end()) continue;
    const auto& attacker = combat.actors_.at(owner->second);
    if (attacker.controller || attacker.participant.team_code == role.team)
      continue;
    auto [claim, inserted] = defeat_claims_.try_emplace(
        player, DefeatClaim{.assignment = role.assignment});
    if (!inserted && claim->second.assignment != role.assignment)
      claim->second = DefeatClaim{.assignment = role.assignment};
    if (!claim->second.confirmed) {
      claim->second.confirmed =
          DefeatNotice{.victim = player,
                       .attacker = owner->second,
                       .assignment = role.assignment,
                       .life = death->life,
                       .death_count = victim.performance.deaths,
                       .confirmed_at = death->time};
      claim->second.bubble_life = death->bubble_life;
    }
  }
  std::erase_if(defeat_claims_, [&](const auto& entry) {
    const auto actor = combat.actors_.find(entry.first);
    const auto& claim = entry.second;
    if (actor == combat.actors_.end()) return true;
    if (claim.confirmed) {
      if (!combat.actors_.contains(claim.confirmed->attacker) ||
          now - claim.confirmed->confirmed_at > kReportWindow)
        return true;
    } else {
      const auto role = roles_.find(entry.first);
      if (role == roles_.end() || role->second.retired ||
          !role->second.applied ||
          role->second.assignment != claim.assignment ||
          now >= *role->second.expires ||
          actor->second.performance.deaths != role->second.nominated_deaths)
        return true;
    }
    return false;
  });
  for (auto& [player, claim] : defeat_claims_) {
    const auto& actor = combat.actors_.at(player);
    const auto expected_life =
        claim.confirmed
            ? claim.confirmed->life
            : actor.life_generation +
                  (actor.life_state == CombatLifeState::kAlive ? 1U : 0U);
    std::erase_if(claim.receipts, [&](const auto& receipt) {
      return !combat.actors_.contains(receipt.first) ||
             (receipt.second.life != expected_life &&
              receipt.second.life != claim.bubble_life) ||
             now - receipt.second.received > kReportWindow;
    });
  }
  std::erase_if(defeat_claims_, [](const auto& entry) {
    return !entry.second.confirmed && entry.second.receipts.empty();
  });
}

std::vector<CaptainAssignments::DefeatNotice>
CaptainAssignments::CollectDefeats(const CombatRelay& combat, Time now) {
  SyncDefeats(combat, now);
  std::vector<DefeatNotice> result;
  for (auto& [player, claim] : defeat_claims_) {
    static_cast<void>(player);
    if (claim.emitted || !claim.confirmed ||
        now < claim.confirmed->confirmed_at)
      continue;
    const auto receipt = claim.receipts.find(claim.confirmed->attacker);
    if (receipt == claim.receipts.end() || now < receipt->second.received)
      continue;
    auto notice = *claim.confirmed;
    notice.weapon_kind = receipt->second.weapon_kind;
    result.push_back(notice);
    claim.emitted = true;
  }
  return result;
}

CombatRelayResult CaptainAssignments::HandleDefeat(
    domain::PlayerId player, const protocol::Packet& packet,
    const CombatRelay& combat, Time now) {
  const auto report = protocol::DecodeCaptainDefeatReport(packet.body);
  const auto sender = combat.actors_.find(player);
  if (sender == combat.actors_.end() || sender->second.controller)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  const auto& attacker = sender->second.participant;
  if (report.attacker != attacker.actor_object_id ||
      report.attacker_seat != attacker.seat_index)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "captain defeat report belongs to another attacker"};
  const auto victim_owner = combat.object_owners_.find(report.victim);
  if (victim_owner == combat.object_owners_.end())
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  const auto& victim = combat.actors_.at(victim_owner->second);
  if (report.victim_seat != victim.participant.seat_index)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "captain defeat report has a mismatched victim seat"};
  if (victim.controller || victim.participant.team_code == attacker.team_code)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  if (!initialized_ || stopped_)
    return {.reject_reason = CombatRejectReason::kInactiveMatchPhase};
  SyncDefeats(combat, now);
  auto claim = defeat_claims_.find(victim_owner->second);
  if (claim != defeat_claims_.end() && claim->second.confirmed) {
    if (claim->second.confirmed->attacker != player ||
        now < claim->second.confirmed->confirmed_at)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  } else {
    const auto role = roles_.find(victim_owner->second);
    if (role == roles_.end() || role->second.retired || !role->second.applied ||
        now < *role->second.applied || now >= *role->second.expires ||
        victim.life_state == CombatLifeState::kDefeated ||
        victim.performance.deaths != role->second.nominated_deaths)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    if (claim == defeat_claims_.end())
      claim = defeat_claims_
                  .emplace(victim_owner->second,
                           DefeatClaim{.assignment = role->second.assignment})
                  .first;
  }
  if (claim->second.emitted || claim->second.receipts.contains(player))
    return {.reject_reason = CombatRejectReason::kDuplicateAction};
  const auto life =
      claim->second.confirmed
          ? claim->second.confirmed->life
          : victim.life_generation +
                (victim.life_state == CombatLifeState::kAlive ? 1U : 0U);
  claim->second.receipts.emplace(player,
                                 DefeatReceipt{life, now, report.weapon_kind});
  // Receipt only: no invented acknowledgement, relay, points, deaths or
  // replacement nomination.
  return {.accepted = true};
}
}  // namespace bf::runtime
