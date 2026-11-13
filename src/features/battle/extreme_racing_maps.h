#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MAPS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MAPS_H_

/**
 * @file extreme_racing_maps.h
 * @brief 提供当前客户端两张极限竞速地图的五赛段终点和检查点区域。
 */

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace bf::runtime {
/** @brief 从原始 Mesh 顶点恢复的世界坐标 AABB，顺序为 min XYZ、max XYZ。 */
struct ExtremeRacingRegion final {
  std::array<float, 6U> bounds;
  /** @brief
   * 检查有限坐标及包含端点的范围；额外容差是本服策略，不是原生碰撞半径。 */
  [[nodiscard]] bool Contains(const std::array<float, 3U>& position,
                              float tolerance) const noexcept;
};

/** @brief 一赛段的 goalin 区域及按编号从零排列的 saveArea 区域。 */
struct ExtremeRacingStage final {
  ExtremeRacingRegion goal;
  std::vector<ExtremeRacingRegion> checkpoints;
};

/** @brief 已发布地图的固定五赛段；不包含尚未完整恢复的金币、炮台及危险物配置。
 */
struct ExtremeRacingMap final {
  std::string_view key;
  std::array<ExtremeRacingStage, 5U> stages;
};

/** @brief 原生 WB 类型 1 的命名对象；位置与旋转均保留原始 float32。 */
struct ExtremeRacingMarker final {
  std::string_view name;
  std::array<float, 3U> position;
  std::array<float, 3U> rotation;
};

/** @brief 两图实际使用的出生点、检查点、金币、炮台及危险物位置目录。 */
struct ExtremeRacingObjectMap final {
  std::string_view key;
  std::vector<ExtremeRacingMarker> markers;
};

/** @brief 返回原始地图区域目录，存储期覆盖整个进程。 */
[[nodiscard]] std::span<const ExtremeRacingMap> ExtremeRacingMaps() noexcept;
/** @brief 严格匹配稳定的小写地图键；未知地图不能回退成其他场景。 */
[[nodiscard]] const ExtremeRacingMap* FindExtremeRacingMap(
    std::string_view key) noexcept;
/** @brief 返回由原始 WB 记录生成的对象位置；不包含猜测的金币分数或赛段。 */
[[nodiscard]] std::span<const ExtremeRacingObjectMap>
ExtremeRacingObjectMaps() noexcept;
/** @brief 按准确地图键和原生名称查找；缺失时不回退其他对象。 */
[[nodiscard]] const ExtremeRacingMarker* FindExtremeRacingMarker(
    std::string_view map, std::string_view name) noexcept;
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MAPS_H_
