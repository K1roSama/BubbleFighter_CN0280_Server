#include "features/battle/infection_match.h"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
InfectionMatch::InfectionMatch(std::uint32_t mode, domain::PlayerId mother,
                               Time ready_at)
    : mode_(mode), mother_(mother), ready_at_(ready_at) {
  if ((mode != 8U && mode != 28U) || !mother.IsValid()) {
    throw std::invalid_argument{"invalid infection match"};
  }
}

OutboundMessage InfectionMatch::Convert(domain::PlayerId player,
                                        CombatRelay& combat) {
  auto& actor = combat.actors_.at(player);
  previous_weapons_.try_emplace(player, actor.participant.weapon_templates);
  infected_.insert(player);
  actor.participant.team_code = 2U;  // 战斗内部规范阵营；线协议为 34。
  actor.participant.weapon_templates = {};
  actor.participant.weapon_templates[2U] = player == mother_ ? 7200U : 7201U;
  actor.participant.ordinary_respawn_allowed = false;
  actor.life_state = CombatLifeState::kAlive;
  actor.life_settled = false;
  actor.death_stage = 0U;
  actor.respawn_point.reset();
  actor.pending_kick.reset();
  actor.active_kick.reset();
  actor.last_fire.reset();
  actor.protection_report.clear();
  actor.bubble_impacts.clear();
  actor.turret_impacts.clear();
  actor.humiliations.clear();
  actor.humiliated.reset();
  actor.defeated.reset();
  actor.health_report.reset();
  actor.selected_weapon_bar = 2U;
  ++actor.life_generation;
  if (player == mother_) initial_blast_pending_ = true;
  const auto object = actor.participant.actor_object_id;
  std::erase_if(combat.rescues_, [object](const auto& item) {
    return item.first == object || item.second.helper == object;
  });
  // 转换前创建的武器/弹丸不能在新阵营继续复用。
  std::erase_if(combat.projectiles_, [object](const auto& item) {
    return item.second.owner_object_id == object;
  });
  std::erase_if(combat.weapon_objects_, [object](const auto& item) {
    return item.second.owner_object_id == object;
  });
  return {.opcode = 504U,
          .body = protocol::EncodeInfectionTeamResponse(
              mode_, actor.participant.seat_index, 34U)};
}

