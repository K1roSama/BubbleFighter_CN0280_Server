#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MAPS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MAPS_H_

/**
 * @file zombie_pve_maps.h
 * @brief 定义原版僵尸 Boss 模式的地图白名单与房间选图归一规则。
 */

#include <span>
#include <string_view>

#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {

/** @brief Build 752 模式 16 的地图，按 isZombiePveModeMap（47D820）筛选。 */
struct ZombiePveMap final {
  std::string_view key;
  std::string_view display_name;
};

/** @brief 返回原版发布的全部僵尸 Boss 地图。 */
[[nodiscard]] std::span<const ZombiePveMap> ZombiePveMaps() noexcept;
/** @brief 精确查找地图配置键，不凭 zombie 字样接受其他模式地图。 */
[[nodiscard]] const ZombiePveMap* FindZombiePveMap(
    std::string_view key) noexcept;
/** @brief 解析具体地图；随机选择只在原版 PVE 白名单内进行。 */
[[nodiscard]] const ZombiePveMap& SelectZombiePveMap(
    const protocol::SceneRotatingMap& selection);
/** @brief 等待期保留原生 Random 选项，清除其他模式遗留的无效地图。 */
void NormalizeZombiePveConfiguration(
    protocol::SceneConfiguration& configuration);

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MAPS_H_
