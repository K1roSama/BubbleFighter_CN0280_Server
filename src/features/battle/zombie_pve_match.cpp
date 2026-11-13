#include "features/battle/zombie_pve_match.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

#include "features/battle/pirate_map_agents.h"
#include "features/battle/zombie_pve_maps.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
enum class Role {
  kEnemy,
  kProtected,
  kBoss,
  kRescueCitizen,
  kRescueEnemy,
  kRescueFailure
};

struct SpawnRule final {
  std::string_view map;
  std::uint32_t spawner;
  std::uint32_t property;
  std::uint8_t team;
  Role role;
};

constexpr SpawnRule kSpawns[]{
#include "features/battle/generated/zombie_pve_spawns.inc"
};

struct NpcLoadout final {
  std::string_view map;
  std::uint32_t property;
  std::uint32_t weapon;
};

constexpr NpcLoadout kLoadouts[]{
#include "features/battle/generated/zombie_pve_loadouts.inc"
};

std::optional<std::uint8_t> ScoreUnit(std::uint32_t property) {
  // 原CHR_PROPERTY联接AiFighterConfig；-1和空值均不推测成零类。
  constexpr std::array<std::int8_t, 37U> units{
      0, 1, 2, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5,
      4, 6, 7, 9, 8,  10, 11, 13, 12, 14, 15, -1, -1, -1, -1, -1, -1, -1};
  if (property >= units.size() || units[property] < 0) return std::nullopt;
  return static_cast<std::uint8_t>(units[property]);
}

struct RescueRule final {
  std::string_view map;
  std::uint32_t index;
  std::uint32_t enemies;
};

constexpr RescueRule kRescues[]{
#include "features/battle/generated/zombie_pve_rescues.inc"
};
// Scheduling/reward values are server policy, not constants recovered from the
// client. Native S2C926 supplies the start and duration in milliseconds.
constexpr std::uint32_t kRescueFirstMs = 45'000U;
constexpr std::uint32_t kRescueIntervalMs = 60'000U;
constexpr std::uint32_t kRescueDurationMs = 30'000U;

std::uint32_t RescueStartMs(std::uint32_t index) {
  return kRescueFirstMs + index * kRescueIntervalMs;
}

// Native B4A210 looks up a server-supplied slot. Ordering is server
// configuration; item IDs and cooldowns come from the original
// ItemSpecZombiePve table.
constexpr std::uint32_t kSkillTypes[]{229U, 17U, 101U, 146U, 136U};
constexpr std::uint32_t kSkillCooldowns[]{4'000U, 0U, 9'000U, 8'500U, 15'000U};
// All seven released Maps/<map>/<map>_ZombiePve.ini files have three
// ATTACK_LEVEL entries and this union of period/anger/near/far/combo/wide
// skills. Test and Skill_Test map files are deliberately excluded from this
// contract.
constexpr std::uint32_t kBossSkills[]{126U, 203U, 204U, 207U, 208U, 209U, 210U,
                                      211U, 217U, 218U, 219U, 220U, 221U, 222U};
// Ten seconds preserves the existing six-second delayed-binding regression;
// it is local protection, not an
// original server timer or authority to create an NPC from a weapon packet.
constexpr auto kUnboundActionLifetime = std::chrono::seconds{10};

std::optional<std::uint32_t> UnboundActionObject(
    const protocol::Packet& packet) {
  if (protocol::IsNativeActionOpcode(packet.opcode))
    return protocol::DecodeNativeAction(packet.opcode, packet.body)
        .actor_object_id;
  if (protocol::IsNativeLifecycleOpcode(packet.opcode))
    return protocol::DecodeNativeLifecycle(packet.opcode, packet.body)
        .actor_object_id;
  if (protocol::IsNativeWeaponObjectOpcode(packet.opcode))
    return protocol::DecodeNativeWeaponObject(packet.opcode, packet.body)
        .actor_object_id;
  if (packet.opcode == 32899U || packet.opcode == 32910U)
    return protocol::DecodeNativeStorm(packet.opcode, packet.body)
        .actor_object_id;
  return std::nullopt;
}
}  // namespace

bool ZombiePveMatch::PrestartNpcAction(const protocol::Packet& packet,
                                       std::uint16_t host_seat) {
  const auto object = UnboundActionObject(packet);
  const auto base =
      1'000'000U + (static_cast<std::uint32_t>(host_seat) + 4U) * 100'000U;
  return host_seat < 4U && object && *object >= base &&
         *object < base + 100'000U;
}

ZombiePveMatch::ZombiePveMatch(std::string map) : map_(std::move(map)) {
  if (!FindZombiePveMap(map_))
    throw std::invalid_argument{"unknown zombie PVE map"};
  static thread_local std::mt19937 random{std::random_device{}()};
  for (const auto& rule : kRescues) {
    if (rule.map == map_) {
      if (rule.index != rescues_.size())
        throw std::logic_error{"non-contiguous PVE rescue groups"};
      rescues_.push_back(
          {.citizen =
               std::uniform_int_distribution<std::uint32_t>{0U, 7U}(random),
           .enemies = rule.enemies});
    }
  }
}

OutboundMessage ZombiePveMatch::RescueTasks() const {
  protocol::PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(rescues_.size()));
  for (std::uint32_t index = 0U; index < rescues_.size(); ++index) {
    const auto& rescue = rescues_[index];
    const auto grade = index == 0U ? 1U : index < 4U ? 2U : 3U;
    writer.WriteU32(rescue.citizen * 3U + grade);  // MissionInfo_Clt ID
    writer.WriteU32(
        0U);  // Three opaque server metadata fields, unused by this client.
    writer.WriteU32(0U);
    writer.WriteU32(0U);
    writer.WriteU32(RescueStartMs(index));
    writer.WriteU32(kRescueDurationMs);
    writer.WriteU32(
        4U);  // 6B5560: [killmonster, citizen count, enemy count, citizen name]
    writer.WriteUtf16(u"killmonster");
    writer.WriteUtf16(u"1");
    const auto count = std::to_string(rescue.enemies);
    writer.WriteUtf16(std::u16string{count.begin(), count.end()});
    const auto name = std::string{"ms_citizen"} +
                      (rescue.citizen ? std::to_string(rescue.citizen) : "");
    writer.WriteUtf16(std::u16string{name.begin(), name.end()});
    writer.WriteU32(0U);  // Native bool serialized as u32.
  }
  return {.opcode = 926U, .body = writer.Release()};
}

