#include "features/battle/extreme_racing_maps.h"

#include <algorithm>
#include <cmath>

namespace bf::runtime {
namespace {
const std::array kMaps{
#include "features/battle/generated/extreme_racing_maps.inc"
};
const std::array kObjects{
#include "features/battle/generated/extreme_racing_objects.inc"
};
}  // namespace

bool ExtremeRacingRegion::Contains(const std::array<float, 3U>& position,
                                   float tolerance) const noexcept {
  if (!std::isfinite(tolerance) || tolerance < 0.0F) return false;
  for (std::size_t axis = 0U; axis < position.size(); ++axis)
    if (!std::isfinite(position[axis]) ||
        position[axis] < bounds[axis] - tolerance ||
        position[axis] > bounds[axis + 3U] + tolerance)
      return false;
  return true;
}

std::span<const ExtremeRacingMap> ExtremeRacingMaps() noexcept { return kMaps; }

const ExtremeRacingMap* FindExtremeRacingMap(std::string_view key) noexcept {
  const auto found = std::ranges::find(kMaps, key, &ExtremeRacingMap::key);
  return found == kMaps.end() ? nullptr : &*found;
}

std::span<const ExtremeRacingObjectMap> ExtremeRacingObjectMaps() noexcept {
  return kObjects;
}

const ExtremeRacingMarker* FindExtremeRacingMarker(
    std::string_view map, std::string_view name) noexcept {
  const auto scene =
      std::ranges::find(kObjects, map, &ExtremeRacingObjectMap::key);
  if (scene == kObjects.end()) return nullptr;
  const auto marker =
      std::ranges::find(scene->markers, name, &ExtremeRacingMarker::name);
  return marker == scene->markers.end() ? nullptr : &*marker;
}
}  // namespace bf::runtime
