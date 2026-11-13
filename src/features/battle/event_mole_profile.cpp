#include "features/battle/event_mole_profile.h"

namespace bf::runtime {
namespace {
#include "features/battle/generated/event_mole_profile_data.inc"

constexpr EventMoleProfile kProfile{
    kEventMoleMap,
    "두더지 이벤트맵",
    "95d03de0ebb18ff41fdff3a3ac80b0e29f1583a7eaee9cd5d98e56ac73d3f2de",
    "6d51a7e7ee0449d901af67a4f65d46f9f41b298357ecb8a5df933330409b933a",
    "c546ca6511dc82705143ed5211e7f70e7f03930ecd8dc615c9d982151ae4f47a",
    2U,
    8U,
    120'000U,
    true,
    107U,
    17U,
    kActionPoints,
    kProperties,
    kSpawners,
    kLifecycleOptions,
    kGameOptions};
}  // namespace

const EventMoleProfile& GetEventMoleProfile() noexcept { return kProfile; }

bool IsEventMoleMap(std::string_view key) noexcept {
  if (key.size() != kEventMoleMap.size()) return false;
  const auto lower = [](char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
  };
  for (std::size_t i = 0U; i < key.size(); ++i)
    if (lower(key[i]) != lower(kEventMoleMap[i])) return false;
  return true;
}

void NormalizeEventMoleConfiguration(
    protocol::SceneConfiguration& configuration) {
  if (configuration.mode_id == kEventMoleMode) {
    // This restores the original MapMeta preview key, not gameplay. No flag or
    // score target is fabricated from the raw individual-map declaration.
    configuration.family_key = kEventMoleMap;
    configuration.map_key = kEventMoleMap;
    configuration.display_name = kProfile.display_name;
    configuration.auxiliary_label.clear();
    for (auto& map : configuration.rotating_maps) {
      map.family_key = kEventMoleMap;
      map.map_key = kEventMoleMap;
    }
    return;
  }
  if (IsEventMoleMap(configuration.family_key) ||
      IsEventMoleMap(configuration.map_key)) {
    configuration.family_key = "Random";
    configuration.map_key.clear();
    configuration.display_name = "随机地图";
    configuration.auxiliary_label.clear();
  }
  for (auto& map : configuration.rotating_maps)
    if (IsEventMoleMap(map.family_key) || IsEventMoleMap(map.map_key))
      map = {.family_key = "Random", .map_key = {}};
}

std::string_view EventMoleStartBlocker() noexcept {
  return "EventMole 尚未恢复原生对象 cookie/计数器同步登记及原服终局规则，已保护拒绝开局；"
         "地图和报文资料仅为只读组件，不是可用玩法";
}
}  // namespace bf::runtime
