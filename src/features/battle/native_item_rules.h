#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_ITEM_RULES_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_ITEM_RULES_H_

/** @file native_item_rules.h
 * @brief 普通道具模式经原客户端核实的泡封使用限制。
 */

#include <cstdint>

namespace bf::runtime {
// 8B23D0, state 300..399; intersected with ItemSpecItem's positive draw
// weights.
constexpr bool ItemUsableInBubble(std::uint32_t type) noexcept {
  return type == 101U || type == 108U || type == 109U || type == 115U ||
         type == 122U || type == 126U || type == 127U || type == 129U ||
         type == 130U;
}

// 8B2850 excludes medicine from bubbled targets, while shield/self attributes
// and freeze/magnet retain their native bubble branches.
constexpr bool ItemEffectInBubble(std::uint32_t type) noexcept {
  return ItemUsableInBubble(type) && type != 129U;
}
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_NATIVE_ITEM_RULES_H_
