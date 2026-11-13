#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_ATTRIBUTE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_ATTRIBUTE_H_

/**
 * @file item_attribute.h
 * @brief 定义经原客户端读取与效果消费验证的库存动态属性。
 */

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf::domain {

/** @brief
 * 6DD740读取的属性记录；source_item_id是物品来源键，不是SQL属性或实例主键。 */
struct NativeItemAttribute final {
  std::uint32_t source_item_id{
      0U};  ///< 原D3F620传给效果计算与物品分类的来源模板键。
  std::uint32_t definition_id{0U};  ///< 原ItemAttr定义编号。
  std::vector<std::int32_t>
      values;                  ///< 与定义的buffIndex数量严格一致的有符号整数。
  std::uint32_t category{0U};  ///< 原属性分类；100号高级化的分类同为100。
  std::string label;           ///< 原UTF16文字的UTF8表示；已恢复100号属性为空。
  bool operator==(const NativeItemAttribute&) const = default;
};

/** @brief 按真实装备模板分组的属性快照；实例归属由应用及仓储层在冻结时验证。 */
struct EquippedItemAttributes final {
  std::uint32_t item_id{0U};  ///< 真实穿戴模板键，不是套装销售键。
  std::vector<NativeItemAttribute>
      attributes;  ///< 该物品保存的属性，保持原vector顺序。
  bool operator==(const EquippedItemAttributes&) const = default;
};

/**
 * @brief
 * 验证当前已恢复的100号属性线格式；不授权升级或替代原isAdvancedable资格判断。
 * @note 每个服装组件最多一条100；固定[2,1,1]来自原RandomTable的相等min/max。
 *       宠物成长7仍使用既有独立字段，其他属性定义未恢复前不能由此接口发送。
 */
[[nodiscard]] inline bool ValidNativeItemAttributes(
    std::uint64_t item_id,
    std::span<const NativeItemAttribute> attributes) noexcept {
  if (attributes.empty()) return true;
  if (item_id < 100'000U || item_id >= 200'000U || attributes.size() != 1U)
    return false;
  const auto& attribute = attributes.front();
  return attribute.source_item_id == item_id &&
         attribute.definition_id == 100U && attribute.category == 100U &&
         attribute.label.empty() && attribute.values.size() == 3U &&
         attribute.values[0] == 2 && attribute.values[1] == 1 &&
         attribute.values[2] == 1;
}

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_ATTRIBUTE_H_
