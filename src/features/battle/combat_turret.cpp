#include <algorithm>

#include "features/battle/combat_relay.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
using namespace std::chrono_literals;
using Time = std::chrono::steady_clock::time_point;
constexpr auto kTurretFireInterval = 384ms;
// Server transport policy: two native intervals may arrive together with the
// first shot. This allowance never accumulates while a turret is idle.
constexpr auto kTurretFireJitter = 2 * kTurretFireInterval;

CombatRelayResult Reject(
    CombatRejectReason reason = CombatRejectReason::kUnrecoveredAction) {
  return {.accepted = false, .reject_reason = reason};
}
}  // namespace

void CombatRelay::ResetTurrets() noexcept {
  for (const auto& [id, turret] : turrets_) {
    static_cast<void>(turret);
    const auto owner = object_owners_.find(id);
    if (owner != object_owners_.end()) {
      actors_.erase(owner->second);
      object_owners_.erase(owner);
      object_ids_.erase(id);
    }
  }
  turrets_.clear();
  pending_turrets_.clear();
  for (auto& [id, actor] : actors_) {
    static_cast<void>(id);
    actor.turret_throw.reset();
    actor.turret_impacts.clear();
  }
}

CombatRelayResult CombatRelay::SummonTurret(
    ActorState& owner, const protocol::Packet& packet,
    const protocol::NativeAreaCast& request, Time now) {
  if (owner.controller || mode_id_ != 3U || request.type != 137U)
    return Reject();
  PruneProjectiles(now);
  std::erase_if(pending_turrets_, [now](const auto& p) {
    return now < p.created || now - p.created > 7s;
  });
  const auto source =
      std::ranges::find_if(projectiles_, [&](const auto& entry) {
        const auto& p = entry.second;
        return p.owner_object_id == owner.participant.actor_object_id &&
               p.area_type == 137U && !p.terminal;
      });
  if (source == projectiles_.end() || pending_turrets_.size() >= 128U ||
      turrets_.size() >= 4096U)
    return Reject(CombatRejectReason::kUnknownProjectile);
  if (!ConsumeActionQuota(owner, now))
    return Reject(CombatRejectReason::kRateLimited);
  // 631760 emits 32930 only. Unlike Ward, no subsequent 32900 identifies this
  // throw.
  source->second.terminal = true;
  pending_turrets_.push_back(
      {owner.participant.player_id, now, request.position});
  auto result = AcceptedResult(owner.participant.player_id, packet);
  // Native 32930/137 -> 7E0CF0 -> 925230 -> 84C760 constructs the turret and
  // its two Character melee damagers. All three use the summoner's low counter.
  RecordNativeConstructors(result, owner, packet, 3U);
  return result;
}

