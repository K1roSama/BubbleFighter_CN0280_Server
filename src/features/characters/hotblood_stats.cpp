#include "features/characters/hotblood_stats.h"

#include <algorithm>
#include <vector>

namespace bf::domain {
namespace {
struct Fighter {
  std::uint32_t id;
  float hp;
  std::int32_t respawn;
};

struct Equipment {
  std::uint32_t id;
  std::int32_t respawn_decrease;
};

#include "features/characters/generated/hotblood_stats_data.inc"
}  // namespace

HotbloodStats NativeHotbloodStats(std::uint32_t mode, std::uint32_t fighter,
                                  std::span<const std::uint32_t> equipment) {
  // 865950 adds common + fighter values; all channel HP additions are zero.
  HotbloodStats result{100.0F, 7000};
  const auto character = std::ranges::find(kFighters, fighter, &Fighter::id);
  if (character != std::end(kFighters)) {
    result.maximum_hp = character->hp;
    result.respawn_ms = character->respawn;
  }
  result.maximum_hp *= mode == 13U ? 1.5F : 1.4F;  // 81BCE0 -> 5DD5A0
  std::vector<std::uint32_t> unique{equipment.begin(), equipment.end()};
  unique.push_back(300000U + fighter);
  std::ranges::sort(unique);
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  for (const auto id : unique) {
    // D22960 deduplicates item IDs; D34DF0 case23 sums integer reductions.
    for (const auto& item : kRespawnEquipment)
      if (item.id == id) result.respawn_ms -= item.respawn_decrease;
  }
  return result;
}
}  // namespace bf::domain