void ZombiePveMatch::Install(domain::PlayerId host, CombatRelay& combat) {
  const auto actor = combat.actors_.find(host);
  if (actor == combat.actors_.end())
    throw std::invalid_argument{"PVE host missing"};
  const auto base =
      1'000'000U + (actor->second.participant.seat_index + 4U) * 100'000U;
  for (auto& [id, player] : combat.actors_) {
    if (player.participant.seat_index >= 4U ||
        player.participant.team_code != 2U)
      throw std::invalid_argument{"PVE human seat overlaps native NPC range"};
    player.participant.ordinary_respawn_allowed = false;
    remaining_lives_.try_emplace(id, std::uint8_t{3U});
  }
  host_ = host;
  npc_bases_.insert(base);
}

std::vector<OutboundMessage> ZombiePveMatch::MemberLeft(
    domain::PlayerId departed, domain::PlayerId next_host,
    CombatRelay& combat) {
  std::vector<OutboundMessage> messages;
  const auto next = combat.actors_.find(next_host);
  if (next == combat.actors_.end() || next->second.controller)
    throw std::logic_error{"PVE successor missing"};
  const auto leaving = combat.actors_.find(departed);
  if (leaving != combat.actors_.end()) {
    const auto object = leaving->second.participant.actor_object_id;
    for (auto& agent : agents_) {
      if (!agent.active || !agent.object || agent.pursuit_target != object)
        continue;
      agent.pursuit_target = 0xffffffffU;
      protocol::PacketWriter clear;
      clear.WriteU32(*agent.object);
      clear.WriteU32(0xffffffffU);
      messages.push_back({.opcode = 32897U, .body = clear.Release()});
    }
  }
  if (host_ != next_host) {
    // Native 7FE950/B49E80 consult the current host for existing owner
    // slots 4..7. Old objects retain their IDs; 7D31A0 allocates future objects
    // at new host+4.
    for (auto& [id, actor] : combat.actors_) {
      static_cast<void>(id);
      if (actor.controller == host_) actor.controller = next_host;
    }
    host_ = next_host;
    // Unbound actions have no accepted object/slot identity to transfer.
    deferred_actions_.clear();
    unbound_objects_.clear();
    npc_bases_.insert(1'000'000U +
                      (next->second.participant.seat_index + 4U) * 100'000U);
  }
  combat.SetHost(next_host);
  items_.erase(departed);
  skill_due_.erase(departed);
  std::erase_if(skill_uses_,
                [departed](const auto& use) { return use.player == departed; });
  reported_units_.erase(departed);
  reported_combo_.erase(departed);
  remaining_lives_.erase(departed);
  authorized_.erase(departed);
  old_weapons_.erase(departed);
  return messages;
}

bool ZombiePveMatch::IsNpcObject(std::uint32_t object) const {
  return std::ranges::any_of(npc_bases_, [object](auto base) {
    return object >= base && object < base + 100'000U;
  });
}

bool ZombiePveMatch::Bind(std::uint32_t slot, std::uint32_t object,
                          CombatRelay& combat) {
  auto& agent = agents_.at(slot);
  if (!agent.active || !IsNpcObject(object)) return false;
  if (agent.object) return *agent.object == object;
  if (combat.object_ids_.contains(object) || retired_.contains(object))
    return false;
  const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) | object};
  CombatRelay::ActorState state;
  state.participant = {.player_id = synthetic,
                       .actor_object_id = object,
                       .team_code = agent.team,
                       .weapon_templates = {},
                       .ordinary_respawn_allowed = false};
  state.controller = host_;
  // 7D31A0 -> AiFighterConfig equips these during construction, without 32810.
  // This includes the friendly guardian's 9024 rifle and the zombies' claws.
  if (agent.default_weapon)
    state.participant.weapon_templates[0U] = agent.default_weapon;
  if (agent.boss && !agent.boss_final_sequence)
    state.npc_blast_templates = {5204U, 5205U, 5206U, 5207U, 5208U, 5209U};
  // 原版 PVE NPC 也会用当前枪械模板创建 33281（实机模板 7204）。
  state.melee_uses_equipped_template = true;
  // All seven released maps define WideAreaSkillId 211/217 for their Boss.
  state.storm_source = agent.boss && !agent.boss_final_sequence ? 2U : 0U;
  combat.actors_.emplace(synthetic, std::move(state));
  combat.object_ids_.insert(object);
  combat.object_owners_.emplace(object, synthetic);
  agent.object = object;
  if (agent.spawner >= 300U && agent.spawner < 400U)
    rescues_.at(agent.spawner - 300U).citizen_object = object;
  if (agent.spawner >= 400U && agent.spawner < 500U)
    rescues_.at(agent.spawner - 400U).defeated.emplace(object, false);
  std::erase(pending_, slot);
  unbound_objects_.erase(object);
  return true;
}

bool ZombiePveMatch::RememberUnbound(std::uint32_t object) {
  if (retired_.contains(object)) return false;
  if (unbound_objects_.contains(object)) return true;
  if (unbound_objects_.size() >= pending_.size()) return false;
  unbound_objects_.insert(object);
  return true;
}

void ZombiePveMatch::PruneDeferredActions(
    std::chrono::steady_clock::time_point now) {
  std::erase_if(deferred_actions_, [&](const auto& value) {
    const auto& [object, batch] = value;
    const bool current_spawn =
        std::ranges::any_of(batch.authorized_spawns, [&](const auto& spawn) {
          const auto& agent = agents_.at(spawn.first);
          return agent.active && agent.generation == spawn.second &&
                 (!agent.object || agent.object == object);
        });
    if (batch.controller == host_ && now >= batch.received_at &&
        now < batch.expires_at && current_spawn)
      return false;
    unbound_objects_.erase(object);
    return true;
  });
}

