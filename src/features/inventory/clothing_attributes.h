#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_ATTRIBUTES_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_ATTRIBUTES_H_

/**
 * @file clothing_attributes.h
 * @brief 当前原版 ShopItemSpec 的服饰防御、RC 套装、移动及换弹参数。
 */

#include <cstdint>
#include <span>

#include "features/inventory/item_attribute.h"

namespace bf::domain {
/** @brief 按原版单精度累加的参数；百分比区间尚未转换为整数。 */
struct ClothingAttributes final {
  float defense_min{0.0F};
  float defense_max{0.0F};
  float critical_cancel{0.0F};
  float back_defense{0.0F};
  float reload_ratio{0.0F};
  float move_ratio{0.0F};
  float dash_ratio{0.0F};
  bool operator==(const ClothingAttributes&) const = default;
};

/**
 * @brief 与 D22960 一致：先角色、再按 ID
 * 升序去重的装备，逐行执行频道过滤和加法。
 * @note 输入必须为已验证的有效装备；本函数不执行所有权、服装槽及期限授权。
 *       表中不存在的属性不会由商店图表或 modelFrom 推断。动态 ItemAttr
 * 不在本静态表范围。
 */
[[nodiscard]] ClothingAttributes NativeClothingAttributes(
    std::uint32_t fighter, std::span<const std::uint32_t> equipment,
    std::uint32_t channel);

/**
 * @brief
 * D22960逐物品执行静态后动态属性；未穿戴库存不生效，重复穿戴键只计算一次。
 * @note 只消费已恢复100号固定属性；畸形或重复物品属性快照抛出invalid_argument。
 *       调用方仍须从本人有效实例冻结属性；本函数不授予升级，也不从销售套装推导属性。
 */
[[nodiscard]] ClothingAttributes NativeClothingAttributes(
    std::uint32_t fighter, std::span<const std::uint32_t> equipment,
    std::uint32_t channel,
    std::span<const EquippedItemAttributes> dynamic_attributes);

/** @brief 821AC0 / E08FA0：上下界乘 100 后截断，随机值为原版 float [0,1)。 */
[[nodiscard]] std::uint8_t NativeDefensePercent(
    const ClothingAttributes& attributes, float random_fraction);
/** @brief 821C70 的大于等于比较；random_fraction 允许原版舍入得到的 1。 */
[[nodiscard]] bool NativeCriticalCancel(const ClothingAttributes& attributes,
                                        float random_fraction);
}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_ATTRIBUTES_H_
