#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_SERVICE_H_

/**
 * @file purchase_service.h
 * @brief 定义商品购买的幂等、扣款、发货和审计单事务用例。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"
#include "application/system_clock.h"
#include "features/shop/purchase.h"

namespace bf::application {

/** @brief 协议适配层已分配内部稳定操作标识的一次购买命令。 */
struct PurchaseCommand final {
  domain::OperationId
      operation_id;  ///< 内部重试复用的 UUID；不是未经证实的客户端请求号。
  domain::PlayerId player_id;  ///< 发起购买并持有钱包/库存的玩家。
  domain::StockId stock_id;    ///< 客户端选择的销售规格，不能用 item_id 代替。
};

/** @brief 钱包条件扣款成功后返回的事务内余额快照。 */
struct WalletDebit final {
  domain::CurrencyCode currency{
      domain::CurrencyCode::kLucci};  ///< 实际扣款币种。
  std::int64_t balance_before{0};     ///< 扣款前非负余额。
  std::int64_t balance_after{0};      ///< 扣款后非负余额。
};

/** @brief 已提交购买或幂等重放返回给协议层的稳定结果。 */
struct PurchaseResult final {
  domain::OrderId order_id;                 ///< 已结算订单主键。
  domain::PlayerId player_id;               ///< 订单所属玩家。
  domain::StockId stock_id;                 ///< 实际成交销售规格。
  domain::ItemId item_id;                   ///< 发放物品模板。
  domain::ItemInstanceId item_instance_id;  ///< 当前库存实例。
  domain::CurrencyCode currency{
      domain::CurrencyCode::kLucci};  ///< 实际扣款币种。
  std::int64_t charged_amount{0};     ///< 从钱包扣除的非负整数。
  std::int64_t balance_after{0};      ///< 提交后的该币种余额。
  std::int64_t quantity_after{0};     ///< 提交后的库存数量。
  std::optional<std::int64_t>
      expires_at_unix_seconds;  ///< 提交后的期限；永久为空。
};

/** @brief 占用内部操作标识后的状态；已完成项携带原提交结果。 */
enum class PurchaseOperationState : std::uint8_t {
  kNew,
  kCompletedReplay,
};

/** @brief 操作占用结果；新操作不带结果，完成重放必须带结果。 */
struct PurchaseOperationClaim final {
  PurchaseOperationState state{
      PurchaseOperationState::kNew};  ///< 当前事务应继续或直接重放。
  std::optional<PurchaseResult> completed_result;  ///< 已完成操作的持久化结果。
};

/** @brief 一个订单中已验证并写入的真实库存部件。 */
struct PurchaseItemGrant final {
  domain::PurchaseOffer offer;
  domain::PurchaseInventoryEffect effect;
  domain::ItemInstanceId item_instance_id;
};

/**
 * @brief 购买用例独占数据库事务端口。
 * @note 所有方法由同一工作线程、同一连接和同一事务顺序调用；实现不得自行提交。
 */
class PurchaseTransaction {
 public:
  virtual ~PurchaseTransaction() = default;

  /**
   * @brief 占用 operation_id 或读取已完成结果。
   * @note 同标识不同玩家/stock
   * 必须返回冲突；未完成的异常持久项必须返回无效状态。
   */
  [[nodiscard]] virtual ServiceResult<PurchaseOperationClaim> ClaimOperation(
      const PurchaseCommand& command) = 0;

  /** @brief 读取当前已发布、启用的销售规格及模板限制。 */
  [[nodiscard]] virtual ServiceResult<domain::PurchaseOffer> LoadOffer(
      domain::StockId stock_id) = 0;

  /** @brief 同一已锁定规格中的全部部件；普通商品恰好一项，套装保持原始顺序。 */
  [[nodiscard]] virtual ServiceResult<std::vector<domain::PurchaseOffer>>
  LoadItems(const domain::PurchaseOffer& offer) = 0;

  /** @brief 锁定玩家同模板库存行；不存在返回空值。 */
  [[nodiscard]] virtual ServiceResult<std::optional<domain::OwnedItemSnapshot>>
  LockOwnedItem(domain::PlayerId player_id, domain::ItemId item_id) = 0;

  /** @brief 通过余额条件更新扣款并返回前后值；余额不足返回
   * kInsufficientBalance。 */
  [[nodiscard]] virtual ServiceResult<WalletDebit> DebitWallet(
      domain::PlayerId player_id, domain::CurrencyCode currency,
      std::int64_t amount) = 0;

  /** @brief 按领域效果插入或更新库存，并返回属于该玩家的实例 ID。 */
  [[nodiscard]] virtual ServiceResult<domain::ItemInstanceId> ApplyInventory(
      domain::PlayerId player_id, const domain::PurchaseOffer& offer,
      const domain::PurchaseInventoryEffect& effect,
      std::int64_t now_unix_seconds) = 0;

  /**
   * @brief 写入 settled 订单/订单行、钱包流水和库存流水。
   * @note 流水必须使用 command.operation_id 作为 business_id，订单行保存不可变
   * offer 快照。
   */
  [[nodiscard]] virtual ServiceResult<domain::OrderId>
  CreateSettledOrderAndAudits(const PurchaseCommand& command,
                              const domain::PurchaseOffer& offer,
                              std::span<const PurchaseItemGrant> items,
                              const WalletDebit& debit,
                              std::int64_t now_unix_seconds) = 0;

  /** @brief 将操作占位更新为 succeeded
   * 并关联订单；必须是事务内最后一项业务写入。 */
  [[nodiscard]] virtual std::optional<ServiceError> CompleteOperation(
      const domain::OperationId& operation_id, domain::OrderId order_id) = 0;

  /** @brief 原子发布扣款、库存、订单、流水和操作结果。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 购买事务工厂，由数据库工作线程上的 MySQL adapter 实现。 */
class PurchaseUnitOfWork {
 public:
  virtual ~PurchaseUnitOfWork() = default;

  /** @brief 开始独占连接的短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<std::unique_ptr<PurchaseTransaction>>
  Begin() = 0;
};

/** @brief 运行时购买路由依赖的业务用例接口。 */
class PurchaseExecutor {
 public:
  virtual ~PurchaseExecutor() = default;

  /** @brief 执行或重放一次带内部操作标识的购买。 */
  [[nodiscard]] virtual ServiceResult<PurchaseResult> Purchase(
      const PurchaseCommand& command) const = 0;
};

/**
 * @brief 在一个事务中执行内部幂等的条件扣款和库存发放。
 * @note 事务开始后不等待网络；目录、钱包和库存事实都必须在该事务内读取或锁定。
 */
class PurchaseService final : public PurchaseExecutor {
 public:
  /** @brief 注入 UTC 时钟和购买事务端口；生命周期必须覆盖本服务。 */
  PurchaseService(const SystemClock& clock,
                  PurchaseUnitOfWork& unit_of_work) noexcept;

  /** @brief 执行或重放购买；任一失败均依靠事务析构回滚全部中间写入。 */
  [[nodiscard]] ServiceResult<PurchaseResult> Purchase(
      const PurchaseCommand& command) const override;

 private:
  [[nodiscard]] ServiceResult<PurchaseResult> TryPurchase(
      const PurchaseCommand& command, bool retry_on_commit_failure) const;

  const SystemClock& clock_;
  PurchaseUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_PURCHASE_SERVICE_H_
