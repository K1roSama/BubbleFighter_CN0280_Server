#include "features/inventory/clothing_attributes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace bf::domain {
namespace {
enum class Attribute : std::uint8_t {
  kMinimum,
  kMaximum,
  kCriticalCancel,
  kBack,
  kReload,
  kMove,
  kDash,
  kRcMinimum,
  kRcMaximum,
  kRcThreshold,
  kRcCount
};

struct Row final {
  std::uint32_t item;
  std::uint32_t channel;
  Attribute attribute;
  float value;
};

constexpr Row kRows[]{
#include "features/inventory/generated/clothing_attributes_data.inc"
};
}  // namespace

ClothingAttributes NativeClothingAttributes(
    std::uint32_t fighter, std::span<const std::uint32_t> equipment,
    std::uint32_t channel) {
  return NativeClothingAttributes(fighter, equipment, channel, {});
}

ClothingAttributes NativeClothingAttributes(
    std::uint32_t fighter, std::span<const std::uint32_t> equipment,
    std::uint32_t channel,
    std::span<const EquippedItemAttributes> dynamic_attributes) {
  std::vector<const EquippedItemAttributes*> dynamic;
  dynamic.reserve(dynamic_attributes.size());
  for (const auto& item : dynamic_attributes) {
    if (item.item_id == 0U ||
        !ValidNativeItemAttributes(item.item_id, item.attributes))
      throw std::invalid_argument{"invalid native dynamic item attributes"};
    dynamic.push_back(&item);
  }
  std::ranges::sort(dynamic, {}, &EquippedItemAttributes::item_id);
  if (std::ranges::adjacent_find(
          dynamic, {}, &EquippedItemAttributes::item_id) != dynamic.end())
    throw std::invalid_argument{"duplicate native dynamic item attributes"};
  ClothingAttributes result;
  float rc_minimum = 0.0F;
  float rc_maximum = 0.0F;
  int rc_threshold = 5;
  int rc_count = 0;
  const auto apply = [&](std::uint32_t item) {
    auto first = std::ranges::lower_bound(kRows, item, {}, &Row::item);
    for (; first != std::end(kRows) && first->item == item; ++first) {
      if (first->channel != 0U && first->channel != channel) continue;
      switch (first->attribute) {
        case Attribute::kMinimum:
          result.defense_min += first->value;
          break;
        case Attribute::kMaximum:
          result.defense_max += first->value;
          break;
        case Attribute::kCriticalCancel:
          result.critical_cancel = first->value;
          break;
        case Attribute::kBack:
          result.back_defense += first->value;
          break;
        case Attribute::kReload:
          result.reload_ratio += first->value;
          break;
        case Attribute::kMove:
          result.move_ratio += first->value;
          break;
        case Attribute::kDash:
          result.dash_ratio += first->value;
          break;
        case Attribute::kRcMinimum:
          rc_minimum += first->value;
          break;
        case Attribute::kRcMaximum:
          rc_maximum += first->value;
          break;
        case Attribute::kRcThreshold:
          rc_threshold = static_cast<int>(first->value);
          break;
        case Attribute::kRcCount:
          rc_count += static_cast<int>(first->value);
          break;
      }
    }
  };
  apply(fighter);
  std::vector<std::uint32_t> ordered{equipment.begin(), equipment.end()};
  std::ranges::sort(ordered);
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
  for (auto item : ordered) {
    if (item != 0U && item != fighter) {
      apply(item);
      const auto found = std::ranges::lower_bound(
          dynamic, item, {}, &EquippedItemAttributes::item_id);
      if (found != dynamic.end() && (*found)->item_id == item) {
        for (const auto& attribute : (*found)->attributes) {
          // 原D3F160逐项int->float乘conv；D22960在每件静态属性后立即调用。
          result.reload_ratio +=
              static_cast<float>(attribute.values[0]) * -0.01F;
          result.defense_min += static_cast<float>(attribute.values[1]) * 0.01F;
          result.defense_max += static_cast<float>(attribute.values[2]) * 0.01F;
        }
      }
    }
  }
  if (rc_count >= rc_threshold) {
    result.defense_min += rc_minimum;
    result.defense_max += rc_maximum;
  }
  return result;
}

std::uint8_t NativeDefensePercent(const ClothingAttributes& attributes,
                                  float random_fraction) {
  if (!std::isfinite(attributes.defense_min) ||
      !std::isfinite(attributes.defense_max) || attributes.defense_min < 0.0F ||
      attributes.defense_min > 1.0F || attributes.defense_max < 0.0F ||
      attributes.defense_max > 1.0F || !std::isfinite(random_fraction) ||
      random_fraction < 0.0F || random_fraction >= 1.0F)
    throw std::invalid_argument{
        "invalid native clothing defense range or random fraction"};
  const auto minimum = static_cast<int>(attributes.defense_min * 100.0F);
  const auto maximum = static_cast<int>(attributes.defense_max * 100.0F);
  return static_cast<std::uint8_t>(
      minimum + static_cast<int>(static_cast<float>(maximum - minimum) *
                                 random_fraction));
}

bool NativeCriticalCancel(const ClothingAttributes& attributes,
                          float random_fraction) {
  if (!std::isfinite(attributes.critical_cancel) ||
      !std::isfinite(random_fraction) || random_fraction < 0.0F ||
      random_fraction > 1.0F)
    throw std::invalid_argument{"invalid native critical cancellation input"};
  return attributes.critical_cancel >= random_fraction;
}
}  // namespace bf::domain
