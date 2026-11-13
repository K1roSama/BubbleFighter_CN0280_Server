#include "features/battle/survival_loot.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

#include "features/battle/native_survival_tables.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
// E08DD0/E08ED0/E08FA0: subtractive PRNG with single precision sampling.
// Seeds are bounded server policy; the explicit 758 stream never asks the
// client to reproduce this generator or its CSV cluster traversal order.
class NativeRandom final {
 public:
  explicit NativeRandom(std::uint32_t seed) {
    std::int32_t previous =
        161803398 - static_cast<std::int32_t>(seed % 161803399U);
    values_[55] = previous;
    std::int32_t next = 1;
    for (std::size_t i = 1; i < 55; ++i) {
      values_[21 * i % 55] = next;
      const auto saved = next;
      next = Subtract(previous, next);
      previous = saved;
    }
    for (unsigned pass = 0; pass < 4; ++pass)
      for (std::size_t i = 1; i <= 55; ++i)
        values_[i] = Subtract(values_[i], values_[1 + (i + 30) % 55]);
  }

  std::uint32_t Next(std::uint32_t maximum) {
    float sample;
    do {
      first_ = first_ % 55U + 1U;
      second_ = second_ % 55U + 1U;
      values_[first_] = Subtract(values_[first_], values_[second_]);
      sample = static_cast<float>(static_cast<double>(values_[first_]) *
                                  4.656612875245797e-10);
    } while (sample == 1.0F);
    return static_cast<std::uint32_t>(static_cast<float>(maximum) * sample);
  }

  std::uint32_t Range(std::uint32_t minimum, std::uint32_t maximum) {
    return minimum + Next(maximum - minimum + 1U);
  }

 private:
  static std::int32_t Subtract(std::int32_t first, std::int32_t second) {
    const auto value = first - second;
    return value < 0 ? value + 2147483647 : value;
  }

  std::array<std::int32_t, 56U> values_{};
  std::size_t first_{0U}, second_{21U};
};
}  // namespace

std::vector<SurvivalLoot> GenerateSurvivalLoot(std::string_view map,
                                               std::uint32_t seed) {
  using namespace survival_data;
  const auto found = std::ranges::find(kMaps, map, &Map::key);
  if (found == std::end(kMaps)) return {};
  NativeRandom random{seed};
  std::vector<SurvivalLoot> result;
  for (const auto& spot :
       std::span{kSpots}.subspan(found->start, found->count)) {
    const auto& group = kGroups[spot.group];
    const auto draw = random.Next(10000U);
    std::uint32_t cumulative = 0U;
    for (const auto& row : std::span{kRows}.subspan(group.start, group.count)) {
      cumulative += row.weight;
      if (draw >= cumulative) continue;
      const auto& main =
          kCandidates[row.main_start + random.Next(row.main_count)];
      result.push_back({.type = main.type,
                        .quantity = random.Range(main.minimum, main.maximum),
                        .position = spot.position});
      const auto bonus_count = random.Range(row.minimum, row.maximum);
      for (std::uint32_t i = 0; i < bonus_count; ++i) {
        const auto& bonus =
            kCandidates[row.bonus_start + random.Next(row.bonus_count)];
        const auto velocity = kVelocities[random.Next(12U)];
        result.push_back(
            {.type = bonus.type,
             .quantity = random.Range(bonus.minimum, bonus.maximum),
             .position = spot.position,
             .velocity = velocity});
      }
      break;
    }
  }
  return result;
}

std::uint32_t SurvivalLootWeapon(std::uint32_t type) noexcept {
  const auto found = std::ranges::find(survival_data::kSpecs, type,
                                       &survival_data::Spec::type);
  return found == std::end(survival_data::kSpecs) ? 0U : found->weapon;
}

void AllocateSurvivalLootIds(std::span<SurvivalLoot> loot) {
  // A single namespace includes initial and dropped objects in every room.
  static std::atomic<std::uint64_t> next{70'000'000U};
  auto object = next.fetch_add(loot.size());
  if (object + loot.size() > 0x7FFFFFFFU)
    throw std::invalid_argument{"survival object namespace exhausted"};
  for (auto& item : loot) item.id = static_cast<std::uint32_t>(++object);
}

std::vector<SurvivalLoot> DecodeSurvivalDrop(
    std::span<const std::uint8_t> body) {
  protocol::PacketReader reader{body};
  // 7ED270 is a vector of 36-byte records. The recovered UI sends one;
  // sixteen is a transport bound, not a claim about a native batch limit.
  const auto count = reader.ReadCount(16U);
  if (count == 0U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "empty survival drop vector"};
  std::vector<SurvivalLoot> result;
  for (std::uint32_t i = 0U; i < count; ++i) {
    SurvivalLoot item;
    item.type = reader.ReadU32();
    item.quantity = reader.ReadU32();
    if (item.quantity == 0U || item.quantity > 999U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid survival drop quantity"};
    const auto vector = [&reader](auto& value, float bound) {
      for (auto& axis : value) {
        axis = reader.ReadF32();
        if (!std::isfinite(axis) || std::abs(axis) > bound)
          throw protocol::ProtocolError{
              protocol::ProtocolErrorCode::kInvalidValue,
              "invalid survival drop vector"};
      }
    };
    vector(item.position, 10'000'000.0F);
    vector(item.velocity, 100.0F);
    if (reader.ReadU32() != 1U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid survival drop expiry flag"};
    result.push_back(item);
  }
  reader.RequireEnd();
  return result;
}

std::vector<OutboundMessage> EncodeSurvivalLoot(
    std::span<const SurvivalLoot> loot) {
  std::vector<OutboundMessage> result;
  while (!loot.empty()) {
    const auto chunk = loot.first(std::min<std::size_t>(loot.size(), 128U));
    protocol::PacketWriter writer;
    writer.WriteU32(static_cast<std::uint32_t>(chunk.size()));
    for (const auto& item : chunk) {
      if (item.id == 0U || item.quantity == 0U || item.quantity > 999U)
        throw std::invalid_argument{"invalid native survival spawn"};
      writer.WriteU32(item.id);
      writer.WriteU32(item.type);
      writer.WriteU32(item.quantity);
      for (const auto axis : item.position) writer.WriteF32(axis);
      for (const auto axis : item.velocity) writer.WriteF32(axis);
      writer.WriteU32(item.expires ? 1U : 0U);
    }
    result.push_back({.opcode = 758U, .body = writer.Release()});
    loot = loot.subspan(chunk.size());
  }
  return result;
}
}  // namespace bf::runtime
