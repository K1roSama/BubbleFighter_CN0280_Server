#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_HOTBLOOD_STATS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_HOTBLOOD_STATS_H_

/**
 * @file hotblood_stats.h
 * @brief 定义原生热血属性；装备必须来自已验证库存，不提供完整伤害模型。
 */

#include <cstdint>
#include <span>

namespace bf::domain {
/** @brief 本版 FighterConfig 和 ShopItemSpec 推导的热血生命上限、复活初始时长。
 */
struct HotbloodStats final {
  float maximum_hp;
  std::int32_t
      respawn_ms;  ///< 原生可为负数；等待期限按零钳制，上报仍使用原始 DWORD。
};

/** @brief fighter 为角色类型，equipment 必须来自已验证库存；不是完整伤害模型。
 */
[[nodiscard]] HotbloodStats NativeHotbloodStats(
    std::uint32_t mode, std::uint32_t fighter,
    std::span<const std::uint32_t> equipment);
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_HOTBLOOD_STATS_H_
