#include <algorithm>

#include "features/battle/combat_relay.h"
#include "features/battle/native_item_rules.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {

void CombatRelay::ActivateAirstrike(domain::PlayerId player,
                                    std::chrono::steady_clock::time_point now) {
  const auto source = actors_.find(player);
  if (mode_id_ != 3U || source == actors_.end() || source->second.controller ||
      source->second.life_state != CombatLifeState::kAlive)
    return;
  // The authenticated32894 effect precedes84FE60/928D40/9293E0. Only plane
  // indices1/3 release five5026 bombs each;708 alone has created no aircraft.
  auto& strikes = source->second.airstrikes;
  std::erase_if(strikes, [now](const auto& strike) {
    return now >= strike.created + std::chrono::seconds{5} ||
           strike.remaining == 0U;
  });
  if (strikes.size() < 16U) strikes.push_back({now});
}

void CombatRelay::RecordItemEffectConstructors(CombatRelayResult& result,
                                               domain::PlayerId source,
                                               const protocol::Packet& packet) {
  if (mode_id_ != 3U || packet.opcode != 32894U || packet.body.size() != 8U)
    return;
  const auto actor = actors_.find(source);
  if (actor == actors_.end()) return;
  protocol::PacketReader reader{packet.body};
  if (reader.ReadU32() != actor->second.participant.actor_object_id) return;
  const auto type = reader.ReadU32();
  // 7E0F40 ->7E0AA0 ->925390 creates five GoAirBombs for136, one
  // GoShieldField for141. 714/715 only starts the local32894 handshake.
  // The caller has consumed MatchObjectives' once-only item entitlement and
  // supplied the actual outgoing recipients. This records no scene-ID
  // authority.
  const auto count = type == 136U ? 5U : type == 141U ? 1U : 0U;
  RecordNativeConstructors(result, actor->second, packet, count);
}

CombatRelayResult CombatRelay::StageAreaCast(
    ActorState& actor, const protocol::Packet& packet,
    const protocol::NativeAreaCast& request,
    std::chrono::steady_clock::time_point now) {
  const auto reject = [](CombatRejectReason reason) {
    return CombatRelayResult{.accepted = false, .reject_reason = reason};
  };
  // 7BD400 selects GoGrenadeSmoke before the mode-specific Ward factory.
  // Only a real equipped 5061/5161 throw freezes visual237 capability. The
  // damaging248 field and unidentified scene-object cleanup remain unbound.
  const bool visual_smoke = request.type == 237U;
  const bool ward = mode_id_ == 22U && request.type == 238U;
  if (actor.controller || (!visual_smoke && !ward))
    return reject(CombatRejectReason::kUnrecoveredAction);
  PruneProjectiles(now);
  if (actor.pending_area_cast) {
    std::erase_if(actor.pending_area_cast->sources, [&](const auto& source) {
      const auto found = projectiles_.find(source.first);
      return found == projectiles_.end() || found->second.terminal ||
             found->second.created_at != source.second;
    });
    if (actor.pending_area_cast->sources.empty())
      actor.pending_area_cast.reset();
  }
  if (actor.pending_area_cast && now >= actor.pending_area_cast->created &&
      now - actor.pending_area_cast->created <= std::chrono::seconds{5})
    return reject(CombatRejectReason::kDuplicateAction);
  std::map<std::uint32_t, std::chrono::steady_clock::time_point> sources;
  for (const auto& [cookie, projectile] : projectiles_) {
    if (projectile.owner_object_id == actor.participant.actor_object_id &&
        projectile.area_type == request.type && !projectile.terminal)
      sources.emplace(cookie, projectile.created_at);
  }
  if (sources.empty()) return reject(CombatRejectReason::kUnknownProjectile);
  if (!ConsumeActionQuota(actor, now))
    return reject(CombatRejectReason::kRateLimited);
  actor.pending_area_cast =
      ActorState::PendingAreaCast{.packet = packet,
                                  .request = request,
                                  .created = now,
                                  .sources = std::move(sources)};
  return {.accepted = true};
}

