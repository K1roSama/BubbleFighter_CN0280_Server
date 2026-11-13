#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_EVENT_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_EVENT_H_

/**
 * @file megaphone_event.h
 * @brief 保存原版 ShopItem2 与 B02070/B02600 共同验证的喇叭用途和颜色。
 */
#include <array>
#include <cstdint>
#include <optional>

namespace bf::domain {
/** @brief 原生喇叭模板及由 parameter 列派生的广播类型和主色。 */
struct MegaphoneDefinition final {
  std::uint32_t item_id;
  std::uint32_t type;
  std::uint32_t color;
};

inline constexpr std::array<MegaphoneDefinition, 8> kMegaphones{{
    {100251U, 103U, static_cast<std::uint32_t>(-4925199)},
    {100252U, 101U, static_cast<std::uint32_t>(-3754)},
    {100547U, 101U, static_cast<std::uint32_t>(-8531190)},
    {100548U, 101U, static_cast<std::uint32_t>(-52033)},
    {102173U, 101U, static_cast<std::uint32_t>(-8375)},
    {190071U, 101U, static_cast<std::uint32_t>(-11870611)},
    {190074U, 101U, static_cast<std::uint32_t>(-46075)},
    {190105U, 101U, static_cast<std::uint32_t>(-9312513)},
}};

/** @brief 未经原版验证的模板不能作为付费广播道具。 */
[[nodiscard]] constexpr std::optional<MegaphoneDefinition>
GetMegaphoneDefinition(std::uint32_t id) {
  for (const auto& item : kMegaphones) {
    if (item.item_id == id) return item;
  }
  return std::nullopt;
}

/** @brief 自动编号按原生发送器区分全服和频道；显式编号必须与请求类型一致。 */
[[nodiscard]] constexpr bool ValidMegaphoneSelection(std::uint32_t type,
                                                     std::uint32_t selection) {
  if (selection == 10000U) return type == 101U;
  if (selection == 20000U) return type == 102U || type == 103U;
  const auto item = GetMegaphoneDefinition(selection);
  return item && item->type == type;
}
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_EVENT_H_
