#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_BOMB_MAPS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_BOMB_MAPS_H_

/**
 * @file bomb_maps.h
 * @brief 定义原生爆破地图、固定安装点和严格三维操作距离。
 */

#include <array>
#include <span>
#include <string_view>

#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {
/** @brief MapMeta 发布的爆破地图及 WB bombInstallMarker 原始坐标。 */
struct BombMap final {
  std::string_view key;
  std::string_view display_name;
  std::array<std::array<float, 3U>, 2U> sites;
};

/** @brief 当前客户端发布的六张爆破地图。 */
[[nodiscard]] std::span<const BombMap> BombMaps() noexcept;
/** @brief 查找发布地图，返回稳定目录键；不会接受仅有标记的未开放地图。 */
[[nodiscard]] const BombMap* FindBombMap(std::string_view key) noexcept;
/** @brief 随机选择只在原版已发布爆破地图中进行。 */
[[nodiscard]] const BombMap& SelectBombMap(
    const protocol::SceneRotatingMap& selection);
/** @brief 保留合法地图和 Random 语义，清除其他模式遗留的地图字段。 */
void NormalizeBombConfiguration(protocol::SceneConfiguration& configuration);
/** @brief EB3860/53F430：按原生 float 运算顺序计算三维距离，严格小于 15。 */
[[nodiscard]] bool WithinBombRadius(
    const std::array<float, 3U>& position,
    const std::array<float, 3U>& center) noexcept;
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_BOMB_MAPS_H_