std::vector<OutboundMessage> InfectionMatch::Tick(CombatRelay& combat,
                                                  Time now) {
  auto messages = UpdateFever(combat, now);
  if (winner_ != 0U) return messages;
  if (!started_) {
    if (combat.actors_.size() < 2U) {
      winner_ = 1U;
      return messages;
    }
    for (auto& [id, actor] : combat.actors_) {
      static_cast<void>(id);
      actor.participant.team_code = 1U;
      actor.participant.ordinary_respawn_allowed = false;
      actor.infection_actor = true;
    }
    if (now < ready_at_) return messages;
    if (!combat.actors_.contains(mother_)) {
      // A candidate leaving during the opening wait is not a defeated zombie.
      std::vector<domain::PlayerId> candidates;
      for (const auto& [id, actor] : combat.actors_)
        if (actor.life_state == CombatLifeState::kAlive)
          candidates.push_back(id);
      if (candidates.empty()) {
        winner_ = 1U;
        return messages;
      }
      std::mt19937 random{std::random_device{}()};
      mother_ = candidates[std::uniform_int_distribution<std::size_t>{
          0U, candidates.size() - 1U}(random)];
    }
    messages.push_back(Convert(mother_, combat));
    started_ = true;
  }
  const auto mother = combat.actors_.find(mother_);
  if (mother == combat.actors_.end() ||
      mother->second.life_state == CombatLifeState::kDefeated) {
    winner_ = 1U;
    respawns_.clear();
    return messages;
  }
  const auto humans =
      std::ranges::count_if(combat.actors_, [&](const auto& item) {
        // A8ADE0/A77B70 inspect the 504 conversion ledger, not the death state.
        // A defeated human must finish 730 -> 515 -> 504 before zombie results.
        return !infected_.contains(item.first);
      });
  if (humans == 0) {
    winner_ = 2U;
    respawns_.clear();
    return messages;
  }
  std::erase_if(respawns_, [&](const auto& item) {
    const auto actor = combat.actors_.find(item.first);
    return actor == combat.actors_.end() ||
           actor->second.life_state != CombatLifeState::kDefeated ||
           actor->second.life_generation != item.second.generation;
  });
  for (const auto& [id, actor] : combat.actors_) {
    if (actor.life_state != CombatLifeState::kDefeated ||
        actor.death_stage != 2U)
      continue;
    auto [pending, inserted] = respawns_.try_emplace(
        id,
        // 7D84F0 -> 82DA70 enters native 401; 8B6C90 starts the timer there.
        // A73B70 replaces human duration with ZombieCommon.respawnTime (10 s).
        // Child baseline is FighterConfigCommon 7 s + Zombie_Kid1 delta 10 s.
        Respawn{actor.life_generation,
                now + (infected_.contains(id) ? std::chrono::seconds{17}
                                              : std::chrono::seconds{10})});
    static_cast<void>(inserted);
    auto& permission = pending->second;
    const auto& humiliation = actor.humiliated;
    if (!permission.authorized && humiliation &&
        humiliation->life == actor.life_generation &&
        humiliation->serial > permission.last_humiliation) {
      permission.last_humiliation = humiliation->serial;
      // A7C1F0 uses the child spec for every non-mother victim, including
      // humans. 32816 already shortens each client's timer: sending 731 would
      // apply it twice.
      if (infected_.contains(humiliation->source))
        permission.due -= std::chrono::seconds{2};
    }
    if (!permission.authorized && now >= permission.due &&
        permission.generation == actor.life_generation) {
      permission.authorized = true;
      protocol::PacketWriter writer;
      writer.WriteU32(actor.participant.actor_object_id);
      messages.push_back({.opcode = 730U, .body = writer.Release()});
    }
  }
  return messages;
}

std::vector<OutboundMessage> InfectionMatch::Transition(domain::PlayerId player,
                                                        CombatRelay& combat,
                                                        Time now) {
  const auto permission = respawns_.find(player);
  const auto actor = combat.actors_.find(player);
  if (winner_ != 0U || permission == respawns_.end() ||
      (!permission->second.authorized && now < permission->second.due &&
       !permission->second.native_damage_seen) ||
      actor == combat.actors_.end() ||
      actor->second.life_state != CombatLifeState::kDefeated ||
      permission->second.generation != actor->second.life_generation ||
      player == mother_)
    return {};
  auto message = Convert(player, combat);
  respawns_.erase(permission);
  return {std::move(message)};
}

CombatRelayResult InfectionMatch::ReportDuration(domain::PlayerId player,
                                                 const protocol::Packet& packet,
                                                 const CombatRelay& combat) {
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  if (request.kind != protocol::NativeActionKind::kRespawnDurationReport)
    throw std::invalid_argument{"infection duration handler requires32905"};
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end())
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  if (request.actor_object_id != actor->second.participant.actor_object_id)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "native respawn duration belongs to another character"};
  const auto pending = respawns_.find(player);
  if (!started_ || winner_ != 0U || player == mother_ ||
      pending == respawns_.end() ||
      actor->second.life_state != CombatLifeState::kDefeated ||
      actor->second.death_stage != 2U ||
      pending->second.generation != actor->second.life_generation ||
      pending->second.authorized)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  // 8498A0/A73B70: humans are overridden to10s. 57B980/8B2F40 replace zombie
  // equipment with a fresh set; human respawn accessories do not shorten the
  // child.
  const auto expected = infected_.contains(player) ? 17000U : 10000U;
  if (request.respawn_duration_ms != expected)
    return {.reject_reason = CombatRejectReason::kRespawnMismatch};
  if (pending->second.duration_reported)
    return {.reject_reason = CombatRejectReason::kDuplicateAction};
  pending->second.duration_reported = true;
  // This consumes the client report. No native acknowledgement has been
  // identified. Never restart the deadline or undo a stomp by treating initial
  // duration as remaining.
  return {.accepted = true};
}

