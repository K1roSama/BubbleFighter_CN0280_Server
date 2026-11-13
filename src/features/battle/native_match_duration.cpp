#include "features/battle/native_match_duration.h"

#include <array>
#include <string_view>

namespace bf::runtime {
namespace {

struct MapDuration final {
  std::string_view key;
  std::uint32_t eligible_modes;
  std::uint32_t time_attack_ms;
  std::uint32_t conquer_ms;
  std::uint32_t star_ms;
};

#include "features/battle/generated/native_map_durations.inc"

[[nodiscard]] bool EqualKey(std::string_view left,
                            std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  const auto lower = [](char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
  };
  for (std::size_t i = 0; i < left.size(); ++i)
    if (lower(left[i]) != lower(right[i])) return false;
  return true;
}

}  // namespace

std::optional<std::uint32_t> NativeMatchDurationMs(
    std::uint32_t mode, std::string_view metadata_key,
    std::string_view directory) noexcept {
  // B5F8F0 / 500770 explicitly override the infection start rule. The PVE/event
  // defaults below are uniform across all corresponding native MapMeta rows.
  switch (mode) {
    case 8U:
    case 28U:
      return 180'000U;
    case 101U:
    case 102U:
      return 300'000U;
    case 16U:
      return 600'000U;
    case 7U:
    case 17U:
    case 21U:
      return 120'000U;
    case 11U:
      return 241'000U;
    case 1U:
    case 3U:
    case 5U:
    case 6U:
    case 10U:
    case 12U:
    case 13U:
    case 14U:
    case 18U:
    case 20U:
      break;
    default:
      // Bomb rounds, survival phases, racing stages, missions and unverified
      // modes retain their existing server rules, not the common map timer.
      return std::nullopt;
  }
  for (const auto candidate : {metadata_key, directory}) {
    for (const auto& map : kNativeMapDurations) {
      if ((map.eligible_modes & (1U << mode)) == 0U ||
          !EqualKey(map.key, candidate))
        continue;
      if (mode == 5U || mode == 6U) return map.star_ms;
      if (mode == 10U) return map.conquer_ms;
      return map.time_attack_ms;
    }
  }
  // Random/unresolved maps show four minutes in the lobby. Resolve the actual
  // map again at every start, so a prior match cannot freeze this fallback.
  return 240'000U;
}

void ApplyNativeMatchDuration(protocol::SceneConfiguration& configuration,
                              std::optional<std::uint32_t> host_override) {
  const auto native = NativeMatchDurationMs(
      configuration.mode_id, configuration.family_key, configuration.map_key);
  if (!native || configuration.submode_id == 4U) return;
  const auto duration = host_override.value_or(*native);
  configuration.selected_duration_ms = duration;
  configuration.field_24 = 0U;
  configuration.field_28 = 0U;
  configuration.field_32 = 0U;
  if (configuration.mode_id == 5U || configuration.mode_id == 6U)
    configuration.field_32 = duration;
  else if (configuration.mode_id == 10U)
    configuration.field_28 = duration;
  else
    configuration.field_24 = duration;
}

}  // namespace bf::runtime
