#include "features/battle/weapon_master_match.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bf::runtime {
namespace {
// Transport tolerance owned by this server, not an inferred native animation
// time.
constexpr auto kAssistWindow = std::chrono::seconds{3};
constexpr std::array kVerifiedPrimaries{2004U, 3103U, 4004U};
}  // namespace

WeaponMasterMatch::WeaponMasterMatch(WeaponMasterRules rules,
                                     std::uint32_t seed)
    : rules_(std::move(rules)), random_(seed) {
  const std::set unique(rules_.primary_pool.begin(), rules_.primary_pool.end());
  if (rules_.final_level < 2U || rules_.final_level > 100U ||
      rules_.level_step == 0U || rules_.level_step > 10'000U ||
      rules_.pang_experience == 0U ||
      rules_.pang_experience > rules_.level_step ||
      rules_.assist_experience == 0U ||
      rules_.assist_experience > rules_.level_step || unique.size() < 2U ||
      unique.size() != rules_.primary_pool.size() ||
      !std::ranges::all_of(unique, [](auto weapon) {
        return std::ranges::find(kVerifiedPrimaries, weapon) !=
               kVerifiedPrimaries.end();
      })) {
    throw std::invalid_argument{"invalid server weapon master rules"};
  }
}

protocol::WeaponMasterProgress WeaponMasterMatch::Progress(
    domain::PlayerId player) const {
  const auto& state = players_.at(player);
  const auto level = 1U + state.experience / rules_.level_step;
  return {.level = level,
          .experience = state.experience,
          .level_base = (level - 1U) * rules_.level_step,
          .level_next = level * rules_.level_step,
          .assists = state.assists};
}

OutboundMessage WeaponMasterMatch::Change(
    domain::PlayerId player, CombatRelay& combat,
    protocol::WeaponMasterChangeReason reason) {
  auto& actor = combat.actors_.at(player);
  auto& state = players_.at(player);
  const auto previous = actor.participant.weapon_templates[0U];
  std::vector<std::uint32_t> choices;
  for (const auto weapon : rules_.primary_pool)
    if (weapon != previous) choices.push_back(weapon);
  const auto selected = choices[std::uniform_int_distribution<std::size_t>{
      0U, choices.size() - 1U}(random_)];
  const std::array weapons{protocol::WeaponMasterWeapon{0U, selected}};
  OutboundMessage message{.opcode = 1008U,
                          .body = protocol::EncodeWeaponMasterChange(
                              actor.participant.seat_index, weapons, reason)};
  if (previous) state.former_primaries.insert(*previous);
  actor.participant.weapon_templates[0U] = selected;
  state.no_pang_respawns = 0U;
  // In-flight damager cookies retain their original identity. They must not be
  // erased/reissued on a level-up; native 84AA00 only changes the equipped gun.
  return message;
}

WeaponMasterUpdate WeaponMasterMatch::Start(CombatRelay& combat) {
  if (started_) return {};
  if (combat.mode_id_ != 26U || !combat.individual_scoring_ ||
      combat.actors_.size() < 6U || combat.actors_.size() > 8U ||
      std::ranges::any_of(combat.actors_, [](const auto& entry) {
        return entry.second.controller ||
               !entry.second.participant.ordinary_respawn_allowed;
      }))
    throw std::invalid_argument{
        "weapon master requires 6-8 respawning human participants"};
  for (const auto& [id, actor] : combat.actors_) {
    static_cast<void>(actor);
    players_.try_emplace(id);
  }
  WeaponMasterUpdate update{.score_changed = true};
  for (const auto& [id, actor] : combat.actors_) {
    static_cast<void>(actor);
    update.messages.push_back(
        Change(id, combat, protocol::WeaponMasterChangeReason::kStart));
  }
  started_ = true;
  return update;
}

void WeaponMasterMatch::Award(domain::PlayerId player, std::uint32_t amount,
                              CombatRelay& combat, WeaponMasterUpdate& update) {
  auto& state = players_.at(player);
  const auto previous = Progress(player).level;
  state.experience = std::min(state.experience + amount,
                              (rules_.final_level - 1U) * rules_.level_step);
  update.score_changed = true;
  const auto current = Progress(player).level;
  if (current > previous)
    update.messages.push_back(
        Change(player, combat, protocol::WeaponMasterChangeReason::kLevelUp));
  if (current == rules_.final_level) target_reached_ = true;
}