bool ZombiePveMatch::DeferAction(domain::PlayerId sender,
                                 const protocol::Packet& packet,
                                 const CombatRelay& combat,
                                 std::chrono::steady_clock::time_point now) {
  protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
  PruneDeferredActions(now);
  const auto object = UnboundActionObject(packet);
  if (winner_ || sender != host_ || !object || !IsNpcObject(*object) ||
      combat.object_ids_.contains(*object) || packet.body.size() > 4096U ||
      !RememberUnbound(*object))
    return false;
  if (!deferred_actions_.contains(*object)) {
    DeferredActions batch{.controller = sender,
                          .received_at = now,
                          .expires_at = now + kUnboundActionLifetime};
    for (const auto slot : pending_) {
      const auto& agent = agents_.at(slot);
      if (agent.active && !agent.object)
        batch.authorized_spawns.emplace(slot, agent.generation);
    }
    if (batch.authorized_spawns.empty()) return false;
    deferred_actions_.emplace(*object, std::move(batch));
  }
  auto& actions = deferred_actions_.at(*object).packets;
  if (actions.size() >= 64U) return false;
  actions.push_back(packet);
  return true;
}

std::vector<protocol::Packet> ZombiePveMatch::TakeDeferredActions(
    std::uint32_t object, std::chrono::steady_clock::time_point now) {
  PruneDeferredActions(now);
  const auto found = std::ranges::find_if(agents_, [object](const auto& a) {
    return a.active && a.object == object;
  });
  if (found == agents_.end()) return {};
  auto entry = deferred_actions_.extract(object);
  if (entry.empty()) return {};
  const auto slot = static_cast<std::uint32_t>(found - agents_.begin());
  const auto generation = entry.mapped().authorized_spawns.find(slot);
  if (generation == entry.mapped().authorized_spawns.end() ||
      generation->second != found->generation)
    return {};
  return std::move(entry.mapped().packets);
}

bool ZombiePveMatch::MapMessage(const protocol::Packet& packet,
                                CombatRelay* combat) {
  PirateMapAgents::Validate(packet);
  if (winner_) return false;
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  if (packet.opcode == 32892U) {
    const auto property = reader.ReadU32();
    static_cast<void>(reader.ReadF64());
    const auto slot = reader.ReadU32();
    const auto rule = std::ranges::find_if(kSpawns, [&](const auto& row) {
      return row.map == map_ && row.spawner == object &&
             row.property == property;
    });
    auto& agent = agents_[slot];
    if (rule == std::end(kSpawns) || agent.active ||
        agent.generation == std::numeric_limits<std::uint64_t>::max())
      return false;
    // Global 32913/32914 receivers select the first Boss. Two active Bosses
    // would make their identity ambiguous even when their slots differ.
    if (rule->role == Role::kBoss &&
        std::ranges::any_of(agents_, [](const auto& entry) {
          return entry.active && entry.boss;
        }))
      return false;
    if (object >= 300U) {
      const auto index = object % 100U;
      if (!combat || index >= rescues_.size()) return false;
      auto& rescue = rescues_[index];
      if (rule->role == Role::kRescueFailure) {
        if (rescue.state != RescueState::kFailed || rescue.failure_spawned)
          return false;
        rescue.failure_spawned = true;
      } else {
        if (index != rescue_index_ || rescue.state != RescueState::kActive)
          return false;
        if (rule->role == Role::kRescueCitizen) {
          if (rescue.citizen_spawned || property != 10U + rescue.citizen)
            return false;
          rescue.citizen_spawned = true;
        } else {
          if (rescue.spawned >= rescue.enemies) return false;
          ++rescue.spawned;
        }
      }
    }
    const auto loadout = std::ranges::find_if(kLoadouts, [&](const auto& row) {
      return row.map == map_ && row.property == property;
    });
    if (loadout == std::end(kLoadouts))
      throw std::logic_error{"PVE spawner has no original loadout"};
    agent = {.generation = agent.generation + 1U,
             .active = true,
             .team = rule->team,
             .object = std::nullopt,
             .spawn = packet.body,
             .spawner = object,
             .default_weapon = loadout->weapon,
             .boss = rule->role == Role::kBoss,
             .protected_object = rule->role == Role::kProtected,
             .score_unit = ScoreUnit(property)};
    if (agent.boss) {
      boss_claim_ = success_claim_ = boss_defeated_ = boss_final_ =
          boss_zero_hp_ = false;
      boss_finish_due_.reset();
    }
    pending_.push_back(slot);
    boss_spawned_ |= rule->role == Role::kBoss;
    protected_spawned_ |= rule->role == Role::kProtected;
    return true;
  }
  if (!combat || !IsNpcObject(object)) return false;
  if (packet.opcode == 32891U) {
    if (std::ranges::any_of(agents_, [object](const auto& a) {
          return a.active && a.object == object;
        }))
      return true;
    // 动画不携带槽位；跨 NPC 的动画/武器顺序不保证等于生成顺序。
    // 允许同行播放动画，但仅由 32893/32890 中的明确槽位完成身份绑定。
    return RememberUnbound(object);
  }
  const auto hint = reader.ReadU32();
  // Original 7D2940 resolves removal by object, not by its stale slot hint.
  // Never let a retired or absent object remove a replacement in that slot.
  const auto known = std::ranges::find_if(agents_, [object](const auto& a) {
    return a.active && a.object == object;
  });
  const auto slot = packet.opcode == 32890U && known != agents_.end()
                        ? static_cast<std::uint32_t>(known - agents_.begin())
                        : hint;
  if (slot >= agents_.size()) return false;
  if (!Bind(slot, object, *combat)) return false;
  if (packet.opcode == 32890U) {
    ObserveRescueDeaths(
        *combat);  // Preserve evidence before a slot/object is retired.
    ObserveScoreUnits(*combat);
    ObserveTerminalState(*combat);
    agents_[slot].active = false;
    retired_.insert(object);
    deferred_actions_.erase(object);
    static_cast<void>(
        combat->RemoveParticipant(combat->object_owners_.at(object)));
  } else {
    std::array<float, 3U> position{};
    for (auto& value : position) value = reader.ReadF32();
    combat->actors_.at(combat->object_owners_.at(object)).position = position;
  }
  return true;
}

