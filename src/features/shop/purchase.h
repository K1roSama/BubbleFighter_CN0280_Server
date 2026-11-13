#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_H_

/**
 * @file purchase.h
 * @brief 定义商城规格到库存变化的纯领域规则，不依赖协议或数据库驱动。
 */

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "application/identifier.h"

namespace bf::domain {

/** @brief 三种不可互换的整数资产代码。 */
enum class CurrencyCode : std::uint8_t {
  kLucci,
  kBubbleCash,
  kTreasureBubble,
};

/** @brief 当前库存按数量合并或保持单实例的策略。 */
enum class ItemStackPolicy : std::uint8_t {
  kMergeQuantity,
  kSingleInstance,
};

/** @brief 模板对永久和限时销售规格的允许方式。 */
enum class ItemDurationPolicy : std::uint8_t {
  kPermanent,
  kExpiring,
  kOfferDefined,
};

/** @brief 从已发布目录和模板在同一事务中读取的购买事实。 */
struct PurchaseOffer final {
  StockId stock_id;                             ///< 客户端选择的销售规格 ID。
  ItemId item_id;                               ///< 实际发放的模板 ID。
  CurrencyCode currency{CurrencyCode::kLucci};  ///< 条件扣款币种。
  std::int64_t unit_price{0};      ///< 成交整数价格，兼容上限为 INT32_MAX。
  std::int64_t grant_quantity{0};  ///< 数量堆叠增量；计时/永久单实例为 1。
  std::optional<std::int64_t>
      duration_seconds;  ///< 限时增量；永久或数量堆叠为空。
  ItemStackPolicy stack_policy{
      ItemStackPolicy::kSingleInstance};  ///< 库存合并策略。
  ItemDurationPolicy duration_policy{
      ItemDurationPolicy::kPermanent};  ///< 期限策略。
  std::int64_t max_quantity{1};         ///< 当前库存最大数量。
  std::optional<std::int64_t>
      max_duration_seconds;  ///< 从当前时刻计的最大累计期限。
};

/** @brief 事务内锁定的同模板当前库存状态。 */
struct OwnedItemSnapshot final {
  ItemInstanceId item_instance_id;  ///< 当前合并库存实例。
  ItemId item_id;                   ///< 必须与购买规格模板相同。
  std::int64_t quantity{0};         ///< 普通数量为正；原版Life允许零。
  std::optional<std::int64_t>
      expires_at_unix_seconds;  ///< 永久为空，否则为 UTC Unix 秒。
};

/** @brief 购买规格或当前库存违反领域约束时的稳定分类。 */
enum class PurchaseRuleError : std::uint8_t {
  kInvalidOffer,
  kInvalidInventory,
  kAlreadyOwnedPermanent,
  kQuantityLimitExceeded,
  kDurationLimitExceeded,
};

/** @brief 领域计算出的库存目标状态和审计变化。 */
struct PurchaseInventoryEffect final {
  std::int64_t quantity_after{0};  ///< 提交后的正数量。
  std::int64_t quantity_delta{0};  ///< 流水数量变化；续期或永久升级可为零。
  std::optional<std::int64_t> expires_before;  ///< 变更前期限。
  std::optional<std::int64_t> expires_after;   ///< 变更后期限。
};

/**
 * @brief 在事务已锁定目录与库存后计算一次购买的库存效果。
 * @param offer 当前已发布且启用的成交规格与模板限制。
 * @param owned 当前同模板库存；不存在时为空。
 * @param now_unix_seconds 数据库工作线程取得的当前 UTC Unix 秒。
 * @note
 * 本函数不扣款、不生成订单；调用方必须在同一事务重新验证事实并持久化完整结果。
 */
[[nodiscard]] std::variant<PurchaseInventoryEffect, PurchaseRuleError>
CalculatePurchaseInventoryEffect(const PurchaseOffer& offer,
                                 const std::optional<OwnedItemSnapshot>& owned,
                                 std::int64_t now_unix_seconds) noexcept;

/** @brief 返回日志、指标和测试使用的稳定英文购买规则错误名。 */
[[nodiscard]] std::string_view PurchaseRuleErrorName(
    PurchaseRuleError error) noexcept;

/** @brief 返回数据库与协议适配层使用的稳定资产代码。 */
[[nodiscard]] std::string_view CurrencyCodeName(CurrencyCode currency) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_H_
