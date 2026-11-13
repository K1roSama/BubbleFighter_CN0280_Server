#include "features/battle/pirate_maps.h"

#include <algorithm>
#include <array>
#include <random>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bf::runtime {
namespace {
// Build 752 ServerShared/MapMeta.xml and HotRandomDifficultyMapList.csv.
constexpr std::array kMaps{
    PirateMap{"npc_Forest02_Ravine01", "rel_Forest02_Ravine", "森林遗忘山涧",
              1U, false},
    PirateMap{"npc_Forest02_Ravine02", "rel_Forest02_Ravine", "森林遗忘山涧",
              2U, false},
    PirateMap{"npc_Forest02_Ravine03", "rel_Forest02_Ravine", "森林遗忘山涧",
              3U, false},
    PirateMap{"npc_Forest01_Hill01", "rel_Forest01_Hill", "森林小乐园", 1U,
              false},
    PirateMap{"npc_Forest01_Hill02", "rel_Forest01_Hill", "森林小乐园", 2U,
              false},
    PirateMap{"npc_Forest01_Hill03", "rel_Forest01_Hill", "森林小乐园", 3U,
              false},
    PirateMap{"npc_Stadium01_Pangpang01", "rel_Stadium01_Pangpang",
              "砰砰竞技场", 1U, false},
    PirateMap{"npc_Stadium01_Pangpang02", "rel_Stadium01_Pangpang",
              "砰砰竞技场", 2U, false},
    PirateMap{"npc_Stadium01_Pangpang03", "rel_Stadium01_Pangpang",
              "砰砰竞技场", 3U, false},
    PirateMap{"npc_Desert04_Pharaoh01", "rel_Desert04_Pharaoh",
              "沙漠法老的秘密墓地", 1U, true},
    PirateMap{"npc_Desert04_Pharaoh02", "rel_Desert04_Pharaoh",
              "沙漠法老的秘密墓地", 2U, true},
    PirateMap{"npc_Desert04_Pharaoh03", "rel_Desert04_Pharaoh",
              "沙漠法老的秘密墓地", 3U, true},
    PirateMap{"npc_Village07_Easy01", "rel_Village07_Easy02",
              "泡泡村小河边休闲区", 1U, true},
    PirateMap{"npc_Village07_Easy02", "rel_Village07_Easy02",
              "泡泡村小河边休闲区", 2U, true},
    PirateMap{"npc_Village07_Easy03", "rel_Village07_Easy02",
              "泡泡村小河边休闲区", 3U, true},
    PirateMap{"npc_Forest03_Waterfall01", "rel_Forest03_Waterfall",
              "森林清澈瀑布", 1U, true},
    PirateMap{"npc_Forest03_Waterfall02", "rel_Forest03_Waterfall",
              "森林清澈瀑布", 2U, true},
    PirateMap{"npc_Forest03_Waterfall03", "rel_Forest03_Waterfall",
              "森林清澈瀑布", 3U, true},
    PirateMap{"npc_pirate03_prison01", "rel_pirate03_prison", "营救公主", 1U,
              true},
    PirateMap{"npc_pirate03_prison02", "rel_pirate03_prison", "营救公主", 2U,
              true},
    PirateMap{"npc_pirate03_prison03", "rel_pirate03_prison", "营救公主", 3U,
              true},
    PirateMap{"npc_village08_daohouse01", "rel_village08_daohouse", "皮蛋小屋",
              1U, false},
    PirateMap{"npc_village08_daohouse02", "rel_village08_daohouse", "皮蛋小屋",
              2U, false},
    PirateMap{"npc_village08_daohouse03", "rel_village08_daohouse", "皮蛋小屋",
              3U, false},
    PirateMap{"npc_Desert05_mummy01", "rel_Desert05_mummy", "沙漠木乃伊的诅咒",
              1U, true},
    PirateMap{"npc_Desert05_mummy02", "rel_Desert05_mummy", "沙漠木乃伊的诅咒",
              2U, true},
    PirateMap{"npc_Desert05_mummy03", "rel_Desert05_mummy", "沙漠木乃伊的诅咒",
              3U, true},
    PirateMap{"npc_Village05_Playground01", "rel_Village05_Playground",
              "小区BBD游乐场", 1U, true},
    PirateMap{"npc_Village05_Playground02", "rel_Village05_Playground",
              "小区BBD游乐场", 2U, true},
    PirateMap{"npc_Village05_Playground03", "rel_Village05_Playground",
              "小区BBD游乐场", 3U, true},
    PirateMap{"npc_Space01_Spaceship01", "rel_Space01_Spaceship",
              "阿瑞斯号宇宙飞船", 1U, false},
    PirateMap{"npc_Space01_Spaceship02", "rel_Space01_Spaceship",
              "阿瑞斯号宇宙飞船", 2U, false},
    PirateMap{"npc_Space01_Spaceship03", "rel_Space01_Spaceship",
              "阿瑞斯号宇宙飞船", 3U, false},
};
// MapMeta 的 HordeZHE 专用配置；模式 102 仍使用 HordeModeMgr（7FD310）。
// 不把这些条目混入 101 的人气随机图，也不能把 ZombiePve 地图用于该模式。
constexpr std::array kZombieHordeMaps{
    PirateMap{"npc_Zombie_Desert04_Pharaoh01", "rel_Desert04_Pharaoh",
              "沙漠法老的秘密墓地", 1U, true},
    PirateMap{"npc_Zombie_Forest01_Hill01", "rel_Forest01_Hill", "森林小乐园",
              1U, true},
    PirateMap{"npc_Zombie_Stadium01_Pangpang01", "rel_Stadium01_Pangpang",
              "砰砰竞技场", 1U, true},
    PirateMap{"npc_Zombie_Village07_Easy01", "rel_Village07_Easy02",
              "泡泡村小河边休闲区", 1U, true},
};
}  // namespace