void WeaponMasterMatch::FinishIfReady(const CombatRelay& combat, Time now,
                                      WeaponMasterUpdate& update) {
  if (!target_reached_ || finished_) return;
  finish_deadline_.reset();
  for (const auto& [id, actor] : combat.actors_) {
    const auto& state = players_.at(id);
    if (state.assist_claimed ||
        actor.life_state != CombatLifeState::kDefeated || !actor.defeated ||
        state.settled_death != actor.life_generation ||
        now >= actor.defeated->time + kAssistWindow)
      continue;
    const bool candidate = std::ranges::any_of(state.sources, [&](auto source) {
      const auto owner = combat.object_owners_.find(source);
      return owner != combat.object_owners_.end() && owner->second != id &&
             players_.contains(owner->second) &&
             source != actor.defeated->attacker;
    });
    if (candidate) {
      const auto due = actor.defeated->time + kAssistWindow;
      if (!finish_deadline_ || due > *finish_deadline_) finish_deadline_ = due;
    }
  }
  if (!finish_deadline_) {
    finished_ = true;
    update.finished = true;
  }
}

WeaponMasterUpdate WeaponMasterMatch::Tick(const CombatRelay& combat,
                                           Time now) {
  WeaponMasterUpdate update;
  FinishIfReady(combat, now, update);
  return update;
}

bool WeaponMasterMatch::ValidAssist(
    domain::PlayerId victim, const protocol::WeaponMasterAssistRequest& request,
    const CombatRelay& combat) const {
  const auto actor = combat.actors_.find(victim);
  const auto finisher = combat.object_owners_.find(request.finisher_object);
  const auto assistant =
      std::ranges::find_if(combat.actors_, [&](const auto& entry) {
        return entry.second.participant.seat_index == request.assistant_seat;
      });
  return actor != combat.actors_.end() &&
         finisher != combat.object_owners_.end() &&
         players_.contains(finisher->second) && finisher->second != victim &&
         assistant != combat.actors_.end() &&
         players_.contains(assistant->first) && assistant->first != victim &&
         assistant->first != finisher->second &&
         players_.at(victim).sources.contains(
             assistant->second.participant.actor_object_id);
}

