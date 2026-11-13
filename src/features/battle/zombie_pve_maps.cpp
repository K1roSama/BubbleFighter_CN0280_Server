#include "features/battle/zombie_pve_maps.h"

#include <algorithm>
#include <array>
#include <random>

namespace bf::runtime {
namespace {
// Original ServerShared/MapMeta.xml: all seven are released, and Directory ==
// key.
constexpr std::array kMaps{
    ZombiePveMap{"rel_zombie06_Frontal", "僵尸！正面突破！！"},
    ZombiePveMap{"rel_zombie05_Tear", "骷髅的眼泪僵尸袭击!"},
    ZombiePveMap{"rel_zombie04_Oneway", "沙漠小路僵尸袭击！"},
    ZombiePveMap{"rel_zombie04_1_Oneway", "[进阶]沙漠小路袭击！"},
    ZombiePveMap{"rel_zombie03_Circle", "僵尸公园"},
    ZombiePveMap{"rel_zombie02_Twobridge", "守护积雪桥！"},
    ZombiePveMap{"rel_zombie_01", "泡泡村防御大作战！"},
};

const ZombiePveMap* SelectedMap(
    const protocol::SceneRotatingMap& selection) noexcept {
  // An explicit random choice must not retain a previous round's resolved
  // directory.
  if (selection.family_key == "Random") return nullptr;
  if (const auto* map = FindZombiePveMap(selection.family_key)) return map;
  // Only an absent family permits using the directory as the selection.
  return selection.family_key.empty() ? FindZombiePveMap(selection.map_key)
                                      : nullptr;
}
}  // namespace

std::span<const ZombiePveMap> ZombiePveMaps() noexcept { return kMaps; }

const ZombiePveMap* FindZombiePveMap(std::string_view key) noexcept {
  const auto found = std::ranges::find(kMaps, key, &ZombiePveMap::key);
  return found == kMaps.end() ? nullptr : &*found;
}

const ZombiePveMap& SelectZombiePveMap(
    const protocol::SceneRotatingMap& selection) {
  if (const auto* map = SelectedMap(selection)) return *map;
  static thread_local std::mt19937 engine{std::random_device{}()};
  return kMaps[std::uniform_int_distribution<std::size_t>{
      0U, kMaps.size() - 1U}(engine)];
}

void NormalizeZombiePveConfiguration(
    protocol::SceneConfiguration& configuration) {
  if (configuration.mode_id != 16U) return;
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
}  // namespace bf::runtime