void CombatRelay::CommitAreaCast(ActorState& actor,
                                 const protocol::Packet& detonation,
                                 std::uint32_t source,
                                 std::chrono::steady_clock::time_point now,
                                 CombatRelayResult& result) {
  if (!actor.pending_area_cast) return;
  auto& pending = *actor.pending_area_cast;
  if (now < pending.created ||
      now - pending.created > std::chrono::seconds{5}) {
    actor.pending_area_cast.reset();
    return;
  }
  const auto expected = pending.sources.find(source);
  if (expected == pending.sources.end()) return;
  if (projectiles_.at(source).created_at != expected->second) {
    pending.sources.erase(expected);
    if (pending.sources.empty()) actor.pending_area_cast.reset();
    return;
  }
  protocol::PacketReader reader{std::span{detonation.body}.subspan(4U, 12U)};
  float distance_squared = 0.0F;
  for (auto axis : pending.request.position) {
    const auto delta = axis - reader.ReadF32();
    distance_squared += delta * delta;
  }
  // Five seconds and fifty units are server correlation allowances, not native
  // rules.
  if (distance_squared <= 50.0F * 50.0F) {
    auto cast = AcceptedResult(actor.participant.player_id, pending.packet);
    // 84D000/84CE40 construct one original field on each actual receiving peer.
    RecordNativeConstructors(cast, actor, pending.packet, 1U);
    result.deliveries.insert(result.deliveries.begin(), cast.deliveries.begin(),
                             cast.deliveries.end());
    result.native_constructors.insert(result.native_constructors.end(),
                                      cast.native_constructors.begin(),
                                      cast.native_constructors.end());
  }
  actor.pending_area_cast.reset();
}

