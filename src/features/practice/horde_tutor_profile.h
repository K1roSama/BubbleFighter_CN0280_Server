#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_PROFILE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_PROFILE_H_

/**
 * @file horde_tutor_profile.h
 * @brief 原生103唯一启用波次的不可变生成授权，不接受房主扩展脚本计划。
 */

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace bf::runtime {

inline constexpr std::uint32_t kHordeTutorMode = 103U;
inline constexpr std::string_view kHordeTutorMap = "rel_prologue_01";
inline constexpr std::uint32_t kHordeTutorWaveLimitMs = 180'000U;

/** @brief 原WaveTest的组和ConsoleMacro展开字段；索引按Lua的1基编号。 */
struct HordeTutorSpawn final {
  std::uint8_t group;
  std::uint32_t start_ms;
  std::u16string_view macro;
  std::u16string_view mob_type;
  std::uint8_t replay_group;
  std::u16string_view spawn_point;
  bool friendly;
};

/** @brief 仅活跃第一波的19组；注释波次不是额外游戏内容。 */
[[nodiscard]] std::span<const HordeTutorSpawn> HordeTutorSpawns() noexcept;
/** @brief 已核实WP第一waypoint，非容易混淆的spatial header；组必须1..19。 */
[[nodiscard]] const std::array<float, 3U>& HordeTutorSpawnPosition(
    std::uint8_t group);

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_PROFILE_H_