bool ZombiePveMatch::PrepareAction(domain::PlayerId sender,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat) {
  protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
  std::optional<std::uint32_t> object = UnboundActionObject(packet);
  std::optional<protocol::NativeActionRequest> action;
  if (protocol::IsNativeActionOpcode(packet.opcode)) {
    action = protocol::DecodeNativeAction(packet.opcode, packet.body);
    object = action->actor_object_id;
  } else if (protocol::IsNativeLifecycleOpcode(packet.opcode)) {
    const auto request =
        protocol::DecodeNativeLifecycle(packet.opcode, packet.body);
    object = request.actor_object_id;
    const auto actor = combat.actors_.find(sender);
    if (request.kind == protocol::NativeLifecycleKind::kRecovery &&
        actor != combat.actors_.end() && !actor->second.controller &&
        request.actor_object_id == actor->second.participant.actor_object_id &&
        actor->second.life_state == CombatLifeState::kDefeated)
      return false;  // Bubble recovery cannot bypass the 730 -> 704/515 death
                     // branch.
  } else if (protocol::IsNativeRespawnOpcode(packet.opcode)) {
    const auto request =
        protocol::DecodeNativeRespawn(packet.opcode, packet.body);
    const auto actor = combat.actors_.find(sender);
    if (actor != combat.actors_.end() &&
        request.actor_object_id == actor->second.participant.actor_object_id) {
      if (infected_.contains(sender) || !remaining_lives_.contains(sender) ||
          remaining_lives_.at(sender) == 0U || !authorized_.contains(sender) ||
          authorized_.at(sender) != actor->second.life_generation)
        return false;
    }
  }
  if (!object) return true;
  if (IsNpcObject(*object)) {
    if (sender != host_ || retired_.contains(*object)) return false;
    if (!combat.object_ids_.contains(*object)) return false;
    const auto agent = std::ranges::find_if(agents_, [&](const auto& row) {
      return row.active && row.object == object;
    });
    if (agent != agents_.end() && agent->boss_final_sequence &&
        (packet.opcode == 33281U || packet.opcode == 32772U ||
         packet.opcode == 32899U || packet.opcode == 32910U ||
         (action && action->compressed_fire)))
      return false;  // A late binding cannot revive construction after native
                     // final retreat.
    if (action && action->weapon_switch) {
      const auto& change = *action->weapon_switch;
      if (agent == agents_.end() || change.bar_index != 0U ||
          !agent->default_weapon || change.template_id != agent->default_weapon)
        return false;
      combat.actors_.at(combat.object_owners_.at(*object))
          .participant.weapon_templates[change.bar_index] = change.template_id;
    }
    return true;
  }
  const auto actor = combat.actors_.find(sender);
  if (action && actor != combat.actors_.end() &&
      *object == actor->second.participant.actor_object_id &&
      infected_.contains(sender)) {
    if (action->weapon_switch) {
      const auto& change = *action->weapon_switch;
      if (old_weapons_[sender][change.bar_index] == change.template_id &&
          actor->second.participant.weapon_templates[change.bar_index] !=
              change.template_id)
        return false;
    }
    if (action->compressed_fire &&
        !actor->second.participant
             .weapon_templates[action->compressed_fire->bar_index])
      return false;
  }
  return packet.opcode != 32804U;
}

OutboundMessage ZombiePveMatch::SkillSlots() {
  protocol::PacketWriter writer;
  writer.WriteU32(5U);
  for (std::uint32_t slot = 0U; slot < 5U; ++slot) {
    writer.WriteU32(slot);
    writer.WriteU32(kSkillTypes[slot]);
    writer.WriteU32(slot);
  }
  return {.opcode = 932U, .body = writer.Release()};
}

