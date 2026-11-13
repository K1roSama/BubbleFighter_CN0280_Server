#include "features/battle/bomb_maps.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace bf::runtime {
namespace {
constexpr std::array kMaps{
#include "features/battle/generated/bomb_maps.inc"
};

const BombMap* SelectedMap(
    const protocol::SceneRotatingMap& selection) noexcept {
  if (selection.family_key == "Random") return nullptr;
  if (const auto* map = FindBombMap(selection.family_key)) return map;
  return selection.family_key.empty() ? FindBombMap(selection.map_key)
                                      : nullptr;
}
}  // namespace

std::span<const BombMap> BombMaps() noexcept { return kMaps; }

const BombMap* FindBombMap(std::string_view key) noexcept {
  const auto found = std::ranges::find(kMaps, key, &BombMap::key);
  return found == kMaps.end() ? nullptr : &*found;
}

const BombMap& SelectBombMap(const protocol::SceneRotatingMap& selection) {
  if (const auto* map = SelectedMap(selection)) return *map;
  static thread_local std::mt19937 engine{std::random_device{}()};
  return kMaps[std::uniform_int_distribution<std::size_t>{
      0U, kMaps.size() - 1U}(engine)];
}

void NormalizeBombConfiguration(protocol::SceneConfiguration& configuration) {
  if (configuration.mode_id != 19U) return;
  const auto* map = SelectedMap({.family_key = configuration.family_key,
                                 .map_key = configuration.map_key});
  configuration.family_key = map ? map->key : "Random";
  configuration.map_key = map ? map->key : "";
  configuration.display_name = map ? map->display_name : "随机地图";
  configuration.auxiliary_label.clear();
  for (auto& rotating : configuration.rotating_maps) {
    const auto* selected = SelectedMap(rotating);
    rotating.family_key = selected ? selected->key : "Random";
    rotating.map_key = selected ? selected->key : "";
  }
}

bool WithinBombRadius(const std::array<float, 3U>& position,
                      const std::array<float, 3U>& center) noexcept {
  const auto x = position[0] - center[0];
  const auto y = position[1] - center[1];
  const auto z = position[2] - center[2];
  return std::sqrt((y * y + x * x) + z * z) < 15.0F;
}
}  // namespace bf::runtime
