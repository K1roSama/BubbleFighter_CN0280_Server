#include "features/battle/combat_relay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/battle/native_projectile_counts.h"
#include "features/characters/hotblood_stats.h"
#include "features/practice/training_room.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {

constexpr std::size_t kMaximumActionsPerSecond = 180U;
constexpr std::size_t kMaximumProjectiles = 8192U;
constexpr std::size_t kMaximumDefeatReports = 8192U;
constexpr auto kProjectileRetention = std::chrono::seconds{30};
constexpr std::uint32_t kWorldTarget = 0U;
constexpr std::uint32_t kNoTarget = 0xFFFFFFFFU;

[[nodiscard]] application::ServiceError InvalidActor() {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidState,
      .safe_message = "实时消息发送者不在当前比赛"};
}

[[noreturn]] void ProtocolViolation(std::string_view message) {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                message};
}

void SaturatedIncrement(std::uint32_t& value) noexcept {
  if (value != std::numeric_limits<std::uint32_t>::max()) {
    ++value;
  }
}

}  // namespace

std::optional<std::uint32_t> CombatRelay::EquippedTemplate(
    const ActorState& actor, std::uint32_t bar) noexcept {
  if (bar >= actor.participant.weapon_templates.size()) return std::nullopt;
  if (bar == 4U && actor.giant_weapon &&
      actor.giant_weapon->life == actor.life_generation &&
      actor.life_state == CombatLifeState::kAlive)
    return 9001U;
  if (bar == 4U && actor.bazooka &&
      actor.bazooka->first == actor.life_generation &&
      actor.life_state == CombatLifeState::kAlive)
    return actor.bazooka_template;
  return actor.participant.weapon_templates[bar];
}

bool CombatRelay::CanChangePreset(domain::PlayerId player) const noexcept {
  const auto found = actors_.find(player);
  return found != actors_.end() && !found->second.controller.has_value() &&
         found->second.life_state == CombatLifeState::kDefeated &&
         !found->second.respawn_point.has_value();
}

bool CombatRelay::CanInstantRespawn(domain::PlayerId player,
                                    std::uint32_t object) const noexcept {
  const auto found = actors_.find(player);
  return CanChangePreset(player) &&
         found->second.participant.actor_object_id == object &&
         found->second.participant.ordinary_respawn_allowed &&
         found->second.instant_respawn_life != found->second.life_generation;
}

bool CombatRelay::AuthorizeInstantRespawn(domain::PlayerId player,
                                          std::uint64_t life) noexcept {
  const auto found = actors_.find(player);
  if (found == actors_.end() || found->second.life_generation != life ||
      !CanInstantRespawn(player, found->second.participant.actor_object_id)) {
    return false;
  }
  found->second.instant_respawn_life = life;
  return true;
}

bool CombatRelay::ChangePreset(
    domain::PlayerId player,
    const std::array<std::optional<std::uint32_t>, 5U>& weapons,
    std::optional<std::uint32_t> fighter,
    std::vector<std::uint32_t> equipment) {
  if (!CanChangePreset(player)) {
    return false;
  }
  auto& actor = actors_.at(player);
  actor.participant.weapon_templates = weapons;
  if (fighter) {
    actor.participant.fighter = *fighter;
    actor.participant.equipment_items = std::move(equipment);
  }
  actor.last_fire.reset();
  return true;
}

CombatRelay::CombatRelay(const std::vector<CombatParticipant>& participants,
                         bool individual_scoring,
                         const std::vector<domain::PlayerId>& observers,
                         std::uint32_t mode_id, bool grenades_forbidden)
    : individual_scoring_(individual_scoring),
      mode_id_(mode_id),
      grenades_forbidden_(grenades_forbidden) {
  if (participants.empty()) {
    throw std::invalid_argument{
        "combat relay requires at least one participant"};
  }
  for (auto& participant : participants) {
    if (!participant.player_id.IsValid() || participant.actor_object_id == 0U ||
        ((individual_scoring && (mode_id == 13U || mode_id == 23U))
             ? (participant.team_code < 1U || participant.team_code > 16U)
             : (participant.team_code != 1U && participant.team_code != 2U)) ||
        !object_ids_.insert(participant.actor_object_id).second) {
      throw std::invalid_argument{
          "combat participant identity is invalid or duplicated"};
    }
    const auto player_id = participant.player_id;
    const auto inserted = actors_.emplace(
        player_id, ActorState{.participant = participant,
                              .rate_window = std::nullopt,
                              .messages_in_window = 0U,
                              .position = std::nullopt,
                              .last_fire = std::nullopt,
                              .accepted_shots = 0U,
                              .life_state = CombatLifeState::kAlive,
                              .performance = {},
                              .life_settled = false,
                              .death_stage = 0U,
                              .respawn_point = std::nullopt,
                              .protection_report = {},
                              .defeat_reports = {},
                              .life_generation = 0U,
                              .pending_kick = std::nullopt,
                              .active_kick = std::nullopt});
    if (!inserted.second) {
      throw std::invalid_argument{"combat participant player is duplicated"};
    }
    if ((mode_id_ == 23U || mode_id_ == 25U) &&
        participant.weapon_templates[4U] == 5920U)
      inserted.first->second.survival_medkits = 1U;
    inserted.first->second.initial_weapons = participant.weapon_templates;
    object_owners_.emplace(participant.actor_object_id, player_id);
  }
  for (const auto observer : observers) {
    if (!observer.IsValid() || actors_.contains(observer) ||
        !observers_.insert(observer).second)
      throw std::invalid_argument{
          "combat observer identity is invalid or duplicated"};
  }
}

void CombatRelay::SetHost(domain::PlayerId player_id) noexcept {
  host_ = actors_.contains(player_id) || observers_.contains(player_id)
              ? std::optional{player_id}
              : std::nullopt;
}

void CombatRelay::EnableMovementTickAggregation(bool enabled) noexcept {
  movement_tick_aggregation_enabled_ = enabled;
  if (!enabled) pending_movements_.clear();
}

std::size_t CombatRelay::PendingMovementCount() const noexcept {
  return pending_movements_.size();
}

std::vector<CombatMovementDelivery> CombatRelay::FlushMovementTick() {
  std::vector<CombatMovementDelivery> result;
  if (!movement_tick_aggregation_enabled_ || pending_movements_.empty())
    return result;

  std::size_t recipients_per_sender = 0U;
  for (const auto& [player_id, actor] : actors_) {
    static_cast<void>(player_id);
    if (!actor.controller) ++recipients_per_sender;
  }
  recipients_per_sender += observers_.size();
  result.reserve(pending_movements_.size() * recipients_per_sender);
  for (const auto& [sender, message] : pending_movements_) {
    for (const auto& [player_id, actor] : actors_) {
      if (player_id != sender && !actor.controller) {
        result.push_back(
            {.sender = sender, .recipient = player_id, .message = message});
      }
    }
    for (const auto observer : observers_) {
      if (observer != sender)
        result.push_back(
            {.sender = sender, .recipient = observer, .message = message});
    }
  }
  pending_movements_.clear();
  return result;
}

application::ServiceResult<CombatRelayResult> CombatRelay::Handle(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  auto found = actors_.find(actor);
  if (found == actors_.end()) {
    return InvalidActor();
  }
  for (auto& [id, member] : actors_) {
    static_cast<void>(id);
    if (member.giant_weapon &&
        (member.giant_weapon->life != member.life_generation ||
         now >= member.giant_weapon->expires))
      member.giant_weapon.reset();
  }
  if (found->second.racing_stage &&
      !protocol::IsNativeRespawnOpcode(packet.opcode)) {
    const auto& stage = *found->second.racing_stage;
    if (!stage.begins || !stage.expires || now < *stage.begins ||
        now >= *stage.expires || stage.needs_reset) {
      protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
      return RejectedResult(CombatRejectReason::kInactiveMatchPhase);
    }
  }
  if (auto turret = HandleTurret(actor, packet, now))
    return FinishNativeConstructorAttempt(actor, packet, *turret);
  if (protocol::IsNativeStormOpcode(packet.opcode) ||
      (packet.opcode == 33282U &&
       storms_.contains(
           protocol::DecodeNativeWeaponObject(packet.opcode, packet.body)
               .cookie))) {
    return HandleStorm(actor, packet, now);
  }
  if (protocol::IsNativeRescueOpcode(packet.opcode)) {
    return HandleRescue(actor, packet, now);
  }
  if (packet.body.size() >= 4U) {
    std::uint32_t object = 0U;
    if (protocol::IsNativeActionOpcode(packet.opcode)) {
      object = protocol::DecodeNativeAction(packet.opcode, packet.body)
                   .actor_object_id;
    } else if (protocol::IsNativeLifecycleOpcode(packet.opcode)) {
      object = protocol::DecodeNativeLifecycle(packet.opcode, packet.body)
                   .actor_object_id;
    } else if (protocol::IsNativeHitOpcode(packet.opcode)) {
      const auto request =
          protocol::DecodeNativeHit(packet.opcode, packet.body);
      const auto shot = projectiles_.find(request.projectile_cookie);
      if (shot != projectiles_.end()) {
        object = shot->second.owner_object_id;
      }
    } else if (protocol::IsNativeWeaponObjectOpcode(packet.opcode)) {
      const auto request =
          protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
      object = request.actor_object_id;
      const auto& ledger = (packet.opcode == 32900U ||
                            packet.opcode == 32774U || packet.opcode == 32818U)
                               ? projectiles_
                               : weapon_objects_;
      const auto shot = ledger.find(
          packet.opcode == 32900U ? request.source_cookie : request.cookie);
      if (object == 0U && shot != ledger.end()) {
        object = shot->second.owner_object_id;
      }
    }
    const auto owner = object_owners_.find(object);
    if (owner != object_owners_.end() &&
        actors_.at(owner->second).controller == actor) {
      found = actors_.find(owner->second);
    }
  }
  if (protocol::IsNativeWeaponObjectOpcode(packet.opcode)) {
    try {
      return FinishNativeConstructorAttempt(
          found->first, packet, HandleWeaponObject(found->second, packet, now));
    } catch (...) {
      MarkNativeConstructorGap(found->first, packet.opcode);
      throw;
    }
  }
  if (protocol::IsNativeActionOpcode(packet.opcode)) {
    try {
      return FinishNativeConstructorAttempt(
          found->first, packet, HandleAction(found->second, packet, now));
    } catch (...) {
      MarkNativeConstructorGap(found->first, packet.opcode);
      throw;
    }
  }
  if (protocol::IsNativeHitOpcode(packet.opcode)) {
    return HandleHit(found->second, packet, now);
  }
  if (protocol::IsNativeLifecycleOpcode(packet.opcode)) {
    return HandleLifecycle(found->second, packet, now);
  }
  if (protocol::IsNativeRespawnOpcode(packet.opcode)) {
    return HandleRespawn(found->second, packet, now);
  }
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = "当前战斗转发器不处理该 opcode"};
}

