#include <limits>
#include <numeric>

#include "features/battle/zombie_pve_match.h"

namespace bf::runtime {

void ZombiePveMatch::ObserveScoreUnits(const CombatRelay& combat) {
  for (auto& agent : agents_) {
    if (!agent.active || agent.team != 1U || !agent.object || !agent.score_unit)
      continue;
    const auto owner = combat.object_owners_.find(*agent.object);
    if (owner == combat.object_owners_.end()) continue;
    const auto victim = combat.actors_.find(owner->second);
    if (victim == combat.actors_.end() ||
        victim->second.life_state != CombatLifeState::kDefeated ||
        !victim->second.defeated)
      continue;
    const auto& fact = *victim->second.defeated;
    if (fact.life != victim->second.life_generation ||
        agent.classified_life == fact.life)
      continue;
    agent.classified_life = fact.life;
    const auto attacker = combat.object_owners_.find(fact.attacker);
    if (attacker == combat.object_owners_.end()) continue;
    auto player = attacker->second;
    if (const auto turret = combat.turrets_.find(fact.attacker);
        turret != combat.turrets_.end())
      player = turret->second.owner;
    const auto source = combat.actors_.find(player);
    if (source == combat.actors_.end() || source->second.controller ||
        source->second.participant.team_code != 2U)
      continue;
    auto& counts = confirmed_units_[player];
    const auto total =
        std::accumulate(counts.begin(), counts.end(), std::uint64_t{0U});
    // 类别是击败的子集，不为没有实际记入击爆的生命周期动作创建分数。
    if (total >= source->second.performance.pangs ||
        counts[*agent.score_unit] == std::numeric_limits<std::uint32_t>::max())
      continue;
    ++counts[*agent.score_unit];
  }
}

std::array<std::uint32_t, 16U> ZombiePveMatch::ScoreUnits(
    domain::PlayerId player) const noexcept {
  const auto found = confirmed_units_.find(player);
  return found == confirmed_units_.end() ? std::array<std::uint32_t, 16U>{}
                                         : found->second;
}

}  // namespace bf::runtime