std::optional<InfectionMatch::Time> InfectionMatch::Deadline() const {
  if (winner_ != 0U) return std::nullopt;
  if (!started_) return ready_at_;
  std::optional<Time> due;
  if (fever_ && !fever_->ended) due = fever_->until;
  for (const auto& [id, request] : respawns_) {
    static_cast<void>(id);
    if (!request.authorized && (!due || request.due < *due)) due = request.due;
  }
  return due;
}

std::uint8_t InfectionMatch::Winner(bool timeout) const noexcept {
  return winner_ != 0U ? winner_ : timeout ? 1U : 0U;
}

std::uint8_t InfectionMatch::Team(domain::PlayerId player) const noexcept {
  return infected_.contains(player) ? 2U : 1U;
}

bool InfectionMatch::PrepareAction(domain::PlayerId player,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat) {
  if (packet.opcode == 33281U || packet.opcode == 32772U) {
    const auto request =
        protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
    const auto actor = combat.actors_.find(player);
    const auto old = previous_weapons_.find(player);
    if (actor != combat.actors_.end() && old != previous_weapons_.end() &&
        request.actor_object_id == actor->second.participant.actor_object_id &&
        std::ranges::find(old->second, request.template_id) !=
            old->second.end() &&
        std::ranges::find(actor->second.participant.weapon_templates,
                          request.template_id) ==
            actor->second.participant.weapon_templates.end())
      return false;
    if (packet.opcode == 33281U && player == mother_ &&
        actor != combat.actors_.end() &&
        request.actor_object_id == actor->second.participant.actor_object_id &&
        started_) {
      const bool base =
          request.template_id == 7200U || request.template_id == 7220U;
      const bool boosted =
          request.template_id == 7202U || request.template_id == 7205U ||
          request.template_id == 7207U || request.template_id == 7222U;
      // A79BF0/A7D890 replace the claw locally without requiring a switch
      // packet.
      if ((base && !FeverActive()) || (boosted && FeverActive()))
        actor->second.participant.weapon_templates[2U] = request.template_id;
      else if (base || boosted)
        return false;
    }
  }
  if (!protocol::IsNativeActionOpcode(packet.opcode)) return true;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto found = combat.actors_.find(player);
  if (found == combat.actors_.end() ||
      request.actor_object_id != found->second.participant.actor_object_id)
    return true;
  auto& actor = found->second;
  if (packet.opcode == 32804U) {
    // 7C8180 writes -32764 (32772), not32804. No native infection grant here.
    return false;
  }
  if (!infected_.contains(player)) return true;
  if (request.weapon_switch) {
    const auto& change = *request.weapon_switch;
    if (player == mother_ && change.bar_index == 2U) {
      const bool base =
          change.template_id == 7200U || change.template_id == 7220U;
      const bool boosted =
          change.template_id == 7202U || change.template_id == 7205U ||
          change.template_id == 7207U || change.template_id == 7222U;
      if ((base && !FeverActive()) || (boosted && FeverActive()))
        actor.participant.weapon_templates[2U] = change.template_id;
      else if (base || boosted)
        return false;
    } else if (previous_weapons_[player][change.bar_index] ==
                   change.template_id &&
               actor.participant.weapon_templates[change.bar_index] !=
                   change.template_id) {
      return false;
    }
  }
  if (request.compressed_fire &&
      !actor.participant.weapon_templates[request.compressed_fire->bar_index])
    return false;
  return true;
}
}  // namespace bf::runtime
