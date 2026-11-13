#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MODE_WEAPON_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MODE_WEAPON_H_

/**
 * @file mode_weapon.h
 * @brief 当前原生武器表中僵尸PVE与海盗专用模板的确定性选择。
 */

#include <cstdint>

namespace bf::domain {

/** @brief 按834DB0/DE7020及当前WeaponSpec表选择战斗模板，保留未替换的配装。
 * @note
 * 必须在角色专用变体之后调用；102/103不走101分支，不能因同用HordeModeMgr而替换。
 *       当前四个目标模板的hordeModeId均为空，功能516的二次查找保持首次结果。
 */
[[nodiscard]] constexpr std::uint32_t ModeWeaponTemplate(
    std::uint32_t mode, std::uint32_t weapon) noexcept {
  if (mode != 16U && mode != 101U) return weapon;
  switch (weapon) {
    case 2207U:
      return 2208U;
    case 2347U:
      return 2349U;
    case 3103U:
      return 3221U;
    case 3344U:
      return 3345U;
    default:
      return weapon;
  }
}

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MODE_WEAPON_H_
