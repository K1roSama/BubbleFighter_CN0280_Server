#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_SET_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_SET_H_

/**
 * @file clothing_set.h
 * @brief 定义原SetItem仓库套装与有序服装组件，区分关联商城销售记录。
 */

#include <cstdint>
#include <span>

namespace bf::domain {

/** @brief 原catalog+276的SetItem记录；不包含商城礼包附送道具。 */
struct NativeClothingSet final {
  std::uint32_t set_id{0U};  ///< 原setId；高级化136提交这个编号。
  std::uint32_t shop_stock_set_id{
      0U};  ///< 关联销售记录，不能替代setId或决定组件。
  std::uint32_t declared_component_count{
      0U};  ///< 原setCnt；残缺原记录不能授权升级。
  std::span<const std::uint32_t>
      component_ids;  ///< 原ShopItemID01..setCnt有序模板。
  bool catalog_advancedable{
      false};  ///< 每件原isAdvancedable精确为1；不代表库存资格。
};

/** @brief 返回当前原资源的仓库套装表；引用在进程生命周期内有效。 */
[[nodiscard]] std::span<const NativeClothingSet> NativeClothingSets() noexcept;

/** @brief 只按原setId精确查找；不接受销售别名、单件或SQL实例号。 */
[[nodiscard]] const NativeClothingSet* FindNativeClothingSet(
    std::uint32_t set_id) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_CLOTHING_SET_H_
