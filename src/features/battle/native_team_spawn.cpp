#include "features/battle/native_team_spawn.h"

#include <algorithm>
#include <array>
#include <bit>

namespace bf::runtime {
namespace {
constexpr NativeTeamSpawnMap kMaps[]{
#include "features/battle/generated/native_team_spawn_maps.inc"
};
constexpr NativeTeamSpawnModeMap kModeMaps[]{
#include "features/battle/generated/native_team_spawn_mode_maps.inc"
};

struct MapSpawn final {
  std::string_view map, name;
  std::array<std::uint32_t, 6> bits;
};

constexpr MapSpawn kMapSpawns[]{
#include "features/battle/generated/native_map_spawns.inc"
};

unsigned char AsciiLower(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return static_cast<unsigned char>(
      byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
}

bool SameMap(std::string_view left, std::string_view right) noexcept {
  return std::ranges::equal(left, right, [](char first, char second) {
    return AsciiLower(first) == AsciiLower(second);
  });
}
}  // namespace

std::span<const NativeTeamSpawnMap> NativeTeamSpawnMaps() noexcept {
  return kMaps;
}

const NativeTeamSpawnMap* FindNativeTeamSpawnMap(
    std::string_view key) noexcept {
  const auto found = std::ranges::find_if(kMaps, [&](const auto& entry) {
    return std::ranges::equal(key, entry.key, [](char left, char right) {
      return AsciiLower(left) == AsciiLower(right);
    });
  });
  return found == std::end(kMaps) ? nullptr : found;
}

std::span<const NativeTeamSpawnModeMap> NativeTeamSpawnModeMaps() noexcept {
  return kModeMaps;
}

const NativeTeamSpawnModeMap* FindNativeTeamSpawnModeMap(
    std::uint32_t mode, std::string_view key) noexcept {
  if (mode >= 32U) return nullptr;
  const auto found = std::ranges::find_if(kModeMaps, [&](const auto& entry) {
    return (entry.mode_mask & (1U << mode)) != 0U &&
           std::ranges::equal(key, entry.key, [](char left, char right) {
             return AsciiLower(left) == AsciiLower(right);
           });
  });
  return found == std::end(kModeMaps) ? nullptr : found;
}

std::optional<NativeTeamSpawnRange> NativeTeamRespawnRange(
    std::uint32_t mode, std::string_view key,
    std::span<const NativeTeamSpawnSeat> roster, std::uint8_t seat) noexcept {
  if (!FindNativeTeamSpawnModeMap(mode, key)) return std::nullopt;
  const auto point = NativeTeamSpawnPoint(roster, seat);
  if (!point) return std::nullopt;
  if (mode == 12U && key == "rel_pirate02_frontal") {
    const auto first = (*point / 10U) * 10U;
    return NativeTeamSpawnRange{.first = first, .last = first + 6U};
  }
  return NativeTeamSpawnRange{.first = *point, .last = *point};
}

std::optional<NativeTeamSpawnPose> NativeTeamRespawnPose(
    std::uint32_t mode, std::string_view key, std::uint32_t point) noexcept {
  if (!FindNativeTeamSpawnModeMap(mode, key)) return std::nullopt;
  const bool special = mode == 12U && key == "rel_pirate02_frontal";
  const auto first = special ? 0U : 1U;
  if ((point / 10U != 1U && point / 10U != 2U) || point % 10U < first ||
      point % 10U > 6U)
    return std::nullopt;
  std::array<char, 13> name{'#', 's', 'p', 'a', 'w', 'n', '_',
                            'T', '0', '0', '_', '0', '0'};
  name[8] = static_cast<char>('0' + point / 10U);
  if (special)
    name[12] = static_cast<char>('0' + point % 10U);
  else
    name[9] = static_cast<char>('0' + point % 10U);
  const std::string_view marker{name.data(), special ? name.size() : 10U};
  const auto found = std::ranges::find_if(kMapSpawns, [&](const auto& entry) {
    return SameMap(key, entry.map) && entry.name == marker;
  });
  if (found == std::end(kMapSpawns)) return std::nullopt;
  return NativeTeamSpawnPose{.point = point,
                             .position = {std::bit_cast<float>(found->bits[0]),
                                          std::bit_cast<float>(found->bits[1]),
                                          std::bit_cast<float>(found->bits[2])},
                             .direction = std::bit_cast<float>(found->bits[3])};
}

std::optional<std::uint32_t> NativeTeamSpawnPoint(
    std::span<const NativeTeamSpawnSeat> roster, std::uint8_t seat) noexcept {
  std::array<std::optional<std::uint8_t>, 36U> slots{};
  for (const auto& member : roster) {
    if (member.seat >= slots.size() || slots[member.seat] ||
        (member.team != 1U && member.team != 2U && member.team != 36U))
      return std::nullopt;
    slots[member.seat] = member.team;
  }
  if (seat >= slots.size() || !slots[seat] ||
      (*slots[seat] != 1U && *slots[seat] != 2U))
    return std::nullopt;
  std::uint32_t preceding = 0U;
  for (std::size_t index = 0U; index < seat; ++index)
    if (slots[index] == slots[seat]) ++preceding;
  return (*slots[seat] == 1U ? 11U : 21U) + preceding % 6U;
}
}  // namespace bf::runtime
