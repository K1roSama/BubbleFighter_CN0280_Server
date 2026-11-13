#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ITEM_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ITEM_H_

/** @file life_item.h
 * @brief Build 752 ShopItem2 endurance 道具集合及原始 Life 上限。
 */
#include <algorithm>
#include <array>
#include <cstdint>

namespace bf::domain {
/** @brief 当前原版资源中 durationType=endurance 的完整模板集合。 */
inline constexpr std::array<std::uint32_t, 80> kLifeItemIds{
    101447, 101448, 101449, 101450, 101451, 101452, 101453, 101454, 101455,
    101700, 101701, 101702, 101703, 101704, 101786, 101856, 101862, 101920,
    101922, 101956, 101987, 101999, 102029, 102068, 102069, 102070, 102073,
    102089, 102115, 102147, 102265, 102314, 102315, 102316, 102317, 102318,
    102369, 190149, 190150, 190155, 190157, 190175, 190202, 190244, 201337,
    201350, 201359, 201362, 201923, 201924, 202339, 202386, 202394, 202400,
    202924, 202938, 202939, 202956, 203381, 203390, 203395, 203925, 203926,
    203934, 203935, 203949, 204131, 204375, 204382, 204388, 204924, 204933,
    204934, 204944, 207925, 207928, 207941, 207946, 207952, 207975};
inline constexpr std::uint32_t kLifeMaximum = 15U;
inline constexpr std::uint32_t kLifeSyringe = 190144U;

/** @brief 不把其他数量堆叠道具或可修改目录字段误当成 Life。 */
[[nodiscard]] constexpr bool IsLifeItem(std::uint64_t item) noexcept {
  return std::ranges::binary_search(kLifeItemIds, item);
}
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ITEM_H_