void WeaponMasterMatch::SettleAssist(domain::PlayerId victim,
                                     CombatRelay& combat, Time now,
                                     WeaponMasterUpdate& update) {
  auto& state = players_.at(victim);
  const auto& actor = combat.actors_.at(victim);
  if (!state.pending || state.assist_claimed || !actor.defeated ||
      actor.life_state != CombatLifeState::kDefeated)
    return;
  const auto& report = *state.pending;
  if (now < report.received || now - report.received > kAssistWindow ||
      now < actor.defeated->time ||
      now - actor.defeated->time > kAssistWindow ||
      actor.defeated->life != actor.life_generation ||
      actor.defeated->attacker != report.request.finisher_object ||
      !ValidAssist(victim, report.request, combat)) {
    state.pending.reset();
    return;
  }
  const auto assistant =
      std::ranges::find_if(combat.actors_, [&](const auto& entry) {
        return entry.second.participant.seat_index ==
               report.request.assistant_seat;
      })->first;
  state.assist_claimed = true;
  state.pending.reset();
  auto& count = players_.at(assistant).assists;
  if (count <
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
    ++count;
  Award(assistant, rules_.assist_experience, combat, update);
}

WeaponMasterUpdate WeaponMasterMatch::Observe(domain::PlayerId sender,
                                              const protocol::Packet& packet,
                                              CombatRelay& combat, Time now) {
  if (!started_ || target_reached_ || !players_.contains(sender) ||
      !combat.actors_.contains(sender))
    return {};
  auto& state = players_.at(sender);
  const auto& actor = combat.actors_.at(sender);
  if (protocol::IsNativeLifecycleOpcode(packet.opcode) &&
      protocol::DecodeNativeLifecycle(packet.opcode, packet.body).kind ==
          protocol::NativeLifecycleKind::kRecovery) {
    // A rescue/recovery begins a new damage history, but is not an authorized
    // ordinary respawn and cannot advance the no-Pang reroll counter.
    state.sources.clear();
    state.pending.reset();
    state.assist_claimed = false;
  }
  if (protocol::IsNativeRespawnOpcode(packet.opcode)) {
    const auto respawn =
        protocol::DecodeNativeRespawn(packet.opcode, packet.body);
    if (respawn.completion && actor.life_state == CombatLifeState::kAlive &&
        state.completed_respawn != actor.life_generation) {
      state.completed_respawn = actor.life_generation;
      if (state.rerolled_life != actor.life_generation)
        state.no_pang_respawns = std::min(state.no_pang_respawns + 1U, 2U);
      state.sources.clear();
      state.pending.reset();
      state.assist_claimed = false;
    }
  }
  std::optional<std::uint32_t> target;
  if (protocol::IsNativeHitOpcode(packet.opcode))
    target =
        protocol::DecodeNativeHit(packet.opcode, packet.body).target_object_id;
  if (protocol::IsNativeWeaponObjectOpcode(packet.opcode)) {
    const auto hit =
        protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
    if (hit.kind == protocol::NativeWeaponObjectKind::kHit)
      target = hit.target_object_id;
  }
  if (target && combat.object_owners_.contains(*target)) {
    const auto victim = combat.object_owners_.at(*target);
    if (victim != sender && players_.contains(victim) &&
        combat.actors_.at(victim).life_state != CombatLifeState::kDefeated)
      players_.at(victim).sources.insert(actor.participant.actor_object_id);
  }
  WeaponMasterUpdate update;
  for (const auto& [id, current] : combat.actors_) {
    auto& player = players_.at(id);
    if (current.life_state != CombatLifeState::kDefeated || !current.defeated ||
        player.settled_death == current.life_generation)
      continue;
    player.settled_death = current.life_generation;
    const auto finisher =
        combat.object_owners_.find(current.defeated->attacker);
    if (finisher != combat.object_owners_.end() && finisher->second != id &&
        players_.contains(finisher->second)) {
      players_.at(finisher->second).no_pang_respawns = 0U;
      Award(finisher->second, rules_.pang_experience, combat, update);
      SettleAssist(id, combat, now, update);
    }
  }
  FinishIfReady(combat, now, update);
  return update;
}

WeaponMasterUpdate WeaponMasterMatch::Request(domain::PlayerId sender,
                                              const protocol::Packet& packet,
                                              CombatRelay& combat, Time now) {
  std::optional<protocol::WeaponMasterAssistRequest> assist;
  if (packet.opcode == 760U)
    protocol::ValidateWeaponMasterReroll(packet.body);
  else if (packet.opcode == 2026U)
    assist = protocol::DecodeWeaponMasterAssist(packet.body);
  else
    throw std::invalid_argument{"not a weapon master request"};
  const auto rejected = [](CombatRejectReason reason) {
    return WeaponMasterUpdate{.rejection = reason};
  };
  if (!started_ || finished_ || !players_.contains(sender) ||
      !combat.actors_.contains(sender))
    return rejected(CombatRejectReason::kInactiveMatchPhase);
  auto& state = players_.at(sender);
  const auto& actor = combat.actors_.at(sender);
  if (target_reached_ &&
      (!assist || actor.life_state != CombatLifeState::kDefeated))
    return rejected(CombatRejectReason::kInactiveMatchPhase);
  if (!assist) {
    if (state.rerolled_life == actor.life_generation)
      return rejected(CombatRejectReason::kDuplicateAction);
    // 82C8F0 can send 760 while applying the server's 707, before the caller
    // sends 32926. Count that one authorized completion provisionally, once.
    const bool authorized = actor.life_state == CombatLifeState::kDefeated &&
                            actor.respawn_point &&
                            state.completed_respawn != actor.life_generation;
    if ((!authorized && actor.life_state != CombatLifeState::kAlive) ||
        state.no_pang_respawns + static_cast<std::uint32_t>(authorized) < 2U)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    state.rerolled_life = actor.life_generation;
    return {.messages = {Change(sender, combat,
                                protocol::WeaponMasterChangeReason::kRevival)}};
  }
  if (state.assist_claimed)
    return rejected(CombatRejectReason::kDuplicateAction);
  if (!ValidAssist(sender, *assist, combat))
    return rejected(CombatRejectReason::kUnknownRelatedActor);
  if (actor.life_state == CombatLifeState::kDefeated &&
      (!actor.defeated || actor.defeated->attacker != assist->finisher_object ||
       now < actor.defeated->time ||
       now - actor.defeated->time > kAssistWindow))
    return rejected(CombatRejectReason::kInvalidLifecycleState);
  if (state.pending && now >= state.pending->received &&
      now - state.pending->received <= kAssistWindow)
    return rejected(CombatRejectReason::kDuplicateAction);
  state.pending = Assist{*assist, now};
  WeaponMasterUpdate update;
  SettleAssist(sender, combat, now, update);
  FinishIfReady(combat, now, update);
  return update;
}

bool WeaponMasterMatch::StaleSwitch(domain::PlayerId sender,
                                    const protocol::Packet& packet,
                                    const CombatRelay& combat) const {
  if (packet.opcode != 32810U || !players_.contains(sender) ||
      !combat.actors_.contains(sender))
    return false;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto& actor = combat.actors_.at(sender);
  return request.actor_object_id == actor.participant.actor_object_id &&
         request.weapon_switch && request.weapon_switch->bar_index == 0U &&
         actor.participant.weapon_templates[0U] !=
             request.weapon_switch->template_id &&
         players_.at(sender).former_primaries.contains(
             request.weapon_switch->template_id);
}

}  // namespace bf::runtime