CombatRelayResult ZombiePveMatch::HandleEvent(
    domain::PlayerId sender, const protocol::Packet& packet,
    CombatRelay& combat, std::chrono::steady_clock::time_point now) {
  if (protocol::IsPveEventOpcode(packet.opcode))
    protocol::ValidatePveEvent(packet.opcode, packet.body);
  else {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    reader.RequireEnd();
  }
  const auto rejected = [](CombatRejectReason reason) {
    return CombatRelayResult{.accepted = false, .reject_reason = reason};
  };
  const auto actor = combat.actors_.find(sender);
  if (winner_ || actor == combat.actors_.end() || actor->second.controller)
    return rejected(CombatRejectReason::kInactiveMatchPhase);
  const auto deliver = [&](std::vector<OutboundMessage> messages,
                           bool exclude_sender = false) {
    CombatRelayResult result{.accepted = true};
    for (const auto& [id, member] : combat.actors_) {
      if (member.controller || (exclude_sender && id == sender)) continue;
      for (const auto& message : messages)
        result.deliveries.push_back({.recipient = id, .message = message});
    }
    return result;
  };
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 32913U || packet.opcode == 32908U) {
    if (sender != host_) return rejected(CombatRejectReason::kUnknownTarget);
    const auto boss = std::ranges::find_if(
        agents_, [](const auto& entry) { return entry.active && entry.boss; });
    if (boss == agents_.end() || boss->boss_final_sequence)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    if (boss->object) {
      const auto owner = combat.object_owners_.find(*boss->object);
      if (owner == combat.object_owners_.end() ||
          combat.actors_.at(owner->second).life_state !=
              CombatLifeState::kAlive)
        return rejected(CombatRejectReason::kInvalidLifecycleState);
    }
    if (packet.opcode == 32913U) {
      const auto phase = reader.ReadU32();
      const auto parameter = reader.ReadU32();
      if ((phase == 8U && std::ranges::find(kBossSkills, parameter) ==
                              std::end(kBossSkills)) ||
          ((phase == 10U || phase == 11U) && parameter >= 3U))
        return rejected(CombatRejectReason::kUnrecoveredAction);
      // Animation completion also resets AI to state 2 locally (91A120/91A350),
      // without a network packet. Do not permanently suppress repeated skills.
      // A short retransmission window is server policy, not a native timer.
      const auto identity = std::pair{phase, parameter};
      if (boss->boss_phase == identity &&
          (now < boss->boss_phase_time ||
           now - boss->boss_phase_time < std::chrono::milliseconds{250}))
        return rejected(CombatRejectReason::kDuplicateAction);
      if (phase == 9U && boss->boss_phase && boss->boss_phase->first == 9U &&
          now - boss->boss_phase_time < std::chrono::seconds{4})
        return rejected(CombatRejectReason::kDuplicateAction);
      if (!combat.ConsumeActionQuota(actor->second, now))
        return rejected(CombatRejectReason::kRateLimited);
      boss->boss_phase = identity;
      boss->boss_phase_time = now;
      boss->boss_final_sequence = phase == 9U && parameter == 1U;
      if (boss->boss_final_sequence) {
        boss_final_ = true;
        // 91A2B0 ends this path with 725(1), WITHOUT 726. Five seconds is a
        // server grace period for the native final animation / lost final
        // claim.
        boss_finish_due_ = now + std::chrono::seconds{5};
      }
      if (boss->boss_final_sequence && boss->object)
        combat.actors_.at(combat.object_owners_.at(*boss->object))
            .storm_source = 0U;
      boss->teleport_due.reset();
      if (phase == 9U && parameter == 0U)
        boss->teleport_due = now + std::chrono::seconds{6};
      // 91B610 applies its state before sending. Only observers need the relay.
      // No synthetic combat kill is awarded for the final retreat animation.
      return deliver({{.opcode = packet.opcode, .body = packet.body}}, true);
    }
    const auto object = reader.ReadU32();
    const auto target = reader.ReadU32();
    static_cast<void>(reader.ReadU32());
    std::array<float, 3U> destination{};
    for (auto& value : destination) value = reader.ReadF32();
    if (boss->object != object || !boss->teleport_due ||
        now > *boss->teleport_due || now < boss->boss_phase_time)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    if (target != 0xffffffffU) {
      const auto owner = combat.object_owners_.find(target);
      if (owner == combat.object_owners_.end())
        return rejected(CombatRejectReason::kUnknownTarget);
      const auto& victim = combat.actors_.at(owner->second);
      if (victim.controller || victim.participant.team_code != 2U)
        return rejected(CombatRejectReason::kUnknownTarget);
    }
    if (!combat.ConsumeActionQuota(actor->second, now))
      return rejected(CombatRejectReason::kRateLimited);
    boss->teleport_due.reset();
    combat.actors_.at(combat.object_owners_.at(object)).position = destination;
    // Sender only marks teleport pending; 919360 applies it on all clients.
    return deliver({{.opcode = packet.opcode, .body = packet.body}});
  }
  if (packet.opcode == 32896U || packet.opcode == 32914U) {
    if (sender != host_) return rejected(CombatRejectReason::kUnknownTarget);
    const auto agent = std::ranges::find_if(agents_, [&](const auto& entry) {
      return entry.active &&
             (packet.opcode == 32914U ? entry.boss : entry.protected_object);
    });
    if (agent == agents_.end())
      return rejected(CombatRejectReason::kUnknownTarget);
    const auto value = reader.ReadF32();
    auto& previous =
        packet.opcode == 32914U ? agent->reported_hp : protected_gauge_;
    if (previous == value)
      return rejected(CombatRejectReason::kDuplicateAction);
    if (!combat.ConsumeActionQuota(actor->second, now))
      return rejected(CombatRejectReason::kRateLimited);
    previous = value;
    if (packet.opcode == 32914U) {
      boss_zero_hp_ = value <= 0.0F;
      if (!boss_zero_hp_ && !boss_defeated_ && !boss_final_)
        boss_finish_due_.reset();
    } else if (value > 0.0F) {
      guard_failure_due_.reset();
    }
    // Original controllers already apply the FLOAT. It corroborates terminal
    // claims without synthesizing damage, a combat kill, or a score increment.
    return deliver({{.opcode = packet.opcode, .body = packet.body}}, true);
  }
  if (packet.opcode == 929U) {
    // 7C4310: index, NPC kind (100 citizen, 101/102 mission zombie), reserved.
    // This packet has NO object ID; counting it would allow retransmits to
    // complete a rescue twice. Settlement uses authenticated NPC deaths
    // instead.
    const auto index = reader.ReadU32();
    const auto kind = reader.ReadU32();
    const auto reserved = reader.ReadU32();
    if (index >= rescues_.size() || index != rescue_index_ ||
        rescues_[index].state != RescueState::kActive || kind < 100U ||
        kind > 102U || reserved)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    return {.accepted = true};
  }
  if (packet.opcode == 32897U) {
    const auto object = reader.ReadU32();
    const auto target = reader.ReadU32();
    const auto agent = std::ranges::find_if(agents_, [&](const auto& entry) {
      return entry.active && entry.team == 1U && entry.object == object;
    });
    if (sender != host_ || agent == agents_.end())
      return rejected(CombatRejectReason::kUnknownTarget);
    if (agent->pursuit_target == target)
      return rejected(CombatRejectReason::kDuplicateAction);
    if (target != 0xffffffffU) {
      const auto owner = combat.object_owners_.find(target);
      if (owner == combat.object_owners_.end())
        return rejected(CombatRejectReason::kUnknownTarget);
      const auto& victim = combat.actors_.at(owner->second);
      if (victim.controller || victim.participant.team_code != 2U ||
          victim.life_state != CombatLifeState::kAlive ||
          agent->pursuit_target != 0xffffffffU)
        return rejected(CombatRejectReason::kInvalidLifecycleState);
    }
    // 915240 multiplies pursuit speed. Repeated notifications must not stack
    // it.
    agent->pursuit_target = target;
    return deliver({{.opcode = packet.opcode, .body = packet.body}}, true);
  }
  if (packet.opcode == 925U || packet.opcode == 926U) {
    const auto value = reader.ReadU32();
    const auto kills = actor->second.performance.pangs;
    if (packet.opcode == 925U) {
      ObserveScoreUnits(combat);
      const auto confirmed = ScoreUnits(sender);
      auto& counts = reported_units_[sender];
      if (value >= counts.size() || counts[value] >= confirmed[value])
        return rejected(CombatRejectReason::kInvalidLifecycleState);
      ++counts[value];
    } else {
      if (value == 0U || value > kills || reported_combo_[sender] >= value)
        return rejected(CombatRejectReason::kInvalidLifecycleState);
      reported_combo_[sender] = value;
    }
    // 925 indexes serverPangScoreUnitnum; 926 reports a combo. The original
    // server reward coefficients are unavailable. Keep validated observations
    // separate from the authoritative combat totals; neither opcode is echoed.
    return {.accepted = true};
  }
  if (packet.opcode == 923U) {
    const auto slot = reader.ReadU32();
    const auto& player = actor->second;
    std::erase_if(skill_uses_, [&](const auto& use) {
      return now >= use.expires ||
             combat.LifeGeneration(use.player) != use.life;
    });
    if (slot >= 5U || player.life_state != CombatLifeState::kAlive ||
        player.participant.team_code != 2U ||
        !items_[sender][kSkillTypes[slot]] || now < skill_due_[sender][slot] ||
        skill_uses_.size() >= 128U)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    const auto type = kSkillTypes[slot];
    --items_[sender][type];
    skill_due_[sender][slot] =
        now + std::chrono::milliseconds{kSkillCooldowns[slot]};
    skill_uses_.push_back(
        {sender, type, player.life_generation, now + std::chrono::seconds{10}});
    combat.AuthorizeItemEffects(sender, type, 16U, now);
    protocol::PacketWriter effect;
    effect.WriteU32(player.participant.seat_index);
    effect.WriteU32(type);
    effect.WriteU32(slot);
    // Client 581510 already decremented its skill count when sending 923.
    // 715 executes locally without clearing an unrelated ordinary item slot.
    return deliver({{.opcode = 715U, .body = effect.Release()}});
  }
  if (packet.opcode == 729U) {
    const auto drop = protocol::DecodePveItemDrop(packet.body);
    // ItemSpecZombiePve: only these enabled entries have positive drop weight.
    constexpr std::uint32_t types[]{17U, 101U, 136U, 146U, 229U};
    if (std::ranges::find(types, drop.type) == std::end(types) ||
        (drop.team != 0U && drop.team != 2U) ||
        (drop.lifetime_ms != 10'000U && drop.lifetime_ms != 20'000U) ||
        !std::ranges::all_of(drop.position,
                             [](float v) {
                               return std::isfinite(v) && std::abs(v) <= 1.0e7F;
                             }) ||
        !std::ranges::all_of(drop.velocity, [](float v) {
          return std::isfinite(v) && std::abs(v) <= 1.0e4F;
        }))
      return rejected(CombatRejectReason::kUnrecoveredAction);
    if (sender != host_) {
      const auto& player = actor->second;
      // Original850950 is executed by the local child zombie and emits five
      // 7CC3D0/729 boxes. A guest needs its own confirmed zombie death;
      // being a room member alone must not authorize map-script drops.
      if (!infected_.contains(sender) || player.controller ||
          player.life_state != CombatLifeState::kDefeated || !player.defeated ||
          player.defeated->life != player.life_generation ||
          now < player.defeated->time ||
          now >= player.defeated->time + std::chrono::seconds{20})
        return {};
      auto& permit = infected_drops_[sender];
      if (permit.life != player.life_generation)
        permit = {.life = player.life_generation};
      if (permit.requests.size() >= 5U || permit.requests.contains(packet.body))
        return {};
    }
    if (std::ranges::any_of(drops_, [&](const auto& entry) {
          return entry.request == packet.body;
        }))
      return rejected(CombatRejectReason::kDuplicateAction);
    if (now >= drop_window_ + std::chrono::seconds{1}) {
      drop_window_ = now;
      drops_in_window_ = 0U;
    }
    if (drops_.size() >= 128U || drops_in_window_ >= 32U)
      return rejected(CombatRejectReason::kRateLimited);
    ++drops_in_window_;
    if (sender != host_)
      infected_drops_.at(sender).requests.insert(packet.body);
    const auto object = ++drop_serial_;
    drops_.push_back({object, drop.type, drop.team, drop.position,
                      now + std::chrono::milliseconds{drop.lifetime_ms},
                      packet.body});
    protocol::PacketWriter response;
    response.WriteU32(object);
    // Native 7D1DA0 rejects every ordinary 711 box unless class == 20000.
    // Keep the actual PVE skill type in the visual/pickup field at offset 20.
    response.WriteU32(20'000U);
    for (auto v : drop.position) response.WriteF32(v);
    response.WriteU32(drop.type);
    for (auto v : drop.velocity) response.WriteF32(v);
    response.WriteU8(drop.team);
    return deliver({{.opcode = 711U, .body = response.Release()}});
  }
  if (packet.opcode == 707U) {
    const auto object = reader.ReadU32();
    const auto type = reader.ReadU32();
    const auto found = std::ranges::find(drops_, object, &Drop::object);
    const auto& player = actor->second;
    if (found == drops_.end() || type != found->type || now >= found->expires ||
        player.life_state != CombatLifeState::kAlive ||
        player.participant.team_code != 2U ||
        (found->team != 0U && found->team != player.participant.team_code))
      return rejected(CombatRejectReason::kUnknownTarget);
    // The original client reports collision after local box physics. Neither a
    // stale player sample nor the initial drop position describes that
    // collision.
    if (items_[sender][type] >= 32U)
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    ++items_[sender][type];
    protocol::PacketWriter removed, acquired;
    removed.WriteU32(player.participant.seat_index);
    removed.WriteU32(object);
    acquired.WriteU32(player.participant.seat_index);
    acquired.WriteU32(type);
    acquired.WriteU32(0U);
    // 7D0390 mode16 -> 581820 increments the native PVE item inventory.
    // Unlike pirate ammo, these are selectable skills, not immediate 715
    // effects.
    drops_.erase(found);
    return deliver({{.opcode = 712U, .body = removed.Release()},
                    {.opcode = 713U, .body = acquired.Release()}});
  }
  if (packet.opcode == 32894U) {
    const auto object = reader.ReadU32();
    const auto type = reader.ReadU32();
    std::erase_if(skill_uses_, [&](const auto& use) {
      return now >= use.expires ||
             combat.LifeGeneration(use.player) != use.life;
    });
    const auto use = std::ranges::find_if(skill_uses_, [&](const auto& entry) {
      return entry.player == sender && entry.type == type;
    });
    if (object != actor->second.participant.actor_object_id ||
        actor->second.life_state != CombatLifeState::kAlive ||
        actor->second.participant.team_code != 2U || use == skill_uses_.end())
      return rejected(CombatRejectReason::kInvalidLifecycleState);
    skill_uses_.erase(use);
    return deliver({{.opcode = packet.opcode, .body = packet.body}}, true);
  }
  return rejected(CombatRejectReason::kUnrecoveredAction);
}