std::span<const PirateMap> PirateMaps(std::uint32_t mode) noexcept {
  if (mode == 101U) {
    return kMaps;
  }
  return mode == 102U ? std::span<const PirateMap>{kZombieHordeMaps}
                      : std::span<const PirateMap>{};
}

const PirateMap& SelectPirateMap(std::string_view requested,
                                 std::uint32_t difficulty, std::uint32_t mode) {
  const auto maps = PirateMaps(mode);
  if (maps.empty()) {
    throw std::invalid_argument{"unsupported horde map mode"};
  }
  const auto selected = std::ranges::find(maps, requested, &PirateMap::key);
  if (selected != maps.end()) {
    return *selected;
  }
  difficulty = mode == 102U ? 1U : std::clamp(difficulty, 1U, 3U);
  const auto resource =
      std::ranges::find_if(maps, [requested, difficulty](const auto& map) {
        return map.directory == requested && map.difficulty == difficulty;
      });
  if (resource != maps.end()) {
    return *resource;
  }
  std::vector<const PirateMap*> candidates;
  for (const auto& map : maps) {
    if (map.popular && map.difficulty == difficulty) {
      candidates.push_back(&map);
    }
  }
  static thread_local std::mt19937 engine{std::random_device{}()};
  const auto index = std::uniform_int_distribution<std::size_t>{
      0U, candidates.size() - 1U}(engine);
  return *candidates[index];
}

const PirateMap& SelectPirateMap(const protocol::SceneRotatingMap& selection,
                                 std::uint32_t difficulty, std::uint32_t mode) {
  const auto maps = PirateMaps(mode);
  const auto selected =
      std::ranges::find(maps, selection.family_key, &PirateMap::key);
  return selected != maps.end()
             ? *selected
             : SelectPirateMap(selection.map_key, difficulty, mode);
}

void NormalizePirateConfiguration(protocol::SceneConfiguration& configuration) {
  if (configuration.mode_id != 101U && configuration.mode_id != 102U) {
    return;
  }
  const auto& selected =
      SelectPirateMap({.family_key = configuration.family_key,
                       .map_key = configuration.map_key},
                      1U, configuration.mode_id);
  configuration.map_key = selected.directory;
  configuration.display_name = selected.display_name;
  configuration.family_key = selected.key;
  for (auto& map : configuration.rotating_maps) {
    if (!map.map_key.empty() || !map.family_key.empty()) {
      const auto& rotating =
          SelectPirateMap(map, selected.difficulty, configuration.mode_id);
      map.family_key = rotating.key;
      map.map_key = rotating.directory;
    }
  }
}
}  // namespace bf::runtime
