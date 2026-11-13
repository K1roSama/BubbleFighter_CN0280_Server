#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_MATCH_DURATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_MATCH_DURATION_H_

/**
 * @file native_match_duration.h
 * @brief 原版地图与模式的可信默认时长，以及房间时间同步。
 */

#include <cstdint>
#include <optional>
#include <string_view>

#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {

/** @brief 返回已核实的原版默认时间；专用或未知计时返回空值。 */
[[nodiscard]] std::optional<std::uint32_t> NativeMatchDurationMs(
    std::uint32_t mode, std::string_view metadata_key,
    std::string_view directory) noexcept;

/** @brief 同步房间时间，区分地图默认值与房主主动设置的覆盖值。 */
void ApplyNativeMatchDuration(
    protocol::SceneConfiguration& configuration,
    std::optional<std::uint32_t> host_override = std::nullopt);

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_MATCH_DURATION_H_
