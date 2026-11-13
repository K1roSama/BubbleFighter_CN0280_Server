#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_MYSTERY_SHOP_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_MYSTERY_SHOP_SERVICE_H_

/**
 * @file mystery_shop_service.h
 * @brief 定义神秘商店固定选品、刷新券消耗和单代去重购买用例。
 */

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"
#include "application/system_clock.h"
#include "features/configuration/service_rules.h"

namespace bf::application {

/** @brief 一代神秘商店的不可变六项销售快照及购买位图。 */
struct MysterySelectionSnapshot final {
  domain::PlayerId player_id;    ///< 选品所属玩家。
  std::uint64_t generation{0U};  ///< 单调递增代次，用于拒绝旧 UI 请求。
  std::chrono::system_clock::time_point
      expires_at;                        ///< 本代服务端 UTC 失效时刻。
  std::vector<MysteryOfferRule> offers;  ///< 固定顺序的六项权威销售规格。
  std::uint32_t purchased_mask{0U};      ///< 第 i 位表示 offers[i] 已成交。
  std::uint64_t rules_version_id{0U};    ///< 生成本代的 service_rules 版本。
};

/** @brief 神秘商店购买提交或业务去重重放后的稳定结果。 */
struct MysteryPurchaseResult final {
  MysterySelectionSnapshot selection;  ///< 成交后同一代选品和最新购买位图。
  domain::ItemId reward_item_id;       ///< 客户端成功响应附带的实际物品模板。
  bool inventory_changed{false};       ///< 本次调用是否真正扣款并修改库存。
};

/**
 * @brief 按旧服务已验证公式，从候选池选择不同物品的固定顺序。
 * @note owned_permanent_items 只排除永久拥有物；限时拥有物仍允许续期。
 */
[[nodiscard]] ServiceResult<std::vector<MysteryOfferRule>> SelectMysteryOffers(
    domain::PlayerId player_id, std::uint64_t generation,
    std::span<const domain::ItemId> owned_permanent_items,
    const MysteryShopServiceRules& rules);

/** @brief 神秘商店持久化端口；每个方法必须在一个短事务内串行化同一玩家。 */
class MysteryShopRepository {
 public:
  virtual ~MysteryShopRepository() = default;

  /** @brief 恢复未过期选品；不存在或到期时按规则生成下一代并持久化。 */
  [[nodiscard]] virtual ServiceResult<MysterySelectionSnapshot> LoadOrCreate(
      domain::PlayerId player_id, std::uint64_t rules_version_id,
      const MysteryShopServiceRules& rules,
      std::chrono::system_clock::time_point now) = 0;

  /** @brief 复核代次，原子扣一张刷新券、写流水并生成下一代。 */
  [[nodiscard]] virtual ServiceResult<MysterySelectionSnapshot> Refresh(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation, const MysteryShopServiceRules& rules,
      std::uint64_t rules_version_id,
      std::chrono::system_clock::time_point now) = 0;

  /** @brief 按冻结价格和期限原子扣款发货；同代同 stock 重发不得重复扣款。 */
  [[nodiscard]] virtual ServiceResult<MysteryPurchaseResult> Purchase(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation, domain::StockId stock_id,
      std::uint64_t rules_version_id, const MysteryShopServiceRules& rules,
      std::chrono::system_clock::time_point now) = 0;
};

/** @brief 网关 opcode 129 路由依赖的神秘商店业务接口。 */
class MysteryShopExecutor {
 public:
  virtual ~MysteryShopExecutor() = default;

  /** @brief 查询或建立当前一代选品。 */
  [[nodiscard]] virtual ServiceResult<MysterySelectionSnapshot> State(
      domain::PlayerId player_id) const = 0;

  /** @brief 只接受客户端看到的当前代次，并消耗规则指定的一张刷新券。 */
  [[nodiscard]] virtual ServiceResult<MysterySelectionSnapshot> Refresh(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation) const = 0;

  /** @brief 购买当前六项之一；价格、期限和奖励全部来自冻结快照。 */
  [[nodiscard]] virtual ServiceResult<MysteryPurchaseResult> Purchase(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation, domain::StockId stock_id) const = 0;
};

/** @brief 使用已发布 service_rules 并校验仓储快照的神秘商店应用服务。 */
class MysteryShopService final : public MysteryShopExecutor {
 public:
  /** @brief 注入 UTC 时钟、规则加载器和事务仓储；依赖生命周期必须覆盖本服务。
   */
  MysteryShopService(const SystemClock& clock, ServiceRulesLoader& rules,
                     MysteryShopRepository& repository) noexcept;

  [[nodiscard]] ServiceResult<MysterySelectionSnapshot> State(
      domain::PlayerId player_id) const override;
  [[nodiscard]] ServiceResult<MysterySelectionSnapshot> Refresh(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation) const override;
  [[nodiscard]] ServiceResult<MysteryPurchaseResult> Purchase(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint64_t expected_generation,
      domain::StockId stock_id) const override;

 private:
  const SystemClock& clock_;
  ServiceRulesLoader& rules_;
  MysteryShopRepository& repository_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_MYSTERY_SHOP_SERVICE_H_
