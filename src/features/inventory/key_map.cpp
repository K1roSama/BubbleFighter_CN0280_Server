#include "features/inventory/key_map.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace bf::application {
bool ValidKeyMap(const KeyMapSettings& settings) noexcept {
  // Config/KeyMapConfig.csv 的 Changeable=TRUE；排除固定 F5 与箭头别名。
  static constexpr std::array<std::string_view, kMaximumKeyBindings> kActions{
      "mvFwd",
      "mvRight",
      "mvLeft",
      "mvBack",
      "jumpOrBomb",
      "1stWeapon",
      "2ndWeapon",
      "3rdWeapon",
      "4thWeapon",
      "5thWeapon",
      "changeWeapon",
      "tabBtn",
      "reLoad",
      "humiliate",
      "throwingMainWeapon",
      "backPack",
      "useItemOrRespawn",
      "enter",
      "changeItem",
      "incMouseStv",
      "decMousestv",
      "miniMapZI",
      "miniMapZO",
      "screenShot",
      "recordVideo",
      "recordHDVideo",
      "cancel",
      "leaveParty",
      "PopOrPower",
      "zbItem1",
      "zbItem2",
      "zbItem3",
      "volumeUp",
      "volumeDown",
      "fullScr",
      "gtTutorialBtn",
      "installStopBomb",
      "voiceTalkBlock",
      "chattingBlock",
      "smartPingWarning",
      "smartPingMove"};
  if (settings.bindings.size() > kActions.size() ||
      (settings.reset && !settings.bindings.empty())) {
    return false;
  }
  for (std::size_t index = 0U; index < settings.bindings.size(); ++index) {
    const auto& binding = settings.bindings[index];
    if (binding.key > 255U ||
        std::ranges::find(kActions, binding.action) == kActions.end()) {
      return false;
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (settings.bindings[previous].action == binding.action) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace bf::application