void CombatRelay::Reset() noexcept {
  pending_movements_.clear();
  ResetTurrets();
  native_constructor_ordinals_.clear();
  native_loopback_gaps_.clear();
  item_effects_.clear();
  rescues_.clear();
  projectiles_.clear();
  weapon_objects_.clear();
  survival_traps_.clear();
  storms_.clear();
  storm_serials_.clear();
  for (auto& [player_id, actor] : actors_) {
    static_cast<void>(player_id);
    actor.last_smart_ping.reset();
    actor.rate_window.reset();
    actor.messages_in_window = 0U;
    actor.position.reset();
    actor.last_fire.reset();
    actor.last_storm_time.reset();
    actor.storm_reaction.reset();
    actor.area_items.clear();
    actor.area_reactions.clear();
    actor.pending_area_cast.reset();
    actor.bazooka.reset();
    actor.giant_weapon.reset();
    actor.airstrikes.clear();
    actor.captain_weapon.reset();
    actor.captain_blast.reset();
    actor.deathmatch_fever = {};
    actor.accepted_shots = 0U;
    actor.life_state = CombatLifeState::kAlive;
    actor.performance = {};
    actor.life_settled = false;
    actor.death_stage = 0U;
    actor.respawn_point.reset();
    actor.racing_stage.reset();
    actor.protection_report.clear();
    actor.defeat_reports.clear();
    actor.life_generation = 0U;
    ++actor.item_drop_event;
    actor.item_drop_at.reset();
    ++actor.item_inventory_generation;
    actor.item_escape.reset();
    actor.instant_respawn_life.reset();
    actor.respawn_wait.reset();
    actor.health_report.reset();
    actor.survival_alliance_pose = false;
    actor.survival_melee.reset();
    actor.selected_weapon_bar = 2U;
    if (mode_id_ == 23U || mode_id_ == 25U)
      actor.participant.weapon_templates = actor.initial_weapons;
    actor.survival_primary_weapons.clear();
    actor.survival_supplies.clear();
    actor.survival_medkits =
        (mode_id_ == 23U || mode_id_ == 25U) &&
                actor.participant.weapon_templates[4U] == 5920U
            ? 1U
            : 0U;
    actor.survival_treatment_start.reset();
    actor.survival_heals.clear();
    actor.survival_used_effects.clear();
    actor.survival_water_report.reset();
    actor.pending_kick.reset();
    actor.bubble_impacts.clear();
    actor.active_kick.reset();
    actor.humiliations.clear();
    actor.humiliated.reset();
    actor.defeated.reset();
  }
}

std::size_t CombatRelay::ProjectileCount() const noexcept {
  return projectiles_.size();
}

void CombatRelay::ResetRound() noexcept {
  pending_movements_.clear();
  ResetTurrets();
  item_effects_.clear();
  rescues_.clear();
  projectiles_.clear();
  weapon_objects_.clear();
  survival_traps_.clear();
  storms_.clear();
  storm_serials_.clear();
  for (auto& [player, actor] : actors_) {
    static_cast<void>(player);
    actor.last_smart_ping.reset();
    actor.rate_window.reset();
    actor.messages_in_window = 0U;
    actor.position.reset();
    actor.last_fire.reset();
    actor.last_storm_time.reset();
    actor.storm_reaction.reset();
    actor.area_items.clear();
    actor.area_reactions.clear();
    actor.pending_area_cast.reset();
    actor.bazooka.reset();
    actor.giant_weapon.reset();
    actor.airstrikes.clear();
    actor.captain_weapon.reset();
    actor.captain_blast.reset();
    actor.deathmatch_fever = {};
    actor.life_state = CombatLifeState::kAlive;
    ++actor.life_generation;
    ++actor.item_drop_event;
    actor.item_drop_at.reset();
    ++actor.item_inventory_generation;
    actor.item_escape.reset();
    actor.survival_alliance_pose = false;
    actor.survival_melee.reset();
    actor.selected_weapon_bar = 2U;
    actor.survival_treatment_start.reset();
    actor.survival_heals.clear();
    actor.instant_respawn_life.reset();
    actor.respawn_wait.reset();
    actor.life_settled = false;
    actor.death_stage = 0U;
    actor.respawn_point.reset();
    actor.racing_stage.reset();
    actor.protection_report.clear();
    actor.pending_kick.reset();
    actor.bubble_impacts.clear();
    actor.active_kick.reset();
    actor.humiliations.clear();
    actor.humiliated.reset();
    actor.defeated.reset();
  }
}

std::optional<std::uint64_t> CombatRelay::LifeGeneration(
    domain::PlayerId player_id) const {
  const auto found = actors_.find(player_id);
  return found == actors_.end() ? std::nullopt
                                : std::optional{found->second.life_generation};
}

std::optional<std::uint64_t> CombatRelay::AcceptedShots(
    domain::PlayerId player_id) const {
  const auto found = actors_.find(player_id);
  return found == actors_.end()
             ? std::nullopt
             : std::optional<std::uint64_t>{found->second.accepted_shots};
}

std::optional<std::array<float, 3U>> CombatRelay::LastPosition(
    domain::PlayerId player_id) const {
  const auto found = actors_.find(player_id);
  return found == actors_.end() ? std::nullopt : found->second.position;
}

std::optional<CombatLifeState> CombatRelay::LifeState(
    domain::PlayerId player_id) const {
  const auto found = actors_.find(player_id);
  return found == actors_.end()
             ? std::nullopt
             : std::optional<CombatLifeState>{found->second.life_state};
}

std::optional<domain::MatchPerformance> CombatRelay::Performance(
    domain::PlayerId player_id) const {
  const auto found = actors_.find(player_id);
  return found == actors_.end() ? std::nullopt
                                : std::optional<domain::MatchPerformance>{
                                      found->second.performance};
}

