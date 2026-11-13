/** @file pirate_coin_objectives.cpp
 * @brief 原生2024海盗击败报告，720/736/721金币及一次性结算。
 */
#include <atomic>
#include <cmath>

#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
void MatchObjectives::AdvancePirateCoins(const CombatRelay& combat, Time now,
                                         MatchObjectiveUpdate& result) {
  std::erase_if(pending_pirate_coins_, [&](auto& entry) {
    if (now < entry.received ||
        now - entry.received > std::chrono::seconds{1} ||
        combat.host_ != entry.controller)
      return true;
    auto update = PirateCoin(entry.controller, entry.packet, combat, now);
    if (update.rejection_detail == "pirate coin rejected: no_recent_npc_pang") {
      entry.retry = now + std::chrono::milliseconds{50};
      return false;
    }
    result.messages.insert(result.messages.end(), update.messages.begin(),
                           update.messages.end());
    return true;
  });
}

MatchObjectiveUpdate MatchObjectives::PirateCoin(domain::PlayerId player,
                                                 const protocol::Packet& packet,
                                                 const CombatRelay& combat,
                                                 Time now) {
  const auto reject = [](const char* reason) {
    return MatchObjectiveUpdate{
        .rejection_detail = std::string{"pirate coin rejected: "} + reason};
  };
  if (mode_ != 101U || combat.host_ != player)
    return reject("mode_or_controller");
  protocol::PacketReader reader{packet.body};
  const std::array position{reader.ReadF32(), reader.ReadF32(),
                            reader.ReadF32()};
  const std::array velocity{reader.ReadF32(), reader.ReadF32(),
                            reader.ReadF32()};
  static_cast<void>(
      reader.ReadU32());  // The validated native constant is 5, not an amount.
  const auto source_index = reader.ReadU32();
  for (const auto axis : velocity)
    if (std::abs(axis) > 100.0F) return reject("velocity");

  protocol::PacketWriter signature;
  signature.WriteU16(packet.opcode);
  signature.WriteBytes(packet.body);
  auto bytes = signature.Release();
  if (drops_.contains(bytes)) return reject("duplicate_report");
  // 2024 has no NPC id. Consume the most recent unclaimed pang entitlement.
  // The vectors in 32787/32795 are knockback directions, not world positions
  // (real captures: kick (-0.1,0,1.0) vs coin (2209,262,1646)), so the coin
  // position cannot be checked; issuance stays bounded by confirmed deaths.
  const CombatRelay::ActorState* selected = nullptr;
  for (const auto& [id, actor] : combat.actors_) {
    static_cast<void>(id);
    if (!actor.pirate_coin_source || actor.controller != player ||
        actor.life_state != CombatLifeState::kDefeated || !actor.defeated ||
        actor.defeated->life != actor.life_generation ||
        pirate_coin_claims_.contains(
            {actor.participant.player_id, actor.life_generation}) ||
        now < actor.defeated->time ||
        now - actor.defeated->time > std::chrono::seconds{5} ||
        source_index !=
            (actor.participant.actor_object_id - 1'000'000U) / 100'000U)
      continue;
    const auto attacker = combat.object_owners_.find(actor.defeated->attacker);
    if (attacker == combat.object_owners_.end()) continue;
    const auto& source = combat.actors_.at(attacker->second);
    if (source.controller ||
        source.participant.team_code == actor.participant.team_code ||
        !Participant(attacker->second))
      continue;
    if (!selected || actor.defeated->time > selected->defeated->time)
      selected = &actor;
  }
  if (!selected) return reject("no_recent_npc_pang");
  const auto key =
      std::pair{selected->participant.player_id, selected->life_generation};
  if (pirate_coin_claims_.size() >= 4096U || collectibles_.size() >= 4096U)
    return reject("object_limit");

  // A process-wide namespace distinct from map supplies (60M) and survival
  // loot (70M+) prevents an old 736 from collecting a new room/match's coin.
  static std::atomic<std::uint64_t> next{50'000'000U};
  const auto object = next.fetch_add(1U) + 1U;
  if (object >= 60'000'000U) return reject("object_namespace_exhausted");
  // User-selected local rule, not a claim about unavailable original rates.
  const auto roll =
      std::uniform_int_distribution<unsigned>{0U, 99U}(supply_random_);
  const auto kind = roll < 80U ? 3U : roll < 95U ? 4U : 5U;
  Collectible item;
  item.id = static_cast<std::uint32_t>(object);
  item.kind = kind;
  item.value = kind == 3U ? 1U : kind == 4U ? 5U : 10U;
  item.position = position;
  item.velocity = velocity;
  item.variant = 5U;
  item.spawn_opcode = 720U;
  item.collect_opcode = 736U;
  item.due = now;
  item.active_at = now + std::chrono::seconds{1};
  item.expires = now + std::chrono::seconds{10};
  item.bonus_lucci = true;
  item.pirate_coin = true;
  protocol::PacketWriter body;
  body.WriteU32(item.id);
  body.WriteU32(item.kind);
  for (const auto axis : position) body.WriteF32(axis);
  for (const auto axis : velocity) body.WriteF32(axis);
  body.WriteU32(item.variant);
  collectibles_.push_back(item);
  pirate_coin_claims_.insert(key);
  drops_.insert(std::move(bytes));
  return {.messages = {{.opcode = 720U, .body = body.Release()}}};
}
}  // namespace bf::runtime