std::optional<CombatRelayResult> CombatRelay::HandleTurret(
    domain::PlayerId sender, const protocol::Packet& packet, Time now) {
  const bool dedicated = packet.opcode == 32898U || packet.opcode == 32895U ||
                         packet.opcode == 32778U;
  if (mode_id_ != 3U) {
    if (mode_id_ == 21U && packet.opcode == 32898U)
      return std::nullopt;  // Original BFRunner147/234 uses7C7B40, not the
                            // Item-mode turret source.
    if (dedicated) {
      static_cast<void>(
          protocol::DecodeNativeAction(packet.opcode, packet.body));
      return Reject();
    }
    return std::nullopt;
  }
  if (protocol::IsNativeLifecycleOpcode(packet.opcode)) {
    const auto life =
        protocol::DecodeNativeLifecycle(packet.opcode, packet.body);
    if (turrets_.contains(life.actor_object_id))
      return Reject(CombatRejectReason::kInvalidLifecycleState);
    if (life.related_object_id && turrets_.contains(*life.related_object_id)) {
      const auto& victim = actors_.at(sender);
      const auto impact = victim.turret_impacts.find(*life.related_object_id);
      // 8276C0 reports GoDamager+852, including the turret scene ID. Accept
      // the victim's decision only after a real impact in this alive life.
      // Bubble shot bursts use their own later impact in handle_bubble_action.
      if (life.kind != protocol::NativeLifecycleKind::kDefeat ||
          victim.participant.actor_object_id != life.actor_object_id ||
          victim.life_state != CombatLifeState::kAlive ||
          impact == victim.turret_impacts.end() ||
          impact->second.generation != victim.life_generation ||
          now < impact->second.requested_at ||
          now - impact->second.requested_at > 5s)
        return Reject(CombatRejectReason::kInvalidLifecycleState);
    }
  }
  if (packet.opcode == 32774U) {
    const auto remove =
        protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
    // A scene object and a damage cookie may share a numeric value. NpcTurret
    // normally ends through 32778; never let its scene cleanup consume a
    // bullet.
    if (turrets_.contains(remove.cookie)) return Reject();
  }
  if (!protocol::IsNativeActionOpcode(packet.opcode)) return std::nullopt;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  auto& owner = actors_.at(sender);
  auto turret = turrets_.find(request.actor_object_id);
  if (packet.opcode == 32898U) {
    const auto& fire = *request.explicit_fire;
    const bool throwing =
        request.actor_object_id == owner.participant.actor_object_id &&
        !owner.controller;
    ActorState* source = &owner;
    if (throwing && fire.weapon == 5026U) {
      std::erase_if(owner.airstrikes, [now](const auto& strike) {
        return now >= strike.created + 5s || strike.remaining == 0U;
      });
      const auto strike =
          std::ranges::find_if(owner.airstrikes, [now](const auto& entry) {
            return now >= entry.created && entry.remaining != 0U;
          });
      if (strike == owner.airstrikes.end() ||
          fire.variant != owner.participant.team_code ||
          fire.target != 0xffffffffU || fire.flags != 0U)
        return Reject(CombatRejectReason::kInvalidLifecycleState);
      PruneProjectiles(now);
      if (ContainsDamagerCookie(fire.cookie))
        return Reject(CombatRejectReason::kDuplicateProjectile);
      if (projectiles_.size() >= 8192U)
        return Reject(CombatRejectReason::kProjectileLimit);
      if (!ConsumeActionQuota(owner, now))
        return Reject(CombatRejectReason::kRateLimited);
      auto& shot = projectiles_[fire.cookie];
      shot.owner_object_id = request.actor_object_id;
      shot.created_at = now;
      shot.weapon_bar = 3U;
      shot.area_type = 136U;
      --strike->remaining;
      ++owner.accepted_shots;
      auto result = AcceptedResult(sender, packet);
      // Original 7D4AB0 -> 7BDA90 creates one GoGrenade for WeaponSpec5026.
      RecordNativeConstructors(result, owner, packet, 1U);
      return result;
    }
    if (throwing) {
      if (owner.life_state != CombatLifeState::kAlive || !owner.turret_throw ||
          owner.turret_throw->first != owner.life_generation ||
          now >= owner.turret_throw->second ||
          now < owner.turret_throw->second - 10s ||
          fire.weapon != (owner.participant.team_code == 1U ? 5410U : 5411U) ||
          fire.target != 0xffffffffU || fire.flags != 0U)
        return Reject(CombatRejectReason::kInvalidLifecycleState);
    } else {
      if (turret == turrets_.end() || turret->second.owner != sender ||
          turret->second.ended || turret->second.state != 3U ||
          now < turret->second.changed + 750ms ||
          now >= turret->second.created + 7250ms || fire.weapon != 2206U)
        return Reject(CombatRejectReason::kInvalidLifecycleState);
      const auto target = object_owners_.find(fire.target);
      if (target == object_owners_.end() ||
          actors_.at(target->second).participant.team_code ==
              owner.participant.team_code ||
          actors_.at(target->second).life_state != CombatLifeState::kAlive)
        return Reject(CombatRejectReason::kUnknownTarget);
      source = &actors_.at(object_owners_.at(request.actor_object_id));
    }
    PruneProjectiles(now);
    if (ContainsDamagerCookie(fire.cookie))
      return Reject(CombatRejectReason::kDuplicateProjectile);
    if (!throwing && turret->second.native_fire_tick) {
      const auto& timing = turret->second;
      // 7C7B40 preserves unrelated high bits; only the low24 are elapsed game
      // milliseconds. A1F140/4A9BE0 enforce WeaponSpec2206's rounded 384ms.
      const auto elapsed =
          (fire.token - *timing.native_fire_tick) & 0x00ffffffU;
      if (elapsed < 384U || elapsed > 7250U ||
          now < timing.last_fire_received ||
          now + kTurretFireJitter < timing.next_fire)
        return Reject(CombatRejectReason::kRateLimited);
    }
    if (projectiles_.size() >= 8192U)
      return Reject(CombatRejectReason::kProjectileLimit);
    if (!ConsumeActionQuota(owner, now))
      return Reject(CombatRejectReason::kRateLimited);
    auto& shot = projectiles_[fire.cookie];
    shot.owner_object_id = request.actor_object_id;
    shot.created_at = now;
    shot.weapon_bar = throwing ? 3U : 4U;
    shot.area_type = throwing ? 137U : 0U;
    ++source->accepted_shots;
    if (throwing)
      owner.turret_throw.reset();
    else {
      auto& timing = turret->second;
      timing.native_fire_tick = fire.token & 0x00ffffffU;
      timing.last_fire_received = now;
      timing.next_fire = std::max(now, timing.next_fire) + kTurretFireInterval;
    }
    auto result = AcceptedResult(sender, packet);
    // 5410/5411 construct one GoGrenadeTurret; 2206 constructs one GoBullet
    // through 7D4E40. The turret ID selects its summoner's low-counter range,
    // so record against the authenticated player, retaining the wire actor ID.
    RecordNativeConstructors(result, owner, packet, 1U);
    return result;
  }
  if (packet.opcode == 32810U &&
      request.actor_object_id != owner.participant.actor_object_id) {
    const auto& change = *request.weapon_switch;
    if (turret != turrets_.end()) {
      if (turret->second.owner != sender || turret->second.ended)
        return Reject(CombatRejectReason::kInvalidLifecycleState);
      return Reject(CombatRejectReason::kDuplicateAction);
    }
    const auto base = owner.participant.actor_object_id;
    if (owner.controller || request.actor_object_id <= base ||
        request.actor_object_id - base >= 100000U ||
        object_ids_.contains(request.actor_object_id) ||
        change.template_id != 2206U || change.bar_index != 4U ||
        !change.select || turrets_.size() >= 4096U)
      return Reject();
    const auto pending =
        std::ranges::find_if(pending_turrets_, [&](const auto& p) {
          return p.owner == sender && now >= p.created && now - p.created <= 7s;
        });
    if (pending == pending_turrets_.end())
      return Reject(CombatRejectReason::kUnknownRelatedActor);
    if (!ConsumeActionQuota(owner, now))
      return Reject(CombatRejectReason::kRateLimited);
    const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) |
                                     request.actor_object_id};
    ActorState npc;
    npc.participant = {.player_id = synthetic,
                       .actor_object_id = request.actor_object_id,
                       .team_code = owner.participant.team_code,
                       .weapon_templates = {std::nullopt, std::nullopt,
                                            std::nullopt, std::nullopt, 2206U},
                       .ordinary_respawn_allowed = false};
    npc.controller = sender;
    npc.position = pending->position;
    npc.selected_weapon_bar = 4U;
    actors_.emplace(synthetic, std::move(npc));
    object_ids_.insert(request.actor_object_id);
    object_owners_.emplace(request.actor_object_id, synthetic);
    turrets_.emplace(request.actor_object_id,
                     TurretState{sender, pending->created, pending->created});
    pending_turrets_.erase(pending);
    return AcceptedResult(sender, packet);
  }
  if (packet.opcode == 32895U || packet.opcode == 32778U) {
    if (turret == turrets_.end() || turret->second.owner != sender ||
        turret->second.ended)
      return Reject(CombatRejectReason::kUnknownRelatedActor);
    auto& state = turret->second;
    const auto value = *request.scene_value;
    if (now < state.created)
      return Reject(CombatRejectReason::kInvalidLifecycleState);
    if (packet.opcode == 32895U) {
      const bool valid =
          value == 2U ? state.state == 0U && now < state.created + 7250ms
          : value == 3U
              ? (state.state == 0U || state.state == 2U) &&
                    (now >= state.created + 2750ms ||
                     (state.state == 2U && now >= state.changed + 750ms)) &&
                    now < state.created + 7250ms
          : value == 5U ? state.state != 5U && now >= state.created + 6750ms
                        : false;
      if (!valid) return Reject(CombatRejectReason::kInvalidLifecycleState);
      if (!ConsumeActionQuota(owner, now))
        return Reject(CombatRejectReason::kRateLimited);
      state.state = value;
      state.changed = now;
    } else {
      // 93B110 expiry uses self; 93A3A0 damage destruction uses the attacker.
      bool proven = value == request.actor_object_id && state.state == 5U;
      if (!proven && object_owners_.contains(value) &&
          value != request.actor_object_id) {
        const auto& attacker = actors_.at(object_owners_.at(value));
        const auto impact = [&](const auto& entry) {
          const auto& p = entry.second;
          return p.owner_object_id == value &&
                 p.targets.contains(request.actor_object_id) &&
                 now >= p.created_at && now - p.created_at <= 30s;
        };
        proven =
            attacker.participant.team_code != owner.participant.team_code &&
            (std::ranges::any_of(projectiles_, impact) ||
             std::ranges::any_of(weapon_objects_, impact));
      }
      if (!proven) return Reject(CombatRejectReason::kInvalidLifecycleState);
      if (!ConsumeActionQuota(owner, now))
        return Reject(CombatRejectReason::kRateLimited);
      state.ended = true;
      actors_.at(object_owners_.at(request.actor_object_id)).life_state =
          CombatLifeState::kDefeated;
    }
    return AcceptedResult(sender, packet);
  }
  if (turret != turrets_.end()) {
    if (turret->second.owner != sender || turret->second.ended)
      return Reject(CombatRejectReason::kInvalidLifecycleState);
    // NpcTurret::+932 is false: it never uses the compressed player-fire
    // branch.
    if (!request.position &&
        request.kind != protocol::NativeActionKind::kHealthReport &&
        request.kind != protocol::NativeActionKind::kStopShoot)
      return Reject();
  }
  return std::nullopt;
}
}  // namespace bf::runtime