std::optional<domain::MatchPerformance> CombatRelay::RemoveParticipant(
    domain::PlayerId player_id) noexcept {
  observers_.erase(player_id);
  if (host_ == player_id) host_.reset();
  const auto found = actors_.find(player_id);
  if (found == actors_.end()) {
    return std::nullopt;
  }
  const auto object_id = found->second.participant.actor_object_id;
  const auto result = found->second.performance;
  std::erase_if(pending_turrets_, [player_id](const auto& pending) {
    return pending.owner == player_id;
  });
  for (auto& [id, turret] : turrets_) {
    if (turret.owner == player_id) {
      turret.ended = true;
      const auto synthetic = object_owners_.find(id);
      if (synthetic != object_owners_.end()) {
        actors_.erase(synthetic->second);
        object_owners_.erase(synthetic);
        object_ids_.erase(id);
      }
      std::erase_if(projectiles_, [id](const auto& p) {
        return p.second.owner_object_id == id;
      });
    }
  }
  for (auto& [id, participant] : actors_) {
    static_cast<void>(id);
    participant.humiliations.erase(object_id);
    if (participant.survival_melee &&
        participant.survival_melee->victim == object_id)
      participant.survival_melee.reset();
  }
  std::erase_if(rescues_, [object_id](const auto& entry) {
    return entry.first == object_id || entry.second.helper == object_id;
  });
  std::erase_if(storms_, [object_id](const auto& entry) {
    return entry.second.source == object_id;
  });
  for (auto& [cookie, storm] : storms_) {
    static_cast<void>(cookie);
    storm.victims.erase(player_id);
  }
  actors_.erase(found);
  pending_movements_.erase(player_id);
  object_ids_.erase(object_id);
  object_owners_.erase(object_id);
  std::erase_if(projectiles_, [object_id](const auto& entry) {
    return entry.second.owner_object_id == object_id;
  });
  std::erase_if(weapon_objects_, [object_id](const auto& entry) {
    return entry.second.owner_object_id == object_id;
  });
  return result;
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleAction(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  if (actor.soccer_ball &&
      (request.actor_object_id != actor.participant.actor_object_id ||
       (packet.opcode != 32769U && packet.opcode != 32885U &&
        packet.opcode != 32796U)))
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  if (request.kind == protocol::NativeActionKind::kOpaqueReport) {
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  }
  if (request.actor_object_id != actor.participant.actor_object_id) {
    // 原客户端房主预测其他成员的移动和基础动作；校验目标冻结配装后忽略，不转发或记账。
    const bool predicted = packet.opcode == 32769U || packet.opcode == 32779U ||
                           packet.opcode == 32780U || packet.opcode == 32781U ||
                           packet.opcode == 32782U || packet.opcode == 32785U ||
                           packet.opcode == 32810U || packet.opcode == 32811U ||
                           packet.opcode == 32813U || packet.opcode == 32885U ||
                           packet.opcode == 32925U || packet.opcode == 32788U ||
                           packet.opcode == 32789U || packet.opcode == 32790U ||
                           packet.opcode == 32786U || packet.opcode == 32800U ||
                           packet.opcode == 32809U || packet.opcode == 32821U ||
                           packet.opcode == 32834U || packet.opcode == 32784U ||
                           packet.opcode == 32812U || packet.opcode == 32882U ||
                           packet.opcode == 32830U || packet.opcode == 32833U;
    const auto target = object_owners_.find(request.actor_object_id);
    if (predicted && host_ == actor.participant.player_id &&
        target != object_owners_.end()) {
      const auto& predicted_actor = actors_.at(target->second);
      if (request.weapon_switch.has_value() &&
          !CanEquip(predicted_actor, request.weapon_switch->bar_index,
                    request.weapon_switch->template_id)) {
        ProtocolViolation(
            "predicted weapon switch does not match target loadout");
      }
      if (request.compressed_fire.has_value() &&
          !EquippedTemplate(predicted_actor, request.compressed_fire->bar_index)
               .has_value()) {
        ProtocolViolation(
            "predicted fire uses an unequipped target weapon bar");
      }
      return RejectedResult(CombatRejectReason::kUnrecoveredAction);
    }
    ProtocolViolation("native action belongs to another character");
  }
  if (!actor.controller && mode_id_ == 23U &&
      actor.life_state == CombatLifeState::kDefeated &&
      actor.death_stage == 2U && request.position.has_value()) {
    // 980 retires the native character after death animation completion.
    // Keep late stop-shoot/cleanup messages, but do not move its hidden body
    // or change the authoritative position used by the death backpack.
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  if (grenades_forbidden_ && !actor.controller) {
    const bool select = request.weapon_switch &&
                        request.weapon_switch->select &&
                        request.weapon_switch->bar_index == 3U;
    const bool fire =
        request.compressed_fire && request.compressed_fire->bar_index == 3U;
    const bool hold =
        request.kind == protocol::NativeActionKind::kHoldGrenade &&
        packet.body[4U] == 1U;
    const bool explicit_grenade =
        request.explicit_fire &&
        EquippedTemplate(actor, 3U) == request.explicit_fire->weapon;
    if (select || fire || hold || explicit_grenade)
      return RejectedResult(CombatRejectReason::kModeWeaponRestricted);
  }
  if (mode_id_ == 21U && request.explicit_fire) {
    const auto& fire = *request.explicit_fire;
    if (actor.controller || actor.life_state != CombatLifeState::kAlive ||
        !actor.runner_throw ||
        actor.runner_throw->life != actor.life_generation ||
        actor.runner_throw->weapon != fire.weapon ||
        now >= actor.runner_throw->expires ||
        now < actor.runner_throw->expires - std::chrono::seconds{10} ||
        fire.variant != actor.participant.team_code ||
        fire.target != kNoTarget || fire.flags != 0U || !actor.position)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // Native927F40/928080 uses the character hand point and normalized
    // direction. Fifty units are only server transport/hand-offset correlation
    // tolerance.
    for (std::size_t axis = 0; axis < 3; ++axis)
      if (std::abs(fire.position[axis] - (*actor.position)[axis]) > 50.0F)
        return RejectedResult(CombatRejectReason::kUnknownTarget);
    PruneProjectiles(now);
    if (ContainsDamagerCookie(fire.cookie))
      return RejectedResult(CombatRejectReason::kDuplicateProjectile);
    if (projectiles_.size() >= kMaximumProjectiles)
      return RejectedResult(CombatRejectReason::kProjectileLimit);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    auto& shot = projectiles_[fire.cookie];
    shot.owner_object_id = actor.participant.actor_object_id;
    shot.created_at = now;
    shot.weapon_bar = 3U;
    shot.explosive_projectile = true;
    actor.runner_throw.reset();
    ++actor.accepted_shots;
    auto result = AcceptedResult(actor.participant.player_id, packet);
    RecordNativeConstructors(result, actor, packet, 1U);
    return result;
  }
  if (request.kind == protocol::NativeActionKind::kRespawnDurationReport &&
      (mode_id_ == 13U || mode_id_ == 14U)) {
    if (actor.controller || actor.life_state != CombatLifeState::kDefeated ||
        !actor.respawn_wait ||
        actor.respawn_wait->life != actor.life_generation)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    auto& wait = *actor.respawn_wait;
    if (request.respawn_duration_ms !=
        static_cast<std::uint32_t>(wait.duration_ms))
      return RejectedResult(CombatRejectReason::kRespawnDurationMismatch);
    if (wait.reported)
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    wait.reported = true;
    return AcceptedResult(actor.participant.player_id, packet);
  }
  if (request.kind == protocol::NativeActionKind::kOpaqueOwnedReport ||
      request.kind == protocol::NativeActionKind::kRespawnDurationReport ||
      request.kind == protocol::NativeActionKind::kUnrecoveredObjectPair) {
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  }
  if (request.survival_melee)
    return HandleSurvivalMelee(actor, packet, *request.survival_melee, now);
  if (request.area_cast && mode_id_ == 3U && request.area_cast->type == 137U)
    return SummonTurret(actor, packet, *request.area_cast, now);
  if (request.area_cast)
    return StageAreaCast(actor, packet, *request.area_cast, now);
  if (request.kind == protocol::NativeActionKind::kSurvivalTrap)
    return HandleSurvivalTrap(actor, packet, now);
  if (packet.opcode == 32813U && actor.survival_melee &&
      !actor.survival_melee->ended &&
      actor.survival_melee->source_life == actor.life_generation &&
      actor.life_state == CombatLifeState::kAlive)
    return HandleSurvivalMeleeEnd(actor, packet, now);
  if (request.kind == protocol::NativeActionKind::kSurvivalAlliancePose) {
    if (mode_id_ != 23U && mode_id_ != 25U)
      return RejectedResult(CombatRejectReason::kUnrecoveredAction);
    if (actor.controller || actor.life_state != CombatLifeState::kAlive)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    const bool starting = packet.opcode == 33273U;
    if (actor.survival_alliance_pose == starting)
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    actor.survival_alliance_pose = starting;
    auto result = AcceptedResult(actor.participant.player_id, packet);
    // The sender only queues the request; the response applies the animation
    // and input locks locally, so include the authenticated owner as well.
    result.deliveries.push_back(
        {.recipient = actor.participant.player_id,
         .message = {.opcode = packet.opcode, .body = packet.body}});
    return result;
  }
  if (request.survival_treatment)
    return HandleSurvivalTreatment(actor, packet, *request.survival_treatment,
                                   now);
  if (request.kind == protocol::NativeActionKind::kHealthReport ||
      request.kind == protocol::NativeActionKind::kAreaHitReaction) {
    // 5833F0 schedules the32933 snapshot;580F30 can send it after a bubble
    // or death transition.7C4E90 updates remote HP without changing lifecycle.
    // Only the active hit-reaction message requires a living character.
    if (actor.controller ||
        (request.kind == protocol::NativeActionKind::kAreaHitReaction &&
         actor.life_state != CombatLifeState::kAlive))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (request.kind == protocol::NativeActionKind::kAreaHitReaction &&
        !object_ids_.contains(*request.reaction_source))
      return RejectedResult(CombatRejectReason::kUnknownRelatedActor);
    if (request.reported_health && (mode_id_ == 13U || mode_id_ == 14U)) {
      const auto stats =
          domain::NativeHotbloodStats(mode_id_, actor.participant.fighter,
                                      actor.participant.equipment_items);
      // Preserve legitimate late death/bubble snapshots and healing. This is
      // a trusted maximum bound, not a reconstruction of client collision
      // damage.
      if (*request.reported_health > stats.maximum_hp + 0.01F)
        return RejectedResult(CombatRejectReason::kHealthOutOfRange);
    }
    if (request.reported_health &&
        actor.health_report ==
            std::pair{actor.life_generation, *request.reported_health} &&
        ((mode_id_ != 23U && mode_id_ != 25U) || !actor.health_report_at ||
         now - *actor.health_report_at < std::chrono::seconds{1}))
      return RejectedResult(CombatRejectReason::kDuplicateAction);
  }
  if (request.kind == protocol::NativeActionKind::kItemEffect) {
    if ((mode_id_ == 3U || mode_id_ == 13U || mode_id_ == 14U ||
         mode_id_ == 18U) &&
        packet.opcode == 32828U) {
      protocol::PacketReader reader{packet.body};
      static_cast<void>(reader.ReadU32());
      const auto attribute = reader.ReadU32();
      const auto type = reader.ReadU32();
      if (attribute == 11U || type == 104U)
        return DeathmatchFeverAttribute(actor, packet, now);
    }
    return HandleItemEffect(actor, packet, now);
  }
  if (packet.opcode == 32812U && (mode_id_ == 3U || mode_id_ == 13U ||
                                  mode_id_ == 14U || mode_id_ == 18U)) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() == 11U)
      return DeathmatchFeverAttribute(actor, packet, now);
  }
  if (request.kind == protocol::NativeActionKind::kEffectEnd) {
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    if (!EndAreaItem(actor, packet, now))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    auto result = AcceptedResult(actor.participant.player_id, packet);
    result.deliveries.push_back(
        {.recipient = actor.controller.value_or(actor.participant.player_id),
         .message = {.opcode = packet.opcode, .body = packet.body}});
    return result;
  }
  if (request.humiliation_target.has_value()) {
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    return HandleHumiliation(actor, *request.humiliation_target, packet, now);
  }
  if (request.kind == protocol::NativeActionKind::kSlip) {
    // The -1 branch is produced by map hazards (521C40) and victim-side weapon
    // reactions (823C70). Real banana traps have a separate, unrecovered
    // ledger: forwarding their IDs would let clients delete arbitrary scene
    // objects.
    if (request.consumed_object != kNoTarget)
      return RejectedResult(CombatRejectReason::kUnknownProjectile);
    const auto expected = request.slip_bubbled ? CombatLifeState::kBubbled
                                               : CombatLifeState::kAlive;
    if (actor.life_state != expected)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  if (request.bubble_action.has_value()) {
    if (!ConsumeActionQuota(actor, now)) {
      return RejectedResult(CombatRejectReason::kRateLimited);
    }
    return HandleBubbleAction(actor, *request.bubble_action, packet, now);
  }
  if (request.reaction_source.has_value() &&
      *request.reaction_source != kWorldTarget &&
      *request.reaction_source != kNoTarget &&
      !object_ids_.contains(*request.reaction_source)) {
    return RejectedResult(CombatRejectReason::kUnknownTarget);
  }
  if (request.weapon_switch.has_value()) {
    const auto& weapon_switch = request.weapon_switch.value();
    if (!CanEquip(actor, weapon_switch.bar_index, weapon_switch.template_id)) {
      if (mode_id_ == 3U && weapon_switch.bar_index == 4U &&
          (weapon_switch.template_id == 9001U ||
           weapon_switch.template_id == 9021U))
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      ProtocolViolation(
          "native weapon switch does not match frozen loadout actor_object=" +
          std::to_string(actor.participant.actor_object_id) +
          " bar=" + std::to_string(weapon_switch.bar_index) + " template=" +
          std::to_string(weapon_switch.template_id) + " expected_template=" +
          std::to_string(
              EquippedTemplate(actor, weapon_switch.bar_index).value_or(0U)) +
          " controlled_npc=" + std::to_string(actor.controller.has_value()));
    }
  }
  if (request.compressed_fire.has_value()) {
    const auto& compressed_fire = request.compressed_fire.value();
    // A human corpse needs the original 704/707/32926 new-life handshake before
    // creating another projectile. Existing in-flight projectiles keep their
    // separate ownership/impact ledger; controlled NPC callbacks are unchanged.
    if (!actor.controller && actor.life_state == CombatLifeState::kDefeated)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (!EquippedTemplate(actor, compressed_fire.bar_index).has_value()) {
      ProtocolViolation("native fire uses an unequipped weapon bar");
    }
    // BackpackedWeaponInstance(4, 5920) drives treatment, never GoDamager
    // bullets.
    if (EquippedTemplate(actor, compressed_fire.bar_index) == 5920U ||
        EquippedTemplate(actor, compressed_fire.bar_index) == 5921U)
      return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  }
  // 8447D0 -> 8113B0 blocks sidearm selection in modes 12/18. Feature 20 is
  // absent in this PE, so melee remains usable. 84AA00 still emits set-only
  // weapon updates; preserve those without allowing a shot to bypass selection.
  const bool sniper = mode_id_ == 12U || mode_id_ == 18U;
  if (sniper &&
      ((request.weapon_switch && request.weapon_switch->select &&
        request.weapon_switch->bar_index == 1U) ||
       (request.compressed_fire && request.compressed_fire->bar_index == 1U))) {
    return RejectedResult(CombatRejectReason::kModeWeaponRestricted);
  }
  if (!ConsumeActionQuota(actor, now)) {
    return RejectedResult(CombatRejectReason::kRateLimited);
  }

  if (mode_id_ == 3U && packet.opcode == 32812U) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    const auto attribute = reader.ReadU32();
    if (attribute == 8U) actor.giant_weapon.reset();
    if (attribute == 22U) actor.bazooka.reset();
  }
  if (mode_id_ == 21U && packet.opcode == 32812U) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() == 22U) {
      if (!actor.bazooka || actor.bazooka->first != actor.life_generation ||
          actor.bazooka->second != 0U)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      actor.bazooka.reset();
    }
  }
  if (request.compressed_fire.has_value()) {
    const auto& fire = request.compressed_fire.value();
    const std::array signature{static_cast<std::uint32_t>(fire.bar_index),
                               fire.serial, fire.token};
    if (actor.last_fire == signature) {
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    }
    const bool bazooka = fire.bar_index == 4U && actor.bazooka &&
                         actor.bazooka->first == actor.life_generation;
    const bool giant = mode_id_ == 3U && !actor.controller &&
                       fire.bar_index == 4U && actor.giant_weapon &&
                       actor.giant_weapon->life == actor.life_generation &&
                       now < actor.giant_weapon->expires &&
                       EquippedTemplate(actor, 4U) == 9001U;
    if (bazooka && actor.bazooka->second == 0U)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (mode_id_ == 3U && fire.bar_index == 4U &&
        EquippedTemplate(actor, 4U) == 9001U &&
        (!giant || actor.giant_weapon->remaining_ammunition == 0U))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    PruneProjectiles(now);
    const auto count =
        NativeProjectileCount(EquippedTemplate(actor, fire.bar_index).value());
    // 7D4E40 为每颗霰弹分配 serial + index；逐颗保留归属、命中集合和终态。
    // 先校验整个编号集合，避免后面的弹丸冲突时留下部分登记。
    for (std::uint32_t index = 0U; index < count; ++index) {
      if (ContainsDamagerCookie(fire.serial + index)) {
        return RejectedResult(CombatRejectReason::kDuplicateProjectile);
      }
    }
    if (projectiles_.size() > kMaximumProjectiles - count) {
      return RejectedResult(CombatRejectReason::kProjectileLimit);
    }
    for (std::uint32_t index = 0U; index < count; ++index) {
      auto& projectile = projectiles_[fire.serial + index];
      projectile.owner_object_id = actor.participant.actor_object_id;
      projectile.created_at = now;
      projectile.weapon_bar = fire.bar_index;
      const auto weapon = EquippedTemplate(actor, fire.bar_index);
      // Current original WeaponSpecDetail4354/4390 has probability100, ally0,
      // allyBubble1/enemyBubble1.809820 emits several same-cookie32924 keep0
      // bodies in one collision update. Other probabilities are not rerolled
      // here; their existing keep contract remains unchanged.
      if (!actor.controller && fire.bar_index == 0U &&
          (weapon == 4354U || weapon == 4390U)) {
        projectile.native_piercing = ProjectileState::NativePiercing{
            .weapon_template = *weapon,
            .source_team = actor.participant.team_code,
            .source_life = actor.life_generation,
            .maximum_character_targets = actors_.size(),
            .ally = false,
            .ally_bubble = true,
            .enemy_bubble = true};
      }
      projectile.explosive_projectile =
          (bazooka && (weapon == 9021U || weapon == 9025U ||
                       (mode_id_ == 21U && weapon == 9100U))) ||
          (giant && weapon == 9001U) ||
          (mode_id_ == 16U && actor.controller && weapon == 5304U);
      // 7BD400 prioritizes smokeItemId237 for these real smoke templates in
      // every mode. Freeze it from the authorized throw;32930 cannot declare
      // its own source.
      if (!actor.controller && fire.bar_index == 3U &&
          (weapon == 5061U || weapon == 5161U))
        projectile.area_type = 237U;
      // isWard is honored only in mode22; this snapshot survives preset
      // changes.
      else if (!actor.controller && mode_id_ == 22U && fire.bar_index == 3U &&
               (weapon == 5035U || weapon == 5135U))
        projectile.area_type = 238U;
    }
    actor.last_fire = signature;
    if (bazooka) --actor.bazooka->second;
    if (giant) --actor.giant_weapon->remaining_ammunition;
    ++actor.accepted_shots;
  }
  if (request.position.has_value()) {
    actor.position = request.position;
  }
  if (request.reported_health) {
    actor.health_report =
        std::pair{actor.life_generation, *request.reported_health};
    actor.health_report_at = now;
  }
  if (request.weapon_switch && (mode_id_ == 23U || mode_id_ == 25U))
    actor.participant.weapon_templates[request.weapon_switch->bar_index] =
        request.weapon_switch->template_id;
  auto result = AcceptedResult(actor.participant.player_id, packet);
  if (movement_tick_aggregation_enabled_ && !actor.controller &&
      IsMovementOpcode(packet.opcode) && request.position.has_value()) {
    pending_movements_.insert_or_assign(
        actor.participant.player_id,
        OutboundMessage{.opcode = packet.opcode, .body = packet.body});
    // Movement is an unreliable latest-state lane. Reliable actions keep their
    // FIFO order; the snapshot is expanded at the room tick instead of once
    // per input packet.
    result.deliveries.clear();
  }
  if (request.compressed_fire) {
    const auto weapon =
        EquippedTemplate(actor, request.compressed_fire->bar_index);
    if (weapon)
      RecordNativeConstructors(result, actor, packet,
                               NativeProjectileCount(*weapon));
  }
  if (request.weapon_switch && request.weapon_switch->select) {
    actor.selected_weapon_bar =
        static_cast<std::uint8_t>(request.weapon_switch->bar_index);
    if (actor.selected_weapon_bar != 4U)
      CancelSurvivalTreatment(actor, result, false);
  }
  if (request.kind == protocol::NativeActionKind::kWallAction ||
      request.kind == protocol::NativeActionKind::kCharacterControl ||
      request.kind == protocol::NativeActionKind::kEffectEnd ||
      request.kind == protocol::NativeActionKind::kTextEmote ||
      request.kind == protocol::NativeActionKind::kAreaHitReaction ||
      request.kind == protocol::NativeActionKind::kSlip) {
    // 原版 8C4340/904D20 发送后等待 904DD0/904FA0
    // 状态确认；只转发旁观端会卡在贴墙前。
    const auto owner = actor.controller.value_or(actor.participant.player_id);
    result.deliveries.push_back(
        {.recipient = owner,
         .message = {.opcode = packet.opcode, .body = packet.body}});
  }
  return result;
}

