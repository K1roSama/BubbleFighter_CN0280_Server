#include <algorithm>

#include "features/battle/combat_relay.h"

namespace bf::runtime {

CombatRelayResult CombatRelay::HandleStorm(
    domain::PlayerId sender, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  using namespace std::chrono_literals;
  protocol::NativeStormRequest request;
  if (packet.opcode == 33282U) {
    const auto impact =
        protocol::DecodeNativeWeaponObject(packet.opcode, packet.body);
    request.actor_object_id = impact.target_object_id;
    request.cookie = impact.cookie;
  } else {
    request = protocol::DecodeNativeStorm(packet.opcode, packet.body);
  }
  auto& member = actors_.at(sender);
  const auto reject = [](CombatRejectReason reason) {
    return RejectedResult(reason);
  };
  PruneProjectiles(now);
  if (packet.opcode == 32899U || packet.opcode == 32910U) {
    const auto owner = object_owners_.find(request.actor_object_id);
    if (owner == object_owners_.end())
      return reject(CombatRejectReason::kUnknownTarget);
    auto& source = actors_.at(owner->second);
    // Both native sources are client-controlled AI. This authorizes only an
    // already bound script NPC/Boss, not arbitrary players or arbitrary items.
    if (host_ != sender || source.controller != sender ||
        !source.storm_source || source.life_state != CombatLifeState::kAlive)
      return reject(CombatRejectReason::kInvalidLifecycleState);
    const bool pirate = source.storm_source == 1U;
    const bool type_ok =
        pirate ? packet.opcode == 32899U
               : packet.opcode == 32910U &&
                     (request.type == 211U || request.type == 217U);
    const auto weapon = pirate ? 9011U : request.type == 211U ? 9027U : 9026U;
    if (!type_ok || request.weapon != weapon ||
        request.team != source.participant.team_code ||
        request.target != 0xffffffffU || request.cookie == 0xffffffffU)
      return reject(CombatRejectReason::kUnrecoveredAction);
    // 7C6ED0/7C6DD0 allocate increasing GoDamager cookies. Retain the
    // high-water mark on the native actor range, shared by NPCs, even after
    // source removal.
    const auto range = (request.actor_object_id - 1'000'000U) / 100'000U;
    const auto previous = storm_serials_.find(range);
    if ((previous != storm_serials_.end() &&
         request.cookie <= previous->second) ||
        ContainsDamagerCookie(request.cookie))
      return reject(CombatRejectReason::kDuplicateProjectile);
    // The AI chooses its own attack phase. A one-second floor is server abuse
    // policy, below recovered Kephi/Boss timers; it is not a fabricated item
    // use.
    if (source.last_storm_time &&
        (now < *source.last_storm_time || now - *source.last_storm_time < 1s))
      return reject(CombatRejectReason::kRateLimited);
    if (storms_.size() + weapon_objects_.size() >= 8192U)
      return reject(CombatRejectReason::kProjectileLimit);
    if (!ConsumeActionQuota(member, now))
      return reject(CombatRejectReason::kRateLimited);
    StormState storm{.source = request.actor_object_id,
                     .type = request.type,
                     .created = now,
                     .flight = request.type == 211U ? 1500ms : 2000ms};
    for (const auto& [id, actor] : actors_) {
      if (!actor.controller)
        storm.victims.emplace(
            id,
            StormVictim{.life = actor.life_generation,
                        .enemy = actor.participant.team_code != request.team});
    }
    storms_.emplace(request.cookie, std::move(storm));
    storm_serials_[range] = request.cookie;
    source.last_storm_time = now;
    return AcceptedResult(sender, packet);
  }
  if (member.controller ||
      request.actor_object_id != member.participant.actor_object_id)
    return reject(CombatRejectReason::kUnknownTarget);
  const auto eligible = [&](const StormState& storm, bool cleanup) {
    const auto victim = storm.victims.find(sender);
    return now >= storm.created && now - storm.created <= storm.flight + 3s &&
           victim != storm.victims.end() && !victim->second.ended &&
           (cleanup || (victim->second.enemy &&
                        victim->second.life == member.life_generation &&
                        member.life_state == CombatLifeState::kAlive));
  };
  if (packet.opcode == 32911U) {
    // The native packet has an item type, not a cookie. Attribute the report to
    // the oldest eligible cast; never turn it into server HP or reward changes.
    // The packet cannot distinguish simultaneous casts of the same type. A
    // short repeat window prevents an identical retry consuming a second cast.
    if (member.storm_reaction && member.storm_reaction->first == request.type &&
        (now < member.storm_reaction->second ||
         now - member.storm_reaction->second < 250ms))
      return reject(CombatRejectReason::kDuplicateImpact);
    auto found = storms_.end();
    for (auto it = storms_.begin(); it != storms_.end(); ++it) {
      if (it->second.type == request.type && eligible(it->second, false) &&
          !it->second.victims.at(sender).reaction &&
          (found == storms_.end() ||
           it->second.created < found->second.created))
        found = it;
    }
    if (found == storms_.end())
      return reject(CombatRejectReason::kDuplicateImpact);
    if (!ConsumeActionQuota(member, now))
      return reject(CombatRejectReason::kRateLimited);
    found->second.victims.at(sender).reaction = true;
    member.storm_reaction = std::pair{request.type, now};
    return AcceptedResult(sender, packet);
  }
  const auto found = storms_.find(request.cookie);
  if (found == storms_.end())
    return reject(CombatRejectReason::kUnknownProjectile);
  auto& storm = found->second;
  const bool cleanup = packet.opcode == 32916U;
  if (!eligible(storm, cleanup))
    return reject(CombatRejectReason::kInvalidLifecycleState);
  // A 250 ms transport allowance is server policy. The actual flight durations
  // are ItemSpec's 1500/2000 ms, recovered through 9239F0 -> 924A10.
  if (cleanup && now - storm.created < storm.flight - 250ms)
    return reject(CombatRejectReason::kInvalidLifecycleState);
  auto& victim = storm.victims.at(sender);
  if (!cleanup && victim.impact)
    return reject(CombatRejectReason::kDuplicateImpact);
  if (!ConsumeActionQuota(member, now))
    return reject(CombatRejectReason::kRateLimited);
  if (cleanup) {
    victim.ended = true;
    // 7D62D0 checks the second DWORD against the receiving local Character.
    // Clearing one observer's proxy must not end other observers' reports.
    return {.accepted = true,
            .deliveries = {
                {.recipient = sender,
                 .message = {.opcode = packet.opcode, .body = packet.body}}}};
  }
  victim.impact = true;
  return AcceptedResult(sender, packet);
}

}  // namespace bf::runtime