std::optional<std::chrono::steady_clock::time_point> ZombiePveMatch::Deadline()
    const {
  if (winner_) return std::nullopt;
  std::optional<std::chrono::steady_clock::time_point> due;
  if (!drops_.empty())
    due = std::ranges::min(drops_, {}, &Drop::expires).expires;
  for (const auto& [object, batch] : deferred_actions_) {
    static_cast<void>(object);
    if (!due || batch.expires_at < *due) due = batch.expires_at;
  }
  for (const auto terminal : {boss_finish_due_, guard_failure_due_}) {
    if (terminal && (!due || *terminal < *due)) due = terminal;
  }
  if (rescue_started_ && rescue_index_ < rescues_.size()) {
    auto rescue_due = *rescue_started_ +
                      std::chrono::milliseconds{RescueStartMs(rescue_index_)};
    if (rescues_[rescue_index_].state == RescueState::kActive)
      rescue_due += std::chrono::milliseconds{kRescueDurationMs};
    if (!due || rescue_due < *due) due = rescue_due;
  }
  return due;
}

void ZombiePveMatch::ObserveTerminalState(const CombatRelay& combat) {
  for (const auto& agent : agents_) {
    if (!agent.active || !agent.object ||
        (!agent.boss && !agent.protected_object))
      continue;
    const auto owner = combat.object_owners_.find(*agent.object);
    if (owner != combat.object_owners_.end() &&
        combat.actors_.at(owner->second).life_state ==
            CombatLifeState::kDefeated) {
      boss_defeated_ |= agent.boss;
      protected_defeated_ |= agent.protected_object;
    }
  }
}