void CombatRelay::AuthorizeItemEffects(
    domain::PlayerId player, std::uint32_t type, std::uint32_t mode,
    std::chrono::steady_clock::time_point now) {
  if (mode == 21U && mode_id_ != 21U) return;
  const auto source = actors_.find(player);
  if (source == actors_.end() || source->second.controller ||
      (source->second.life_state != CombatLifeState::kAlive &&
       !(mode == 3U && source->second.life_state == CombatLifeState::kBubbled &&
         ItemUsableInBubble(type)))) {
    return;
  }
  if (mode == 3U && (type == 109U || type == 127U) &&
      source->second.life_state == CombatLifeState::kBubbled) {
    // 938840/927BE0 -> 8BAD50 -> 7C86B0 emits 32806 with helper=-2.
    source->second.item_escape = {source->second.life_generation,
                                  now + std::chrono::seconds{10}};
  }
  // ItemSpec4.xml + 925390/927BE0/9273C0/DD4A60; other sources (weapon hits,
  // persistent magnetic fields, script effects) require their own provenance.
  const bool area = mode == 3U && (type == 115U || type == 126U);
  if ((type == 115U || type == 126U) && !area) return;
  std::uint32_t attribute = 0U, duration = 0U;
  auto window = std::chrono::milliseconds{10'000};
  float radius = 0.0F;
  std::uint16_t opcode = 32828U;
  if (type == 10U || type == 11U || type == 15U || type == 16U || type == 17U) {
    if (source->second.participant.fighter != 4U) return;
    type = 120U;  // 925A90 grants this follow-up only to fighter 4.
  }
  switch (type) {
    case 147U:
    case 234U:
      if (mode == 21U && mode_id_ == 21U)
        source->second.runner_throw = {source->second.life_generation,
                                       type == 147U ? 5047U : 5046U,
                                       now + std::chrono::seconds{10}};
      return;
    case 128U:
      if (mode == 21U && mode_id_ == 21U)
        source->second.runner_banana = {source->second.life_generation, 3U,
                                        now + std::chrono::seconds{10}};
      return;
    case 102U:
      if (mode != 21U || mode_id_ != 21U) return;
      attribute = 3U;
      duration = 7000U;
      break;
    case 136U:
      // MatchObjectives must validate and consume the separate32894 entitlement
      // before activate_airstrike opens the projectile budget.
      return;
    case 300U:
      if (mode == 3U && mode_id_ == 3U)
        source->second.turret_throw = {source->second.life_generation,
                                       now + std::chrono::seconds{10}};
      return;
    case 115U:
      attribute = 4U;
      duration = 3000U;
      window = std::chrono::milliseconds{
          750};  // native 500 ms + 250 ms transport allowance
      radius = 100.0F;
      break;
    case 126U:
      attribute = 12U;
      duration = 1500U;
      window = std::chrono::milliseconds{
          1750};  // native 1500 ms + transport allowance
      radius = 200.0F;
      break;
    case 101U:
      attribute = 2U;
      duration = mode == 16U || mode == 21U ? 6000U : 7000U;
      break;
    case 108U:
      attribute = 7U;
      duration = 6000U;
      break;
    case 112U:
      attribute = 8U;
      duration = 13000U;
      if (mode == 3U && mode_id_ == 3U) {
        source->second.bazooka.reset();
        source->second.giant_weapon = ActorState::GiantWeapon{
            .life = source->second.life_generation,
            .expires = now + std::chrono::milliseconds{13250}};
      }
      break;
    case 122U:
      source->second.giant_weapon.reset();
      attribute = 9U;
      duration = 11000U;
      break;
    case 127U:
      attribute = 15U;
      duration = 4000U;
      break;
    case 130U:
      attribute = 16U;
      duration = 4000U;
      break;
    case 138U:
    case 146U:
      if ((type == 138U ? mode != 3U : mode != 16U && mode != 21U) ||
          (mode != 21U && source->second.participant.weapon_templates[4U]))
        return;
      // 83AFD7 equips ItemSpecItem.weaponId=9021 in temporary bar 4.
      // WeaponSpecDetail Item 9021: ammoCount=totalAmmoCount=2.
      // BFRunner ItemSpecBFRunner/WeaponSpecDetail:9100 has one shot.
      source->second.bazooka = {source->second.life_generation, mode == 21U ? 1U
                                                                : type == 146U
                                                                    ? 3U
                                                                    : 2U};
      source->second.bazooka_template = mode == 21U    ? 9100U
                                        : type == 146U ? 9025U
                                                       : 9021U;
      source->second.giant_weapon.reset();
      attribute = 22U;
      duration = 0xFFFFFFFFU;
      break;
    case 302U:
      attribute = 39U;
      duration = 5000U;
      break;
    case 120U:
    case 129U:
    case 229U:
      opcode = 32829U;
      break;
    default:
      return;
  }
  std::erase_if(item_effects_, [&](const auto& permit) {
    return now >= permit.expires ||
           LifeGeneration(permit.source) != permit.source_life ||
           LifeGeneration(permit.target) != permit.target_life;
  });
  if (area) {
    // The native source has one timer/flag per type; a genuinely consumed next
    // item replaces it. C2S708 replay never calls this method a second time.
    std::erase_if(item_effects_, [&](const auto& permit) {
      return permit.source == player && permit.type == type;
    });
    source->second.area_items[type] = {source->second.life_generation, now};
  }
  for (const auto& [target, actor] : actors_) {
    const bool same_team =
        actor.participant.team_code == source->second.participant.team_code;
    const bool eligible = type == 115U                     ? !same_team
                          : type == 126U                   ? target != player
                          : type == 302U                   ? !same_team
                          : (type == 129U || type == 130U) ? same_team
                                                           : target == player;
    const bool target_alive =
        actor.life_state == CombatLifeState::kAlive ||
        (mode == 3U && actor.life_state == CombatLifeState::kBubbled &&
         (ItemEffectInBubble(type) || type == 302U));
    if (!eligible || actor.controller || !target_alive ||
        item_effects_.size() >= 512U) {
      continue;
    }
    item_effects_.push_back({player, source->second.life_generation, target,
                             actor.life_generation, opcode, type, attribute,
                             duration, now + window, now, radius});
  }
}

CombatRelayResult CombatRelay::HandleItemEffect(
    ActorState& actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{packet.body};
  static_cast<void>(reader.ReadU32());
  const auto attribute = packet.opcode == 32828U ? reader.ReadU32() : 0U;
  const auto type = reader.ReadU32();
  const auto duration = packet.opcode == 32828U ? reader.ReadU32() : 0U;
  std::erase_if(item_effects_, [&](const auto& permit) {
    return now >= permit.expires ||
           LifeGeneration(permit.source) != permit.source_life ||
           LifeGeneration(permit.target) != permit.target_life;
  });
  const auto permit =
      std::ranges::find_if(item_effects_, [&](const auto& entry) {
        if (entry.target != actor.participant.player_id ||
            entry.opcode != packet.opcode || entry.type != type ||
            entry.attribute != attribute || entry.duration != duration ||
            now < entry.created)
          return false;
        if (entry.radius == 0.0F)
          return actor.life_state == CombatLifeState::kAlive ||
                 (mode_id_ == 3U &&
                  actor.life_state == CombatLifeState::kBubbled &&
                  (ItemEffectInBubble(type) || type == 302U));
        const auto source = actors_.find(entry.source);
        if (source == actors_.end() ||
            (source->second.life_state != CombatLifeState::kAlive &&
             !(mode_id_ == 3U &&
               source->second.life_state == CombatLifeState::kBubbled &&
               ItemUsableInBubble(type))) ||
            !source->second.position || !actor.position ||
            (actor.life_state != CombatLifeState::kAlive &&
             !((type == 126U || (mode_id_ == 3U && ItemEffectInBubble(type))) &&
               actor.life_state == CombatLifeState::kBubbled)))
          return false;
        // The native client tests 100-unit forward cone / 200-unit magnetic
        // sphere. Server checks a sphere with 50 units of transport tolerance.
        // It does not claim authoritative direction, line-of-sight, movement or
        // collision.
        double squared = 0.0;
        for (std::size_t i = 0U; i < 3U; ++i) {
          const double delta =
              (*source->second.position)[i] - (*actor.position)[i];
          squared += delta * delta;
        }
        const double radius = entry.radius + 50.0;
        return squared <= radius * radius;
      });
  if (actor.controller || permit == item_effects_.end())
    return RejectedResult(CombatRejectReason::kInvalidLifecycleState);
  if (permit->radius != 0.0F) {
    const auto previous = actor.area_reactions.find(type);
    if (previous != actor.area_reactions.end() &&
        previous->second.first == actor.life_generation &&
        now < previous->second.second)
      return RejectedResult(CombatRejectReason::kDuplicateImpact);
  }
  if (!ConsumeActionQuota(actor, now))
    return RejectedResult(CombatRejectReason::kRateLimited);
  if (permit->radius != 0.0F)
    actor.area_reactions[type] = {actor.life_generation,
                                  now + std::chrono::milliseconds{duration}};
  if (type == 112U && mode_id_ == 3U && actor.giant_weapon)
    actor.giant_weapon->expires = now + std::chrono::milliseconds{13250};
  item_effects_.erase(permit);
  return AcceptedResult(actor.participant.player_id, packet);
}

bool CombatRelay::EndAreaItem(ActorState& actor, const protocol::Packet& packet,
                              std::chrono::steady_clock::time_point now) {
  using namespace std::chrono_literals;
  std::uint32_t type = 0U;
  if (packet.opcode == 32831U)
    type = 115U;
  else if (packet.opcode == 32830U) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    type = reader.ReadU32();
  }
  const auto field = actor.area_items.find(type);
  if (field == actor.area_items.end())
    return true;  // Existing harmless native flag clears, including duplicate
                  // ends.
  const auto minimum = type == 115U ? 250ms : 1250ms;
  if (field->second.first == actor.life_generation &&
      (now < field->second.second || now - field->second.second < minimum))
    return false;
  std::erase_if(item_effects_, [&](const auto& permit) {
    return permit.source == actor.participant.player_id && permit.type == type;
  });
  actor.area_items.erase(field);
  return true;
}

}  // namespace bf::runtime
