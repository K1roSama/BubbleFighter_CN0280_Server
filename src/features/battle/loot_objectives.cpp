/** @file loot_objectives.cpp
 * @brief 按确认击爆/羞辱与冻结装备授权有限掉落；金币随终局事务结算。
 */
#include <algorithm>
#include <cmath>

#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
MatchObjectiveUpdate MatchObjectives::Loot(domain::PlayerId player,
                                           const protocol::Packet& packet,
                                           const CombatRelay& combat,
                                           Time now) {
  const auto owner = combat.actors_.find(player);
  if (owner == combat.actors_.end() || owner->second.controller ||
      collectibles_.size() >= 4096U || drops_.size() >= 16384U ||
      loot_budgets_.size() >= 4096U)
    return {};
  const auto& victim = owner->second;
  protocol::PacketReader reader{packet.body};
  std::array<float, 3U> position{}, velocity{};
  for (auto& axis : position) axis = reader.ReadF32();
  for (auto& axis : velocity) axis = reader.ReadF32();
  const auto kind = reader.ReadU32();
  const auto variant = reader.ReadU32();
  const auto seat = reader.ReadU32();
  if (!victim.position ||
      seat != (victim.participant.actor_object_id - 1'000'000U) / 100'000U)
    return {};
  double distance2 = 0;
  for (std::size_t i = 0; i < position.size(); ++i) {
    const double delta = position[i] - (*victim.position)[i];
    distance2 += delta * delta;
  }
  if (distance2 > 100.0 * 100.0) return {};
  const bool heart = packet.opcode == 743U;
  const bool coin = packet.opcode == 737U;
  std::uint64_t event = victim.life_generation;
  if (heart) {
    if ((mode_ != 13U && mode_ != 14U && mode_ != 18U) || kind != 1U ||
        variant != 1U || victim.life_state != CombatLifeState::kDefeated ||
        victim.death_stage != 2U || !victim.humiliated ||
        victim.humiliated->life != victim.life_generation ||
        now < victim.humiliated->completed ||
        now - victim.humiliated->completed > std::chrono::seconds{2})
      return {};
    event = (victim.life_generation << 32U) | victim.humiliated->serial;
  } else {
    if ((coin ? (kind != 1U && kind != 2U) || variant != 5U
              : kind != 1U || variant != 1U) ||
        victim.life_state != CombatLifeState::kDefeated || !victim.defeated ||
        victim.defeated->life != victim.life_generation ||
        now < victim.defeated->time ||
        now - victim.defeated->time > std::chrono::seconds{2})
      return {};
    const auto attacker = combat.object_owners_.find(victim.defeated->attacker);
    if (attacker == combat.object_owners_.end() || attacker->second == player)
      return {};
    const auto& source = combat.actors_.at(attacker->second);
    if (source.controller ||
        (!combat.individual_scoring_ &&
         source.participant.team_code == victim.participant.team_code) ||
        !(coin ? source.participant.lucci_pouch
               : source.participant.chocolate_box))
      return {};
  }
  protocol::PacketWriter signature;
  signature.WriteU64(player.Value());
  signature.WriteU64(event);
  signature.WriteU16(packet.opcode);
  signature.WriteBytes(packet.body);
  auto bytes = signature.Release();
  if (drops_.contains(bytes)) return {};
  const auto key = std::tuple{player, event, packet.opcode};
  auto budget = loot_budgets_.find(key);
  if (budget == loot_budgets_.end()) {
    // ShopItem2: 100256 gives 2..4 coins; 100257 gives 2..5 chocolates at 50%.
    // 830460 has Hurry Up 3/7 config, but 7D19A0 (S2C709) activates its
    // timer only in modes 5/6/7. Hotblood 13/14 stays on normal 1/4 config,
    // even in the final seconds; no client request can enable that timer.
    const auto maximum = heart  ? (mode_ == 13U || mode_ == 14U ? 4U : 7U)
                         : coin ? 4U
                                : 5U;
    budget = loot_budgets_.emplace(key, LootBudget{maximum, 0U}).first;
  }
  if (budget->second.used >= budget->second.maximum) return {};
  ++budget->second.used;
  drops_.insert(std::move(bytes));
  Collectible item;
  item.position = position;
  item.velocity = velocity;
  // The server chooses coin denomination; clients cannot force rare 100 coins.
  item.kind =
      coin && std::bernoulli_distribution{0.005}(supply_random_) ? 2U : 1U;
  item.variant = variant;
  item.value = coin ? (item.kind == 2U ? 100U : 1U) : 0U;
  item.spawn_opcode = coin ? 720U : heart ? 726U : 722U;
  item.collect_opcode = coin ? 736U : heart ? 742U : 738U;
  item.due = now;
  item.active_at = now + std::chrono::seconds{1};
  item.expires = now + std::chrono::seconds{10};
  item.bonus_lucci = coin;
  collectibles_.push_back(item);
  return {};
}

std::uint32_t MatchObjectives::BonusLucci(
    domain::PlayerId player) const noexcept {
  const auto found = bonus_lucci_.find(player);
  return found == bonus_lucci_.end() ? 0U : std::min(found->second, 10'000U);
}
}  // namespace bf::runtime