CombatRelayResult CombatRelay::HandleHumiliation(
    ActorState& actor, std::uint32_t target, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto owner = object_owners_.find(target);
  if (owner == object_owners_.end() ||
      target == actor.participant.actor_object_id)
    return RejectedResult(CombatRejectReason::kUnknownRelatedActor);
  auto& victim = actors_.at(owner->second);
  const bool eligible =
      actor.life_state == CombatLifeState::kAlive &&
      victim.life_state == CombatLifeState::kDefeated &&
      victim.death_stage == 2U &&
      (individual_scoring_ ||
       actor.participant.team_code != victim.participant.team_code ||
       (actor.infection_actor && victim.infection_actor &&
        actor.participant.team_code == 2U));
  if (packet.opcode == 32815U) {
    std::erase_if(actor.humiliations, [now](const auto& entry) {
      return now < entry.second.started ||
             now - entry.second.started > std::chrono::seconds{3};
    });
    if (!eligible || !actor.position || !victim.position)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // Native range 30 / height 5 (GameOption); allow 20 / 5 units of transport
    // tolerance. Facing/collision remain client predictions, not server
    // physics.
    const auto dx = (*actor.position)[0] - (*victim.position)[0];
    const auto dy = (*actor.position)[1] - (*victim.position)[1];
    const auto dz = (*actor.position)[2] - (*victim.position)[2];
    if (dx * dx + dz * dz > 2500.0F || std::abs(dy) > 10.0F)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // Native state 117 runs one approach animation, so one actor cannot begin
    // simultaneous humiliations against several targets.
    if (!actor.humiliations.empty())
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    actor.humiliations.emplace(
        target,
        PendingHumiliation{actor.life_generation, victim.life_generation, now});
  } else {
    const auto pending = actor.humiliations.find(target);
    if (pending == actor.humiliations.end())
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    const auto started = pending->second;
    // FighterConfig humiliateTime=500 ms; a 150 ms tolerance covers packet
    // jitter.
    if (now < started.started ||
        now - started.started < std::chrono::milliseconds{350})
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    actor.humiliations.erase(pending);
    if (!eligible || started.source_life != actor.life_generation ||
        started.target_life != victim.life_generation ||
        now - started.started > std::chrono::seconds{3}) {
      // 7D9080 accepts a missing target and finishes only the attacker's
      // animation.
      protocol::PacketWriter writer;
      writer.WriteU32(actor.participant.actor_object_id);
      writer.WriteU32(kNoTarget);
      const protocol::Packet cancelled{.opcode = 32816U,
                                       .body = writer.Release()};
      auto result = AcceptedResult(actor.participant.player_id, cancelled);
      result.deliveries.push_back(
          {.recipient = actor.controller.value_or(actor.participant.player_id),
           .message = {.opcode = cancelled.opcode, .body = cancelled.body}});
      return result;
    }
    const auto serial = victim.humiliated ? victim.humiliated->serial + 1U : 1U;
    victim.humiliated = HumiliationEvent{serial, victim.life_generation, now,
                                         actor.participant.player_id};
  }
  auto result = AcceptedResult(actor.participant.player_id, packet);
  result.deliveries.push_back(
      {.recipient = actor.controller.value_or(actor.participant.player_id),
       .message = {.opcode = packet.opcode, .body = packet.body}});
  return result;
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleHit(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeNativeHit(packet.opcode, packet.body);
  PruneProjectiles(now);
  const auto found = projectiles_.find(request.projectile_cookie);
  if (found == projectiles_.end()) {
    return RejectedResult(CombatRejectReason::kUnknownProjectile);
  }
  auto& projectile = found->second;
  if (projectile.owner_object_id != actor.participant.actor_object_id) {
    return RejectedResult(CombatRejectReason::kForeignProjectile);
  }
  if (projectile.terminal) {
    return RejectedResult(CombatRejectReason::kTerminalProjectile);
  }
  if (projectile.area_type == 137U)
    return RejectedResult(CombatRejectReason::kUnrecoveredAction);
  const auto target_turret = turrets_.find(request.target_object_id);
  if (target_turret != turrets_.end() && target_turret->second.ended)
    return RejectedResult(CombatRejectReason::kUnknownTarget);
  if (turrets_.contains(projectile.owner_object_id) &&
      object_owners_.contains(request.target_object_id) &&
      actors_.at(object_owners_.at(request.target_object_id))
              .participant.team_code == actor.participant.team_code)
    return RejectedResult(CombatRejectReason::kUnknownTarget);
  if (request.target_object_id != kWorldTarget &&
      request.target_object_id != kNoTarget &&
      !object_ids_.contains(request.target_object_id)) {
    return RejectedResult(CombatRejectReason::kUnknownTarget);
  }
  if (projectile.targets.contains(request.target_object_id)) {
    return RejectedResult(CombatRejectReason::kDuplicateImpact);
  }
  projectile.targets.insert(request.target_object_id);
  const auto owner = object_owners_.find(request.target_object_id);
  bool native_continuation = false;
  if (projectile.native_piercing && owner != object_owners_.end() &&
      projectile.targets.size() <
          projectile.native_piercing->maximum_character_targets) {
    const auto& snapshot = *projectile.native_piercing;
    const auto& target = actors_.at(owner->second);
    if (!target.controller) {
      const bool ally = snapshot.source_team == target.participant.team_code;
      const bool bubble = target.life_state == CombatLifeState::kBubbled;
      // Original809FD0: ally0 also blocks continuation after an allied bubble.
      // The first collision still relays; this only authorizes the next target.
      native_continuation =
          ally ? snapshot.ally && (!bubble || snapshot.ally_bubble)
               : !bubble || snapshot.enemy_bubble;
    }
  }
  // World/no-target and unbound classes retain their existing terminal
  // contract. Explicit32774 also seals the cookie. Never infer a world
  // penetration grant.
  projectile.terminal = !request.keep_projectile && !native_continuation;
  if (owner != object_owners_.end() &&
      owner->second != actor.participant.player_id) {
    auto& victim = actors_.at(owner->second);
    if (victim.life_state == CombatLifeState::kAlive && !victim.controller &&
        turrets_.contains(actor.participant.actor_object_id)) {
      victim.turret_impacts[actor.participant.actor_object_id] =
          PendingKick{.attacker = actor.participant.actor_object_id,
                      .generation = victim.life_generation,
                      .requested_at = now};
    }
    if (victim.soccer_ball ||
        (victim.life_state == CombatLifeState::kBubbled &&
         (individual_scoring_ ||
          victim.participant.team_code != actor.participant.team_code))) {
      victim.bubble_impacts[actor.participant.actor_object_id] =
          PendingKick{.attacker = actor.participant.actor_object_id,
                      .generation = victim.life_generation,
                      .requested_at = now};
    }
  }
  return AcceptedResult(actor.participant.player_id, packet);
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleWeaponObject(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request =
      protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
  using Kind = protocol::NativeWeaponObjectKind;
  PruneProjectiles(now);
  const auto owner = actor.participant.actor_object_id;
  if (request.kind == Kind::kBlast && request.template_id == 5911U)
    return HandleSurvivalWater(actor, packet, request, now);
  if (request.kind == Kind::kDestroyProjectile &&
      survival_traps_.contains(request.cookie))
    return DestroySurvivalTrap(actor, packet, request.cookie, now);
  // 80E640 -> 7C4B00/32907 -> 7D63A0 uses the shared GoDamager cookie
  // registry, which also contains GoGrenade. A registered projectile must be
  // sealed here before any later 32900 can turn it into a damaging blast.
  const bool cancel_projectile =
      request.kind == Kind::kDestroy && projectiles_.contains(request.cookie);
  if (request.kind == Kind::kAttachGrenade ||
      request.kind == Kind::kDestroyProjectile || cancel_projectile) {
    const auto found = projectiles_.find(request.cookie);
    if (found == projectiles_.end() ||
        (!cancel_projectile && object_ids_.contains(request.cookie)))
      return RejectedResult(CombatRejectReason::kUnknownProjectile);
    auto& projectile = found->second;
    if (projectile.owner_object_id != owner)
      return RejectedResult(CombatRejectReason::kForeignProjectile);
    if (projectile.destroyed ||
        (request.kind == Kind::kAttachGrenade && projectile.terminal))
      return RejectedResult(CombatRejectReason::kTerminalProjectile);
    if (request.kind == Kind::kAttachGrenade) {
      if (projectile.weapon_bar != 3U)
        return RejectedResult(CombatRejectReason::kUnrecoveredAction);
      if (!object_ids_.contains(request.target_object_id))
        return RejectedResult(CombatRejectReason::kUnknownTarget);
      if (projectile.attached)
        return RejectedResult(CombatRejectReason::kDuplicateAction);
    }
    if (!ConsumeActionQuota(actor, now))
      return RejectedResult(CombatRejectReason::kRateLimited);
    if (request.kind == Kind::kAttachGrenade)
      projectile.attached = true;
    else {
      projectile.terminal = true;
      projectile.destroyed = true;
    }
    auto result = AcceptedResult(actor.participant.player_id, packet);
    if (request.kind == Kind::kAttachGrenade) {
      // 80EFB0 only sets the pending flag; 810AD0 installs the target and local
      // transform on the controlling client as well as on its peers.
      result.deliveries.push_back(
          {.recipient = actor.controller.value_or(actor.participant.player_id),
           .message = {.opcode = packet.opcode, .body = packet.body}});
    }
    return result;
  }
  if (request.kind == Kind::kHit || request.kind == Kind::kDestroy) {
    const auto found = weapon_objects_.find(request.cookie);
    if (found == weapon_objects_.end()) {
      return RejectedResult(CombatRejectReason::kUnknownProjectile);
    }
    auto& weapon = found->second;
    if (weapon.owner_object_id != owner) {
      return RejectedResult(CombatRejectReason::kForeignProjectile);
    }
    if (weapon.terminal) {
      return RejectedResult(CombatRejectReason::kTerminalProjectile);
    }
    if (weapon.self_life && *weapon.self_life != actor.life_generation)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (request.kind == Kind::kDestroy) {
      weapon.terminal = true;
    } else {
      if (weapon.self_only && request.target_object_id != owner)
        return RejectedResult(CombatRejectReason::kUnknownTarget);
      if (!object_ids_.contains(request.target_object_id)) {
        return RejectedResult(CombatRejectReason::kUnknownTarget);
      }
      const auto target = object_owners_.find(request.target_object_id);
      // Original5026/923470 has both self/team target flags disabled. Retain
      // the authenticated136 source through detonation; an arbitrary blast or
      // another weapon must not acquire this item's target policy.
      if (weapon.area_type == 136U &&
          (request.target_object_id == owner ||
           (target != object_owners_.end() &&
            actors_.at(target->second).participant.team_code ==
                actor.participant.team_code)))
        return RejectedResult(CombatRejectReason::kUnknownTarget);
      if (!weapon.targets.insert(request.target_object_id).second) {
        return RejectedResult(CombatRejectReason::kDuplicateImpact);
      }
      if (target != object_owners_.end()) {
        auto& victim = actors_.at(target->second);
        if (victim.soccer_ball)
          victim.bubble_impacts[owner] =
              PendingKick{.attacker = owner,
                          .generation = victim.life_generation,
                          .requested_at = now};
      }
    }
    return AcceptedResult(actor.participant.player_id, packet);
  }

  ProjectileState* source = nullptr;
  std::uint8_t source_bar = 3U;
  bool captain_blast = false;
  std::optional<std::uint32_t> trap_blast;
  if (request.kind == Kind::kDetonate) {
    const auto found = projectiles_.find(request.source_cookie);
    if (found == projectiles_.end()) {
      return RejectedResult(CombatRejectReason::kUnknownProjectile);
    }
    source = &found->second;
    if (source->owner_object_id != owner) {
      return RejectedResult(CombatRejectReason::kForeignProjectile);
    }
    if (source->terminal) {
      return RejectedResult(CombatRejectReason::kTerminalProjectile);
    }
    if (source->area_type == 137U)
      return RejectedResult(CombatRejectReason::kUnrecoveredAction);
    if (source->weapon_bar != 3U && !source->explosive_projectile) {
      ProtocolViolation("native detonation source is not a grenade");
    }
    source_bar = source->weapon_bar;
  } else {
    if (request.actor_object_id != owner) {
      ProtocolViolation("native weapon object belongs to another character");
    }
    if (request.kind == Kind::kMelee && !actor.controller &&
        actor.life_state == CombatLifeState::kDefeated)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // 55DFA0 -> 84FAF0 installs captain melee into bar4. Only the consumed
    // assignment grants this exception; an arbitrary equipped item is
    // insufficient.
    const bool captain_melee =
        request.kind == Kind::kMelee && !actor.controller &&
        actor.captain_weapon == request.template_id &&
        EquippedTemplate(actor, 4U) == request.template_id;
    const bool giant_melee = mode_id_ == 3U && request.kind == Kind::kMelee &&
                             !actor.controller && actor.giant_weapon &&
                             request.template_id == 9001U &&
                             EquippedTemplate(actor, 4U) == 9001U;
    if (mode_id_ == 3U && request.template_id == 9001U && !giant_melee)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    // 5608B0 emits exactly one team-colored blast after applying attribute11.
    captain_blast = request.kind == Kind::kBlast && !actor.controller &&
                    actor.captain_blast &&
                    actor.captain_blast->weapon == request.template_id;
    if (request.kind == Kind::kBlast && request.template_id == 5921U) {
      trap_blast = SurvivalTrapBlast(actor, packet, now);
      if (!trap_blast)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (captain_blast) {
      if (actor.life_state != CombatLifeState::kAlive ||
          now >= actor.captain_blast->expires || !actor.captain_weapon)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      if (actor.captain_blast->consumed)
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      if (request.attack_variant != actor.participant.team_code)
        ProtocolViolation("captain fever blast has another team");
      protocol::PacketReader vectors{std::span{packet.body}.subspan(19U, 24U)};
      for (unsigned index = 0U; index < 6U; ++index) {
        if (vectors.ReadF32() != (index == 4U ? 1.0F : 0.0F))
          ProtocolViolation("captain fever blast has non-native fixed vectors");
      }
    }
    const auto bar = captain_melee || captain_blast || giant_melee ? 4U
                     : request.kind == Kind::kMelee                ? 2U
                                                                   : 3U;
    source_bar = static_cast<std::uint8_t>(bar);
    // 8C4BD0 -> 7C82F0 使用当前武器模板；海盗实机 2101 位于栏 0。
    // 仅放开已由房主绑定的海盗/PVE NPC，仍必须匹配该 NPC 已登记的装备。
    const bool equipped_npc_melee =
        request.kind == Kind::kMelee && actor.controller.has_value() &&
        actor.melee_uses_equipped_template &&
        std::ranges::any_of(
            actor.participant.weapon_templates, [&](const auto& weapon) {
              return weapon.has_value() && *weapon == request.template_id;
            });
    // Build 752 ItemSpecZombiePve uses hitAttack/rangeBomb weapons directly,
    // not the human grenade bar. Only a map-bound Boss receives this allowlist.
    const bool npc_blast =
        mode_id_ == 16U && request.kind == Kind::kBlast && actor.controller &&
        actor.life_state == CombatLifeState::kAlive &&
        actor.npc_blast_templates.contains(request.template_id);
    if (EquippedTemplate(actor, bar) != request.template_id &&
        !equipped_npc_melee && !captain_blast && !trap_blast && !npc_blast) {
      ProtocolViolation(
          "native weapon object does not match frozen loadout actor_object=" +
          std::to_string(owner) +
          " template=" + std::to_string(request.template_id) +
          " expected_bar=" + std::to_string(bar) + " expected_template=" +
          std::to_string(actor.participant.weapon_templates[bar].value_or(0U)) +
          " controlled_npc=" + std::to_string(actor.controller.has_value()));
    }
  }
  if (ContainsDamagerCookie(request.cookie)) {
    return RejectedResult(CombatRejectReason::kDuplicateProjectile);
  }
  if (weapon_objects_.size() + storms_.size() >= kMaximumProjectiles) {
    return RejectedResult(CombatRejectReason::kProjectileLimit);
  }
  if (!ConsumeActionQuota(actor, now)) {
    return RejectedResult(CombatRejectReason::kRateLimited);
  }
  auto& weapon = weapon_objects_[request.cookie];
  weapon.owner_object_id = owner;
  weapon.created_at = now;
  weapon.weapon_bar = source_bar;
  if (captain_blast) actor.captain_blast->consumed = true;
  if (trap_blast) {
    auto& rod = survival_traps_.at(*trap_blast);
    rod.exploded = true;
    survival_traps_.at(rod.peer).exploded = true;
  }
  if (source != nullptr) {
    weapon.area_type = source->area_type;
    source->terminal = true;
  }
  auto result = AcceptedResult(actor.participant.player_id, packet);
  if (trap_blast) {
    // 62DA00 only queues 7C8180; the owner also needs the actual damager.
    result.deliveries.push_back(
        {.recipient = actor.participant.player_id,
         .message = {.opcode = packet.opcode, .body = packet.body}});
  }
  if (source != nullptr)
    CommitAreaCast(actor, packet, request.source_cookie, now, result);
  RecordNativeConstructors(result, actor, packet, 1U);
  return result;
}

bool CombatRelay::ContainsDamagerCookie(std::uint32_t cookie) const noexcept {
  // 808B30 (GoBullet) -> 91FBF0 (GoDamager), then 7BBFB0 registers +888
  // in the same Manager+984 lookup used by melee, blasts and storm damagers.
  return projectiles_.contains(cookie) || weapon_objects_.contains(cookie) ||
         storms_.contains(cookie);
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleBubbleAction(
    ActorState& actor, const protocol::NativeBubbleAction& request,
    const protocol::Packet& packet, std::chrono::steady_clock::time_point now) {
  if (packet.opcode == 32792U || packet.opcode == 32837U) {
    if (!actor.active_kick.has_value()) {
      return RejectedResult(CombatRejectReason::kDuplicateAction);
    }
    actor.active_kick.reset();
    return AcceptedResult(actor.participant.player_id, packet);
  }
  const auto victim_owner = object_owners_.find(request.victim);
  const auto attacker_owner = object_owners_.find(request.attacker);
  if (victim_owner == object_owners_.end() ||
      attacker_owner == object_owners_.end() ||
      request.victim == request.attacker) {
    return RejectedResult(CombatRejectReason::kUnknownRelatedActor);
  }
  auto& victim = actors_.at(victim_owner->second);
  auto& attacker = actors_.at(attacker_owner->second);
  if (victim.soccer_ball) {
    // Native NPCTutorial starts in protected state 301 (7CB690 -> 8AC410).
    // Kicks and weapon damage call 943200; its 32796 response applies velocity
    // in 7DD340 -> 82A290, including on the controlling host. It is not a kill.
    if (attacker.controller ||
        ((packet.opcode == 32791U || packet.opcode == 32794U) &&
         attacker.life_state != CombatLifeState::kAlive))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (packet.opcode == 32791U) {
      const std::array<std::uint64_t, 3U> signature{
          request.victim, request.variant, victim.life_generation};
      if (actor.active_kick == signature)
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      actor.active_kick = signature;
      return AcceptedResult(actor.participant.player_id, packet);
    }
    if (packet.opcode == 32794U) {
      if (victim.pending_kick && now >= victim.pending_kick->requested_at &&
          now - victim.pending_kick->requested_at <= std::chrono::seconds{5})
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      victim.pending_kick = PendingKick{.attacker = request.attacker,
                                        .generation = victim.life_generation,
                                        .requested_at = now};
      auto result = AcceptedResult(actor.participant.player_id, packet);
      // The ball controller must execute 943200 even when that same human
      // kicked.
      if (victim.controller == actor.participant.player_id)
        result.deliveries.push_back(
            {.recipient = actor.participant.player_id,
             .message = {.opcode = packet.opcode, .body = packet.body}});
      return result;
    }
    const auto current = [&](const PendingKick& pending) {
      return pending.attacker == request.attacker &&
             pending.generation == victim.life_generation &&
             now >= pending.requested_at &&
             now - pending.requested_at <= std::chrono::seconds{5};
    };
    const auto impact = victim.bubble_impacts.find(request.attacker);
    if (packet.opcode != 32796U ||
        !((victim.pending_kick && current(*victim.pending_kick)) ||
          (impact != victim.bubble_impacts.end() && current(impact->second))))
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    if (impact != victim.bubble_impacts.end())
      victim.bubble_impacts.erase(impact);
    if (victim.pending_kick && current(*victim.pending_kick))
      victim.pending_kick.reset();
    attacker.active_kick.reset();
    auto result = AcceptedResult(actor.participant.player_id, packet);
    result.deliveries.push_back(
        {.recipient = *victim.controller,
         .message = {.opcode = packet.opcode, .body = packet.body}});
    return result;
  }
  if (victim.life_state != CombatLifeState::kBubbled) {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  if (packet.opcode == 32791U || packet.opcode == 32794U) {
    if (attacker.life_state != CombatLifeState::kAlive) {
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (packet.opcode == 32791U) {
      const std::array<std::uint64_t, 3U> signature{
          request.victim, request.variant, victim.life_generation};
      if (actor.active_kick == signature) {
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      }
      actor.active_kick = signature;
    } else {
      if (victim.pending_kick.has_value() &&
          victim.pending_kick->generation == victim.life_generation &&
          now >= victim.pending_kick->requested_at &&
          now - victim.pending_kick->requested_at < std::chrono::seconds{2}) {
        return RejectedResult(CombatRejectReason::kDuplicateAction);
      }
      victim.pending_kick = PendingKick{.attacker = request.attacker,
                                        .generation = victim.life_generation,
                                        .requested_at = now};
    }
    auto result = AcceptedResult(actor.participant.player_id, packet);
    if (packet.opcode == 32794U && victim.training_dummy && victim.controller &&
        victim.controller == attacker.participant.player_id)
      result.deliveries.push_back(
          {*victim.controller, {packet.opcode, packet.body}});
    return result;
  }
  const auto current = [&](const PendingKick& pending) {
    return pending.attacker == request.attacker &&
           pending.generation == victim.life_generation &&
           now >= pending.requested_at &&
           now - pending.requested_at <= std::chrono::seconds{5};
  };
  const bool kicked = victim.pending_kick && current(*victim.pending_kick);
  const auto impact = victim.bubble_impacts.find(request.attacker);
  const bool shot =
      impact != victim.bubble_impacts.end() && current(impact->second);
  // 823C70 -> 8B2D70/8BADF0 sends 32795(special=1) directly after a
  // projectile burst. 32796 can also be the native protection response to it.
  // The victim still owns the decision; an authenticated projectile impact
  // substitutes for the kick request, never for client damage calculation.
  if (request.shot_burst ? !shot
                         : (!kicked && !(packet.opcode == 32796U && shot))) {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  if (impact != victim.bubble_impacts.end())
    victim.bubble_impacts.erase(impact);
  victim.pending_kick.reset();
  if (packet.opcode == 32795U && request.success) {
    victim.life_state = CombatLifeState::kDefeated;
    ++victim.item_drop_event;
    victim.item_drop_at = now;
    victim.bubble_impacts.clear();
    victim.defeated = DefeatEvent{request.attacker, victim.life_generation, now,
                                  victim.life_generation};
    protocol::PacketReader position_reader{packet.body};
    static_cast<void>(position_reader.ReadBytes(12U));
    victim.defeated->position =
        std::array{position_reader.ReadF32(), position_reader.ReadF32(),
                   position_reader.ReadF32()};
    victim.life_settled = false;
    victim.death_stage = 0U;
    victim.respawn_point.reset();
    BeginHotbloodRespawn(victim, now);
    RecordLifecycleStatistics(victim,
                              {.kind = protocol::NativeLifecycleKind::kDefeat,
                               .actor_object_id = request.victim,
                               .related_object_id = request.attacker,
                               .direct_defeat = true});
  }
  auto result = AcceptedResult(actor.participant.player_id, packet);
  if (victim.training_dummy && victim.controller &&
      victim.controller == attacker.participant.player_id)
    result.deliveries.push_back(
        {*victim.controller, {packet.opcode, packet.body}});
  return result;
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleLifecycle(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request =
      protocol::DecodeNativeLifecycle(packet.opcode, packet.body);
  if (actor.soccer_ball)
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (request.actor_object_id != actor.participant.actor_object_id) {
    const auto target = object_owners_.find(request.actor_object_id);
    const bool known_target = target != object_owners_.end();
    ProtocolViolation(
        "native lifecycle belongs to another character actor_object=" +
        std::to_string(request.actor_object_id) + " expected_object=" +
        std::to_string(actor.participant.actor_object_id) + " sender_is_host=" +
        std::to_string(host_ == actor.participant.player_id) +
        " target_registered=" + std::to_string(known_target) +
        " target_controlled_npc=" +
        std::to_string(known_target &&
                       actors_.at(target->second).controller.has_value()));
  }
  const bool item_escape =
      mode_id_ == 3U && !actor.controller &&
      request.kind == protocol::NativeLifecycleKind::kRecovery &&
      request.related_object_id == 0xFFFFFFFEU &&
      actor.life_state == CombatLifeState::kBubbled && actor.item_escape &&
      actor.item_escape->life == actor.life_generation &&
      now < actor.item_escape->expires && !actor.item_escape->recovered;
  if (request.related_object_id.has_value()) {
    const auto related = request.related_object_id.value();
    if (related != kWorldTarget && related != kNoTarget && !item_escape &&
        !object_ids_.contains(related)) {
      return RejectedResult(CombatRejectReason::kUnknownRelatedActor);
    }
  }

  switch (request.kind) {
    case protocol::NativeLifecycleKind::kDefeat: {
      if (packet.opcode == 32787U) {
        if (actor.defeat_reports.contains(packet.body))
          return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
        if (actor.defeat_reports.size() >= kMaximumDefeatReports)
          return RejectedResult(CombatRejectReason::kRateLimited);
      }
      const auto bubble_life = actor.life_state == CombatLifeState::kBubbled
                                   ? std::optional{actor.life_generation}
                                   : std::nullopt;
      const auto next = request.direct_defeat ? CombatLifeState::kDefeated
                                              : CombatLifeState::kBubbled;
      if (actor.life_state == next ||
          actor.life_state == CombatLifeState::kDefeated) {
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      }
      if (packet.opcode == 32787U) actor.defeat_reports.insert(packet.body);
      actor.life_state = next;
      ++actor.life_generation;
      ++actor.item_drop_event;
      actor.item_drop_at = now;
      actor.turret_impacts.clear();
      actor.defeated.reset();
      if (request.direct_defeat && request.related_object_id)
        actor.defeated = DefeatEvent{*request.related_object_id,
                                     actor.life_generation, now, bubble_life};
      if (actor.defeated && packet.opcode == 32787U) {
        protocol::PacketReader position_reader{packet.body};
        static_cast<void>(position_reader.ReadBytes(8U));
        actor.defeated->position =
            std::array{position_reader.ReadF32(), position_reader.ReadF32(),
                       position_reader.ReadF32()};
      }
      rescues_.erase(actor.participant.actor_object_id);
      actor.pending_kick.reset();
      actor.bubble_impacts.clear();
      actor.active_kick.reset();
      actor.life_settled = false;
      actor.death_stage = 0U;
      actor.respawn_point.reset();
      BeginHotbloodRespawn(actor, now);
      break;
    }
    case protocol::NativeLifecycleKind::kBubbleSettled:
      if (actor.life_state != CombatLifeState::kBubbled) {
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      }
      if (actor.life_settled) {
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      }
      actor.life_settled = true;
      break;
    case protocol::NativeLifecycleKind::kRecovery:
      // 32806 is bubble recovery, not an alternative to 704/707/32926 after
      // death.
      if (actor.life_state == CombatLifeState::kDefeated &&
          (mode_id_ == 13U || mode_id_ == 14U || mode_id_ == 21U))
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      if (actor.life_state == CombatLifeState::kDefeated &&
          !actor.participant.ordinary_respawn_allowed) {
        return RejectedResult(CombatRejectReason::kRespawnDisabled);
      }
      if (actor.life_state == CombatLifeState::kDefeated && !actor.controller)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      if (actor.life_state == CombatLifeState::kAlive) {
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      }
      actor.life_state = CombatLifeState::kAlive;
      if (item_escape) actor.item_escape->recovered = true;
      rescues_.erase(actor.participant.actor_object_id);
      actor.pending_kick.reset();
      actor.bubble_impacts.clear();
      actor.life_settled = false;
      actor.death_stage = 0U;
      actor.respawn_point.reset();
      actor.protection_report.clear();
      break;
    case protocol::NativeLifecycleKind::kProtection:
      if (std::ranges::equal(actor.protection_report, packet.body)) {
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      }
      actor.protection_report.assign(packet.body.begin(), packet.body.end());
      break;
    case protocol::NativeLifecycleKind::kDeathAnimationStarted:
    case protocol::NativeLifecycleKind::kDeathAnimationFinished: {
      if (actor.life_state != CombatLifeState::kDefeated) {
        if (!actor.controller.has_value()) {
          return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
        }
        // 原版 7D8430 进入 403，7D84F0 执行死亡完成；DBF410 将 400..499
        // 判为清场死亡。
        // 仅接受已绑定海盗的认证房主通知，不把玩家死亡前置校验一并放宽。
        actor.life_state = CombatLifeState::kDefeated;
        ++actor.life_generation;
        actor.life_settled = true;
        actor.pending_kick.reset();
        actor.bubble_impacts.clear();
        actor.active_kick.reset();
        rescues_.erase(actor.participant.actor_object_id);
      }
      const std::uint8_t stage =
          request.kind == protocol::NativeLifecycleKind::kDeathAnimationStarted
              ? 1U
              : 2U;
      if (stage <= actor.death_stage) {
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      }
      actor.death_stage = stage;
      break;
    }
  }
  RecordLifecycleStatistics(actor, request);
  auto result = AcceptedResult(actor.participant.player_id, packet);
  if (request.kind == protocol::NativeLifecycleKind::kDefeat)
    ClearDeathmatchFever(actor, result);
  if (actor.life_state != CombatLifeState::kAlive)
    CancelSurvivalTreatment(actor, result, true);
  if (actor.survival_alliance_pose &&
      actor.life_state != CombatLifeState::kAlive) {
    // Release input locks before the receiver enters bubble/death animation.
    // This is server cleanup, not a client-authorized change to team or HP.
    protocol::PacketWriter writer;
    writer.WriteU32(actor.participant.actor_object_id);
    protocol::Packet cancel{.opcode = 33274U, .body = writer.Release()};
    auto cleanup = AcceptedResult(actor.participant.player_id, cancel);
    cleanup.deliveries.push_back(
        {.recipient = actor.participant.player_id,
         .message = {.opcode = cancel.opcode, .body = cancel.body}});
    result.deliveries.insert(result.deliveries.begin(),
                             cleanup.deliveries.begin(),
                             cleanup.deliveries.end());
    actor.survival_alliance_pose = false;
  }
  if (packet.opcode == 32805U) {
    // 8BA990 only marks its request pending; the native response starts death.
    result.deliveries.push_back(
        {.recipient = actor.controller.value_or(actor.participant.player_id),
         .message = {.opcode = packet.opcode, .body = packet.body}});
  }
  return result;
}

application::ServiceResult<CombatRelayResult> CombatRelay::HandleRespawn(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request =
      protocol::DecodeNativeRespawn(packet.opcode, packet.body);
  if (request.actor_object_id != actor.participant.actor_object_id) {
    ProtocolViolation("native respawn belongs to another character");
  }
  if (!actor.participant.ordinary_respawn_allowed) {
    return RejectedResult(CombatRejectReason::kRespawnDisabled);
  }
  if (actor.participant.ordinary_spawn_range &&
      (request.respawn_point < actor.participant.ordinary_spawn_range->first ||
       request.respawn_point > actor.participant.ordinary_spawn_range->last))
    return RejectedResult(CombatRejectReason::kRespawnMismatch);
  if (actor.participant.ordinary_spawn_point &&
      request.respawn_point != *actor.participant.ordinary_spawn_point)
    return RejectedResult(CombatRejectReason::kRespawnMismatch);
  if (actor.racing_stage) {
    auto& stage = *actor.racing_stage;
    if (mode_id_ != 24U || !stage.begins || !stage.expires ||
        now < *stage.begins || now >= *stage.expires)
      return RejectedResult(CombatRejectReason::kInactiveMatchPhase);
    if (request.respawn_point != stage.spawn_point || packet.opcode == 705U)
      return RejectedResult(CombatRejectReason::kRespawnMismatch);
    if (stage.needs_reset) {
      if (stage.stage < 2U || (request.completion && request.flag) ||
          actor.life_state != CombatLifeState::kAlive)
        return RejectedResult(CombatRejectReason::kRespawnMismatch);
      if (!request.completion) {
        if (actor.respawn_point)
          return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
        actor.respawn_point = stage.spawn_point;
        return CombatRelayResult{
            .accepted = true,
            .deliveries = {
                {.recipient = actor.participant.player_id,
                 .message = {
                     .opcode = 707U,
                     .body = protocol::EncodeNativeRespawnAuthorization(
                         request.actor_object_id, stage.spawn_point)}}}};
      }
      if (actor.respawn_point != stage.spawn_point)
        return RejectedResult(CombatRejectReason::kRespawnMismatch);
      stage.needs_reset = false;
      actor.respawn_point.reset();
      actor.position = stage.spawn_position;
      actor.protection_report.clear();
      return AcceptedResult(actor.participant.player_id, packet);
    }
  }
  if (!request.completion) {
    if (actor.life_state != CombatLifeState::kDefeated ||
        actor.death_stage != 2U) {
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (actor.respawn_point.has_value()) {
      return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
    }
    if (mode_id_ == 13U || mode_id_ == 14U) {
      if (!actor.respawn_wait ||
          actor.respawn_wait->life != actor.life_generation)
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      if (request.flag && actor.instant_respawn_life != actor.life_generation)
        return RejectedResult(CombatRejectReason::kRespawnMismatch);
      auto& wait = *actor.respawn_wait;
      const bool instant = actor.instant_respawn_life == actor.life_generation;
      if (wait.pending_point &&
          !(instant && *wait.pending_point == request.respawn_point))
        return RejectedResult(CombatRejectReason::kDuplicateLifecycle);
      if (!instant && now < wait.due) {
        wait.pending_point = request.respawn_point;
        return CombatRelayResult{.accepted = true};
      }
      wait.pending_point.reset();
    }
    actor.respawn_point = request.respawn_point;
    return CombatRelayResult{
        .accepted = true,
        .reject_reason = std::nullopt,
        .deliveries = {
            {.recipient = actor.participant.player_id,
             .message = {
                 .opcode = 707U,
                 .body = protocol::EncodeNativeRespawnAuthorization(
                     request.actor_object_id, request.respawn_point)}}}};
  }
  if (actor.life_state != CombatLifeState::kDefeated ||
      actor.respawn_point != request.respawn_point ||
      (request.flag && actor.instant_respawn_life != actor.life_generation)) {
    return RejectedResult(CombatRejectReason::kRespawnMismatch);
  }
  actor.position.reset();
  if (actor.racing_stage)
    actor.position = actor.racing_stage->spawn_position;
  else if (const auto pose = std::ranges::find(
               actor.participant.ordinary_spawn_poses, request.respawn_point,
               &NativeTeamSpawnPose::point);
           pose != actor.participant.ordinary_spawn_poses.end())
    actor.position = pose->position;
  actor.life_state = CombatLifeState::kAlive;
  actor.bubble_impacts.clear();
  ++actor.item_drop_event;
  actor.item_drop_at = now;
  if (mode_id_ == 3U || mode_id_ == 13U || mode_id_ == 14U || mode_id_ == 18U) {
    SaturatedIncrement(actor.deathmatch_fever.respawns_without_pang);
    actor.deathmatch_fever.completed_life = actor.life_generation;
  }
  actor.life_settled = false;
  actor.death_stage = 0U;
  actor.respawn_point.reset();
  actor.respawn_wait.reset();
  actor.protection_report.clear();
  return AcceptedResult(actor.participant.player_id, packet);
}

void CombatRelay::BeginHotbloodRespawn(
    ActorState& actor, std::chrono::steady_clock::time_point now) {
  actor.respawn_wait.reset();
  if ((mode_id_ != 13U && mode_id_ != 14U) || actor.controller ||
      actor.life_state != CombatLifeState::kDefeated)
    return;
  const auto stats = domain::NativeHotbloodStats(
      mode_id_, actor.participant.fighter, actor.participant.equipment_items);
  actor.respawn_wait = ActorState::RespawnWait{
      .life = actor.life_generation,
      .duration_ms = stats.respawn_ms,
      .due = now + std::chrono::milliseconds{std::max(0, stats.respawn_ms)}};
}

std::vector<CombatDelivery> CombatRelay::TickRespawns(
    std::chrono::steady_clock::time_point now) {
  std::vector<CombatDelivery> result;
  for (auto& [player, actor] : actors_) {
    if (!actor.respawn_wait || !actor.respawn_wait->pending_point ||
        actor.respawn_wait->life != actor.life_generation ||
        actor.life_state != CombatLifeState::kDefeated ||
        actor.death_stage != 2U || actor.respawn_point ||
        !actor.participant.ordinary_respawn_allowed ||
        now < actor.respawn_wait->due)
      continue;
    actor.respawn_point = actor.respawn_wait->pending_point;
    actor.respawn_wait->pending_point.reset();
    result.push_back(
        {.recipient = player,
         .message = {
             .opcode = 707U,
             .body = protocol::EncodeNativeRespawnAuthorization(
                 actor.participant.actor_object_id, *actor.respawn_point)}});
  }
  return result;
}

std::optional<std::chrono::steady_clock::time_point>
CombatRelay::RespawnDeadline() const {
  std::optional<std::chrono::steady_clock::time_point> result;
  for (const auto& [player, actor] : actors_) {
    static_cast<void>(player);
    if (actor.respawn_wait && actor.respawn_wait->pending_point &&
        actor.respawn_wait->life == actor.life_generation &&
        actor.life_state == CombatLifeState::kDefeated &&
        (!result || actor.respawn_wait->due < *result))
      result = actor.respawn_wait->due;
  }
  return result;
}

bool CombatRelay::ConsumeActionQuota(
    ActorState& actor, std::chrono::steady_clock::time_point now) noexcept {
  if (!actor.rate_window.has_value()) {
    actor.rate_window = now;
    actor.messages_in_window = 0U;
  } else {
    const auto window = actor.rate_window.value();
    if (now < window || now - window >= std::chrono::seconds{1}) {
      actor.rate_window = now;
      actor.messages_in_window = 0U;
    }
  }
  if (actor.messages_in_window >= kMaximumActionsPerSecond) {
    return false;
  }
  ++actor.messages_in_window;
  return true;
}

void CombatRelay::PruneProjectiles(std::chrono::steady_clock::time_point now) {
  std::erase_if(storms_, [now](const auto& entry) {
    return now < entry.second.created ||
           now - entry.second.created > kProjectileRetention;
  });
  std::erase_if(projectiles_, [now](const auto& entry) {
    return now < entry.second.created_at ||
           now - entry.second.created_at > kProjectileRetention;
  });
  std::erase_if(weapon_objects_, [now](const auto& entry) {
    return now < entry.second.created_at ||
           now - entry.second.created_at > kProjectileRetention;
  });
}

CombatRelayResult CombatRelay::AcceptedResult(
    domain::PlayerId sender, const protocol::Packet& packet) const {
  const auto source = actors_.find(sender);
  if (source != actors_.end() && source->second.controller.has_value()) {
    sender = *source->second.controller;
  }
  CombatRelayResult result{
      .accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
  result.deliveries.reserve(actors_.size() + observers_.size());
  for (const auto& [player_id, actor] : actors_) {
    static_cast<void>(actor);
    if (player_id != sender && !actor.controller.has_value()) {
      result.deliveries.push_back(
          {.recipient = player_id,
           .message = {.opcode = packet.opcode, .body = packet.body}});
    }
  }
  for (const auto observer : observers_) {
    if (observer != sender)
      result.deliveries.push_back(
          {.recipient = observer,
           .message = {.opcode = packet.opcode, .body = packet.body}});
  }
  return result;
}

bool CombatRelay::IsMovementOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32769U || opcode == 32885U;
}

CombatRelayResult CombatRelay::RejectedResult(CombatRejectReason reason) {
  return {.accepted = false, .reject_reason = reason, .deliveries = {}};
}

std::uint64_t CombatRelay::ObservedNativeConstructors(
    domain::PlayerId receiver, std::uint32_t owner_object) const noexcept {
  const auto found =
      native_constructor_ordinals_.find({receiver, owner_object});
  return found == native_constructor_ordinals_.end() ? 0U : found->second;
}

bool CombatRelay::NativeLoopbackOrderUncertain(
    std::uint32_t owner_object) const noexcept {
  return (mode_id_ != 17U && mode_id_ != 21U) ||
         !object_owners_.contains(owner_object) ||
         native_loopback_gaps_.contains(owner_object);
}

bool CombatRelay::IsNativeConstructorOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32925U || opcode == 32898U || opcode == 33281U ||
         opcode == 32772U || opcode == 32900U || opcode == 32832U ||
         opcode == 32930U;
}

void CombatRelay::MarkNativeConstructorGap(domain::PlayerId source,
                                           std::uint16_t opcode) {
  if ((mode_id_ != 17U && mode_id_ != 21U) ||
      !IsNativeConstructorOpcode(opcode))
    return;
  const auto found = actors_.find(source);
  if (found != actors_.end() && !found->second.controller)
    native_loopback_gaps_.insert(found->second.participant.actor_object_id);
}

application::ServiceResult<CombatRelayResult>
CombatRelay::FinishNativeConstructorAttempt(
    domain::PlayerId source, const protocol::Packet& packet,
    application::ServiceResult<CombatRelayResult> result) {
  const auto accepted = std::get_if<CombatRelayResult>(&result);
  if (!accepted || !accepted->accepted)
    MarkNativeConstructorGap(source, packet.opcode);
  return result;
}

void CombatRelay::RecordNativeConstructors(CombatRelayResult& result,
                                           const ActorState& source,
                                           const protocol::Packet& packet,
                                           std::uint32_t count) {
  // These facts describe actual incoming peer constructors, not client-local
  // high-counter prediction. Unknown NPC families/constructors are not
  // inferred.
  if (!result.accepted || source.controller || count == 0U || count > 64U)
    return;
  const auto object = source.participant.actor_object_id;
  if (object !=
      1'000'000U +
          static_cast<std::uint32_t>(source.participant.seat_index) * 100'000U)
    return;
  if (mode_id_ == 17U || mode_id_ == 21U) {
    // 660430 saves default1;65F150 restores the current normal policy1.
    // Under that policy65F220 queues routing1 and sendsTCP. Original5DD4B0
    // excludes both decimal17 and decimal21 (0x17 there means decimal23).
    // Track the owner's local queue without adding a duplicate server echo.
    // This ordinal omits initial Character damage objects and followers, not
    // just actors. It is never an object ID, an execution ACK or proof of a
    // complete scene counter.
    auto& ordinal =
        native_constructor_ordinals_[{source.participant.player_id, object}];
    if (ordinal <= std::numeric_limits<std::uint64_t>::max() - count) {
      result.native_constructors.push_back(
          {source.participant.player_id, object, packet.opcode, count, ordinal,
           source.life_generation,
           mode_id_ == 17U ? NativeConstructorLane::kMode17OwnerLoopback
                           : NativeConstructorLane::kMode21OwnerLoopback,
           native_loopback_gaps_.contains(object)});
      ordinal += count;
    }
  }
  std::set<domain::PlayerId> receivers;
  for (const auto& delivery : result.deliveries) {
    if (delivery.message.opcode != packet.opcode ||
        delivery.message.body != packet.body ||
        !receivers.insert(delivery.recipient).second)
      continue;
    auto& ordinal = native_constructor_ordinals_[{delivery.recipient, object}];
    if (ordinal > std::numeric_limits<std::uint64_t>::max() - count) continue;
    result.native_constructors.push_back({delivery.recipient, object,
                                          packet.opcode, count, ordinal,
                                          source.life_generation});
    ordinal += count;
  }
}

void CombatRelay::RecordLifecycleStatistics(
    const ActorState& victim,
    const protocol::NativeLifecycleRequest& request) noexcept {
  if (request.kind == protocol::NativeLifecycleKind::kDefeat &&
      request.direct_defeat) {
    auto victim_actor = actors_.find(victim.participant.player_id);
    if (victim_actor != actors_.end()) {
      SaturatedIncrement(victim_actor->second.performance.deaths);
      ++victim_actor->second.item_inventory_generation;
      victim_actor->second.item_escape.reset();
    }
  }
  if (!request.related_object_id.has_value()) {
    return;
  }
  const auto owner = object_owners_.find(request.related_object_id.value());
  if (owner == object_owners_.end()) {
    return;
  }
  auto credited = owner->second;
  if (const auto turret = turrets_.find(*request.related_object_id);
      turret != turrets_.end()) {
    // 84C760 inherits the summoner's CharacterBase identity. 7D9800 maps
    // NpcTurret (+280=true) back through 7C0850 before player callbacks.
    // Retain the original scene ID on the wire and use only our bound owner.
    if (request.kind != protocol::NativeLifecycleKind::kDefeat) return;
    credited = turret->second.owner;
  }
  if (credited == victim.participant.player_id) return;
  auto related = actors_.find(credited);
  if (related == actors_.end()) {
    return;
  }

  if (request.kind == protocol::NativeLifecycleKind::kDefeat) {
    if (individual_scoring_ ||
        related->second.participant.team_code != victim.participant.team_code) {
      auto& value = request.direct_defeat ? related->second.performance.pangs
                                          : related->second.performance.bubbles;
      SaturatedIncrement(value);
      if (request.direct_defeat)
        related->second.deathmatch_fever.respawns_without_pang = 0U;
    }
    return;
  }

  if (!individual_scoring_ &&
      request.kind == protocol::NativeLifecycleKind::kRecovery &&
      related->second.participant.team_code == victim.participant.team_code) {
    SaturatedIncrement(related->second.performance.helps);
  }
}

CombatRelayResult CombatRelay::HandleRescue(
    domain::PlayerId sender, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeNativeRescue(packet.opcode, packet.body);
  const auto victim = request.victim;
  const auto helper = request.helper;
  if (victim == helper || !object_owners_.contains(victim) ||
      !object_owners_.contains(helper)) {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  auto& target = actors_.at(object_owners_.at(victim));
  auto& source = actors_.at(object_owners_.at(helper));
  const auto& owner = packet.opcode == 33283U ? source : target;
  if (owner.controller.value_or(owner.participant.player_id) != sender ||
      individual_scoring_ ||
      target.participant.team_code != source.participant.team_code ||
      (target.controller.has_value() != source.controller.has_value() &&
       !(mode_id_ == kTrainingMode && target.training_dummy &&
         !source.controller && target.controller == sender))) {
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  }
  std::erase_if(rescues_, [now](const auto& entry) {
    return now < entry.second.time ||
           now - entry.second.time > std::chrono::seconds{5};
  });
  if (packet.opcode == 33283U) {
    if (target.life_state != CombatLifeState::kBubbled ||
        source.life_state != CombatLifeState::kAlive ||
        rescues_.contains(victim)) {
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (!ConsumeActionQuota(source, now)) {
      return RejectedResult(CombatRejectReason::kRateLimited);
    }
    rescues_.emplace(victim, PendingRescue{.helper = helper,
                                           .generation = target.life_generation,
                                           .time = now,
                                           .recovered = false,
                                           .kind = request.kind});
  } else {
    const auto found = rescues_.find(victim);
    if (found == rescues_.end() || found->second.helper != helper ||
        found->second.generation != target.life_generation) {
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    }
    if (packet.opcode == 32904U) {
      if (found->second.recovered ||
          target.life_state != CombatLifeState::kBubbled) {
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      }
      target.life_state = CombatLifeState::kAlive;
      target.life_settled = false;
      target.protection_report.clear();
      target.death_stage = 0U;
      target.respawn_point.reset();
      target.pending_kick.reset();
      target.bubble_impacts.clear();
      found->second.recovered = true;
    } else {
      if (!found->second.recovered || found->second.kind != request.kind ||
          target.life_state != CombatLifeState::kAlive) {
        return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
      }
      RecordLifecycleStatistics(
          target, {.kind = protocol::NativeLifecycleKind::kRecovery,
                   .actor_object_id = victim,
                   .related_object_id = helper});
      rescues_.erase(found);
    }
  }
  auto result = AcceptedResult(sender, packet);
  // A private training connection owns both the helper and its dummy. Native
  // 7DC040 / 7D9330 / 7D9460 still need the ordinary request/recovery/effect.
  if (mode_id_ == kTrainingMode && target.training_dummy &&
      target.controller == sender)
    result.deliveries.push_back({sender, {packet.opcode, packet.body}});
  return result;
}

bool CombatRelay::IsPeerEffectOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32824U || opcode == 32832U || opcode == 33275U;
}

CombatRelayResult CombatRelay::RelayPeerEffect(
    domain::PlayerId sender, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  try {
    auto result = RelayPeerEffectChecked(sender, packet, now);
    if (!result.accepted) MarkNativeConstructorGap(sender, packet.opcode);
    return result;
  } catch (...) {
    MarkNativeConstructorGap(sender, packet.opcode);
    throw;
  }
}

CombatRelayResult CombatRelay::RelayPeerEffectChecked(
    domain::PlayerId sender, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  const std::size_t size = packet.opcode == 32824U   ? 16U
                           : packet.opcode == 32832U ? 32U
                                                     : 8U;
  if (!IsPeerEffectOpcode(packet.opcode) || packet.body.size() != size)
    ProtocolViolation("peer effect size is invalid");
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  const auto actor = actors_.find(sender);
  if (actor == actors_.end() || actor->second.controller)
    return RejectedResult(CombatRejectReason::kUnknownRelatedActor);
  if (object != actor->second.participant.actor_object_id)
    ProtocolViolation("peer effect belongs to another character");
  const bool banana = mode_id_ == 21U && packet.opcode == 32832U;
  if (banana) {
    auto& state = actor->second;
    if (state.life_state != CombatLifeState::kAlive || !state.runner_banana ||
        state.runner_banana->life != state.life_generation ||
        state.runner_banana->remaining == 0U ||
        now >= state.runner_banana->expires || !state.position ||
        reader.ReadU32() != 0U)
      return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const auto point = reader.ReadF32();
      if (!std::isfinite(point) ||
          std::abs(point - (*state.position)[axis]) > 50.0F)
        return RejectedResult(CombatRejectReason::kUnknownTarget);
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto velocity = reader.ReadF32();
      if (!std::isfinite(velocity) || std::abs(velocity) > 5000.0F)
        ProtocolViolation("BFRunner banana velocity is invalid");
    }
  }
  if (!ConsumeActionQuota(actor->second, now)) {
    return RejectedResult(CombatRejectReason::kRateLimited);
  }
  auto result = AcceptedResult(sender, packet);
  if (banana) --actor->second.runner_banana->remaining;
  if (packet.opcode == 32832U)
    RecordNativeConstructors(result, actor->second, packet, 1U);
  return result;
}

}  // namespace bf::runtime