void ZombiePveMatch::ObserveRescueDeaths(const CombatRelay& combat) {
  const auto dead = [&](std::uint32_t object) {
    const auto owner = combat.object_owners_.find(object);
    return owner != combat.object_owners_.end() &&
           combat.actors_.at(owner->second).life_state ==
               CombatLifeState::kDefeated;
  };
  for (auto& rescue : rescues_) {
    if (rescue.state != RescueState::kActive) continue;
    for (auto& [object, defeated] : rescue.defeated) defeated |= dead(object);
    if (rescue.citizen_object)
      rescue.citizen_dead |= dead(*rescue.citizen_object);
  }
}

std::vector<OutboundMessage> ZombiePveMatch::Tick(
    CombatRelay& combat, std::chrono::steady_clock::time_point now) {
  std::vector<OutboundMessage> messages;
  PruneDeferredActions(now);
  if (winner_) return messages;
  ObserveScoreUnits(combat);
  ObserveTerminalState(combat);
  const bool boss_finished =
      boss_final_ || boss_defeated_ || (boss_zero_hp_ && boss_claim_);
  const bool guard_failed = protected_gauge_ && *protected_gauge_ <= 0.0F;
  if (boss_finished && !boss_finish_due_)
    boss_finish_due_ = now + std::chrono::seconds{5};
  if (guard_failed && !guard_failure_due_)
    guard_failure_due_ = now + std::chrono::seconds{5};
  // Claims may precede the last HP packet: native 919CE0 runs inside damage
  // processing, before 919A10 sends 32914. Retain them for this encounter only.
  if ((failure_claim_ && (guard_failed || protected_defeated_)) ||
      (guard_failure_due_ && now >= *guard_failure_due_))
    winner_ = 1U;
  else if ((success_claim_ && boss_finished) ||
           (boss_finish_due_ && now >= *boss_finish_due_))
    winner_ = 2U;
  else if (std::ranges::any_of(
               combat.actors_,
               [](const auto& item) { return !item.second.controller; }) &&
           std::ranges::none_of(combat.actors_, [](const auto& item) {
             return !item.second.controller &&
                    item.second.participant.team_code == 2U;
           }))
    winner_ = 1U;
  if (winner_) return messages;
  if (!rescue_started_) rescue_started_ = now;
  ObserveRescueDeaths(combat);
  if (rescue_index_ < rescues_.size()) {
    auto& rescue = rescues_[rescue_index_];
    const auto start = *rescue_started_ +
                       std::chrono::milliseconds{RescueStartMs(rescue_index_)};
    const auto end = start + std::chrono::milliseconds{kRescueDurationMs};
    if (rescue.state == RescueState::kWaiting && now >= start) {
      rescue.state = RescueState::kActive;
      protocol::PacketWriter writer;
      writer.WriteU32(rescue_index_);
      messages.push_back({.opcode = 927U, .body = writer.Release()});
    }
    if (rescue.state == RescueState::kActive) {
      const bool success =
          now < end && !rescue.citizen_dead && rescue.citizen_object &&
          rescue.defeated.size() == rescue.enemies &&
          std::ranges::all_of(
              rescue.defeated, [](const auto& entry) { return entry.second; });
      if (success || now >= end || rescue.citizen_dead) {
        rescue.state = success ? RescueState::kSucceeded : RescueState::kFailed;
        protocol::PacketWriter writer;
        if (success) {
          writer.WriteU32(
              0U);  // S2C929: result, completed index (NOT C2S layout).
          writer.WriteU32(rescue_index_);
        } else {
          writer.WriteU32(rescue_index_ +
                          1U);  // 928 cleans current NPC before advancing.
        }
        messages.push_back(
            {.opcode = static_cast<std::uint16_t>(success ? 929U : 928U),
             .body = writer.Release()});
        if (success) {
          // Native 713 -> mode16 skill inventory, also reflected in server 923
          // ledger. One reward per human, once per task. No persistent
          // wallet/inventory grant.
          for (const auto& [id, actor] : combat.actors_) {
            if (actor.controller || actor.participant.team_code != 2U) continue;
            const auto type = kSkillTypes[(rescue.citizen + rescue_index_ +
                                           actor.participant.seat_index) %
                                          5U];
            ++items_[id][type];
            protocol::PacketWriter award;
            award.WriteU32(actor.participant.seat_index);
            award.WriteU32(type);
            award.WriteU32(0U);
            messages.push_back({.opcode = 713U, .body = award.Release()});
          }
        }
        ++rescue_index_;
      }
    }
  }
  std::erase_if(drops_, [&](const auto& drop) {
    if (now < drop.expires) return false;
    protocol::PacketWriter removed;
    removed.WriteU32(0xffffffffU);  // 7CDAC0: remove without collector effects.
    removed.WriteU32(drop.object);
    messages.push_back({.opcode = 712U, .body = removed.Release()});
    return true;
  });
  for (auto& [id, actor] : combat.actors_) {
    if (actor.controller || actor.life_state != CombatLifeState::kDefeated ||
        actor.death_stage != 2U)
      continue;
    const auto previous = authorized_.find(id);
    if (previous != authorized_.end() &&
        previous->second == actor.life_generation)
      continue;
    authorized_[id] = actor.life_generation;
    // Native 32836 -> ModeMgr event100 -> A9A980/A8C500 consumes one LP.
    // Empty C2S927 is a separate fatal-hit report and never authorizes a life.
    auto& remaining = remaining_lives_.at(id);
    if (remaining > 0U) --remaining;
    actor.participant.ordinary_respawn_allowed =
        remaining > 0U && !infected_.contains(id);
    protocol::PacketWriter writer;
    writer.WriteU32(actor.participant.actor_object_id);
    messages.push_back({.opcode = 730U, .body = writer.Release()});
  }
  return messages;
}

