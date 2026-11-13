#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_TEAM_SPAWN_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_TEAM_SPAWN_H_

/**
 * @file native_team_spawn.h
 * @brief 按原席位、模式与地图规则计算普通团队复活点授权。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf::runtime {
/** @brief 一名已登记席位；队伍1/2为参赛者，36为观察者。 */
struct NativeTeamSpawnSeat final {
  std::uint8_t seat{0U};   ///< 原始席位索引，限0..35。
  std::uint8_t team{36U};  ///< 冻结队伍；观察者不推进任一队伍的出生序号。
};

/** @brief 原MapMeta已发布且不带任务/教程标记的普通道具团队地图。 */
struct NativeTeamSpawnMap final {
  std::string_view key;  ///< 原地图目录键。
  std::int32_t index;    ///< 原MapIdx，供资源核验。
};

/** @brief 取得原资源确认的普通道具团队地图目录。 */
[[nodiscard]] std::span<const NativeTeamSpawnMap>
NativeTeamSpawnMaps() noexcept;

/** @brief 按目录键查找适用地图；未知、任务、教程及未发布地图返回空。 */
[[nodiscard]] const NativeTeamSpawnMap* FindNativeTeamSpawnMap(
    std::string_view key) noexcept;

/** @brief 原普通团队地图及其已核对模式的位掩码。 */
struct NativeTeamSpawnModeMap final {
  std::string_view key;     ///< 原地图目录键。
  std::int32_t index;       ///< 原MapIdx，保留有符号哨兵。
  std::uint32_t mode_mask;  ///< 第mode位表示该原地图允许此模式。
};

/** @brief 一名玩家本局普通复活可使用的闭区间；相等端点表示固定点。 */
struct NativeTeamSpawnRange final {
  std::uint32_t first;  ///< 允许的最小点号。
  std::uint32_t last;   ///< 允许的最大点号。
};

/** @brief 原WB出生点的位置及A086D0实际消费的朝向。 */
struct NativeTeamSpawnPose final {
  std::uint32_t point;            ///< 原704／707／32926点号。
  std::array<float, 3> position;  ///< 当前原WB的精确float32坐标。
  float direction;                ///< 原dummy姿态的第一个float。
};

/**
 * @brief 按模式、实际目录及点号读取原WB姿态；特殊狙击使用T10_00／T20_00等名称。
 * @note 只返回确实存在的原记录；未知地图、点号或缺失标记不猜测原回退容器顺序。
 */
[[nodiscard]] std::optional<NativeTeamSpawnPose> NativeTeamRespawnPose(
    std::uint32_t mode, std::string_view key, std::uint32_t point) noexcept;

/** @brief 取得按原模式筛选标志生成的普通团队地图。 */
[[nodiscard]] std::span<const NativeTeamSpawnModeMap>
NativeTeamSpawnModeMaps() noexcept;

/** @brief 同时验证模式及地图；未知、未发布或无原资格时返回空。 */
[[nodiscard]] const NativeTeamSpawnModeMap* FindNativeTeamSpawnModeMap(
    std::uint32_t mode, std::string_view key) noexcept;

/**
 * @brief 根据冻结名单及实际地图确定固定点，或原狙击特例的同队随机范围。
 * @return
 * 普通点号为相等端点；模式12精确rel_pirate02_frontal为红10..16/蓝20..26。
 */
[[nodiscard]] std::optional<NativeTeamSpawnRange> NativeTeamRespawnRange(
    std::uint32_t mode, std::string_view key,
    std::span<const NativeTeamSpawnSeat> roster, std::uint8_t seat) noexcept;

/**
 * @brief 按席位递增次序独立计数两队，每队按六个出生点循环。
 * @param roster 冻结名单；允许稀疏席位和观察者，不允许重复席位或未知队伍。
 * @param seat 请求者的冻结席位。
 * @return 红队11..16或蓝队21..26；无效名单、缺席或观察者返回空。
 */
[[nodiscard]] std::optional<std::uint32_t> NativeTeamSpawnPoint(
    std::span<const NativeTeamSpawnSeat> roster, std::uint8_t seat) noexcept;
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_TEAM_SPAWN_H_
