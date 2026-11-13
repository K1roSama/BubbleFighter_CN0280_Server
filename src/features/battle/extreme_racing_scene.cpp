#include "features/battle/extreme_racing_scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>

namespace bf::runtime {
namespace {
struct CoinPlan final {
  std::string_view marker;
  std::uint32_t stage;
  std::uint32_t type;
  std::int32_t points;
};

struct CannonPlan final {
  std::string_view marker;
  std::string_view target;
  std::uint32_t group;
  std::uint32_t stage;
  std::uint32_t interval;
  std::vector<ExtremeRacingCannonWeapon> weapons;
};

struct ScenePlan final {
  std::string_view map;
  float cannon_height;
  std::vector<CoinPlan> coins;
  std::vector<CannonPlan> cannons;
  std::vector<std::string_view> disposers;
  std::vector<std::uint32_t> route_counts;
};

const std::array kPlans{
#include "features/battle/generated/extreme_racing_scene.inc"
};

std::array<float, 3U> Position(std::string_view map, std::string_view name) {
  const auto* marker = FindExtremeRacingMarker(map, name);
  if (!marker)
    throw std::invalid_argument{
        "extreme racing scene references absent native marker"};
  return marker->position;
}

bool InRange(std::uint32_t object, std::uint32_t base, std::size_t count) {
  return object >= base && static_cast<std::uint64_t>(object) - base < count;
}
}  // namespace

ExtremeRacingScene ExtremeRacingScene::Load(std::string_view map,
                                            std::uint32_t seed,
                                            ExtremeRacingObjectBases bases) {
  const auto plan = std::ranges::find(kPlans, map, &ScenePlan::map);
  if (plan == kPlans.end())
    throw std::invalid_argument{"unknown native extreme racing scene"};
  const std::array<std::pair<std::uint32_t, std::size_t>, 3U> ranges{
      {{bases.cannons, plan->cannons.size() * 2U},
       {bases.coins, plan->coins.size()},
       {bases.disposers, plan->disposers.size()}}};
  for (std::size_t index = 0U; index < ranges.size(); ++index) {
    const auto [base, count] = ranges[index];
    if (base == 0U ||
        base > static_cast<std::uint32_t>(
                   std::numeric_limits<std::int32_t>::max()) ||
        count > 1024U ||
        (count && static_cast<std::uint64_t>(base) + count - 1U >
                      static_cast<std::uint32_t>(
                          std::numeric_limits<std::int32_t>::max())))
      throw std::invalid_argument{"invalid extreme racing object range"};
    if (count)
      for (std::size_t prior = 0U; prior < index; ++prior) {
        const auto [other, size] = ranges[prior];
        if (size && (InRange(base, other, size) || InRange(other, base, count)))
          throw std::invalid_argument{
              "overlapping extreme racing object ranges"};
      }
  }
  ExtremeRacingScene result;
  result.map_ = plan->map;  // Own the static canonical key, not a caller's
                            // temporary string.
  result.initialization_ = {
      .cannon_base = bases.cannons,
      .cannon_count = static_cast<std::uint32_t>(plan->cannons.size()),
      .coin_base = bases.coins,
      .grenade_base = bases.disposers,
      .grenade_count = static_cast<std::uint32_t>(plan->disposers.size())};
  std::uint32_t object = bases.coins;
  for (const auto& coin : plan->coins) {
    result.initialization_.coins.push_back({coin.type, coin.stage});
    result.coins_.push_back(
        {object++, coin.stage, Position(map, coin.marker), coin.points});
  }
  object = bases.cannons;
  for (const auto& cannon : plan->cannons) {
    auto source = Position(map, cannon.marker);
    auto target = Position(map, cannon.target);
    source[1] += plan->cannon_height;  // 51DF20 adds CannonHeight to both ends
                                       // before construction.
    target[1] += plan->cannon_height;
    std::array<float, 3U> direction{};
    float length_squared = 0.0F;
    for (std::size_t axis = 0U; axis < direction.size(); ++axis) {
      direction[axis] = target[axis] - source[axis];
      length_squared += direction[axis] * direction[axis];
    }
    if (!std::isfinite(length_squared) || length_squared <= 0.0F)
      throw std::invalid_argument{"invalid native cannon direction"};
    const auto length = std::sqrt(length_squared);
    for (auto& component : direction) component /= length;
    result.cannons_.push_back(
        {object, object + 1U, cannon.group, cannon.stage, source, direction,
         std::chrono::milliseconds{cannon.interval}, cannon.weapons});
    object += 2U;
  }
  for (const auto marker : plan->disposers)
    result.disposers_.push_back(Position(map, marker));
  // Local selection policy. Native RNG is separately seeded by the complete
  // supplied DWORD.
  std::mt19937 random{seed};
  for (auto& pattern : result.hazards_.floor_patterns)
    pattern = static_cast<std::uint32_t>(random() % 10U);
  result.hazards_.grenade_seed = seed;
  for (const auto count : plan->route_counts) {
    if (count == 0U)
      throw std::invalid_argument{"native vanishing group has no routes"};
    result.hazards_.vanishing_routes.push_back(
        static_cast<std::uint32_t>(random() % count));
  }
  // Exercise all transport bounds before a partially initialized scene can
  // leave this factory.
  static_cast<void>(
      protocol::EncodeExtremeRacingInitialization(result.initialization_));
  static_cast<void>(protocol::EncodeExtremeRacingHazards(result.hazards_));
  return result;
}

bool ExtremeRacingScene::OwnsObject(std::uint32_t object) const noexcept {
  return InRange(object, initialization_.cannon_base, cannons_.size() * 2U) ||
         InRange(object, initialization_.coin_base, coins_.size()) ||
         InRange(object, initialization_.grenade_base, disposers_.size());
}
}  // namespace bf::runtime