std::vector<OutboundMessage> ZombiePveMatch::Transition(domain::PlayerId player,
                                                        CombatRelay& combat) {
  const auto found = combat.actors_.find(player);
  if (winner_ || found == combat.actors_.end() || found->second.controller)
    return {};
  auto& actor = found->second;
  if (!remaining_lives_.contains(player) || remaining_lives_.at(player) != 0U ||
      !authorized_.contains(player) ||
      authorized_.at(player) != actor.life_generation ||
      actor.life_state != CombatLifeState::kDefeated || actor.respawn_point)
    return {};
  authorized_.erase(player);
  old_weapons_[player] = actor.participant.weapon_templates;
  infected_.insert(player);
  actor.participant.team_code = 1U;
  actor.participant.weapon_templates = {};
  actor.participant.weapon_templates[2U] = 7201U;
  actor.participant.ordinary_respawn_allowed = false;
  actor.life_state = CombatLifeState::kAlive;
  actor.death_stage = 0U;
  actor.life_settled = false;
  actor.last_fire.reset();
  actor.pending_kick.reset();
  actor.active_kick.reset();
  actor.protection_report.clear();
  ++actor.life_generation;
  const auto object = actor.participant.actor_object_id;
  std::erase_if(combat.projectiles_, [object](const auto& item) {
    return item.second.owner_object_id == object;
  });
  std::erase_if(combat.weapon_objects_, [object](const auto& item) {
    return item.second.owner_object_id == object;
  });
  if (std::ranges::none_of(combat.actors_, [&](const auto& item) {
        return !item.second.controller && !infected_.contains(item.first);
      }))
    winner_ = 1U;
  return {{.opcode = 504U,
           .body = protocol::EncodeInfectionTeamResponse(
               16U, actor.participant.seat_index, 35U)}};
}

void ZombiePveMatch::Claim(domain::PlayerId sender,
                           const protocol::Packet& packet) {
  protocol::ValidateSpecialModeClaim(packet.opcode, packet.body);
  if (sender != host_ || winner_) return;
  if (packet.opcode == 726U) {
    boss_claim_ = boss_spawned_;
  } else {
    const auto value =
        protocol::DecodeSpecialModeFinishClaim(packet.body).value;
    if (value == 1U && boss_spawned_) success_claim_ = true;
    if (value == 0U && protected_spawned_) failure_claim_ = true;
  }
}

std::uint8_t ZombiePveMatch::Winner(bool timeout) const noexcept {
  return winner_ ? winner_ : timeout ? 1U : 0U;
}

std::uint8_t ZombiePveMatch::Team(domain::PlayerId player) const noexcept {
  return infected_.contains(player) ? 35U : 2U;
}
}  // namespace bf::runtime
