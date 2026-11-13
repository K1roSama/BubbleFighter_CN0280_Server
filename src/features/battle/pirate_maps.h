#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAPS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAPS_H_

/**
 * @file pirate_maps.h
 * @brief 提供原版海盗模式地图白名单及按难度划分的人气随机地图。
 */

#include <cstdint>
#include <span>
#include <string_view>

#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {

/** @brief 当前客户端 MapMeta 与人气难度表共同确认的海盗地图元数据。 */
struct PirateMap final {
  std::string_view key;           ///< MapMeta 配置键，包含海盗难度。
  std::string_view directory;     ///< MapMeta.Directory 指定的实际资源目录。
  std::string_view display_name;  ///< 原版中文地图名。
  std::uint32_t difficulty;       ///< 原版地图难度，范围一至三。
  bool popular;                   ///< 是否列在人气随机难度表中。
};

/** @brief 返回 Build 752 的海盗地图目录，不以名字前缀推断合法性。 */
[[nodiscard]] std::span<const PirateMap> PirateMaps(
    std::uint32_t mode = 101U) noexcept;

/** @brief 保留目录内明确地图，否则从指定难度的人气地图中随机选择。 */
[[nodiscard]] const PirateMap& SelectPirateMap(std::string_view requested,
                                               std::uint32_t difficulty = 1U,
                                               std::uint32_t mode = 101U);

/** @brief 优先保留有效 MapMeta 配置，否则按资源目录和难度解析地图。 */
[[nodiscard]] const PirateMap& SelectPirateMap(
    const protocol::SceneRotatingMap& selection, std::uint32_t difficulty = 1U,
    std::uint32_t mode = 101U);

/** @brief 同步海盗配置的 MapMeta 键、资源目录和显示名。 */
void NormalizePirateConfiguration(protocol::SceneConfiguration& configuration);

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAPS_H_
