#include "features/shop/purchase_service.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError InvalidCommand() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "购买请求缺少有效的玩家或销售规格标识"};
}

ServiceError TransactionContractError(std::string_view detail) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::string{"购买事务返回了无效状态："} +
                                      std::string{detail}};
}

ServiceError RuleError(domain::PurchaseRuleError error) {
  switch (error) {
    case domain::PurchaseRuleError::kAlreadyOwnedPermanent:
      return ServiceError{.code = ErrorCode::kConflict,
                          .safe_message = "该永久物品已经拥有"};
    case domain::PurchaseRuleError::kQuantityLimitExceeded:
      return ServiceError{.code = ErrorCode::kConflict,
                          .safe_message = "物品数量已达到上限"};
    case domain::PurchaseRuleError::kDurationLimitExceeded:
      return ServiceError{.code = ErrorCode::kConflict,
                          .safe_message = "物品期限已达到上限"};
    case domain::PurchaseRuleError::kInvalidOffer:
      return TransactionContractError("已发布商品违反领域约束");
    case domain::PurchaseRuleError::kInvalidInventory:
      return TransactionContractError("当前库存违反领域约束");
  }
  return TransactionContractError("未知领域规则错误");
}

bool ValidResult(const PurchaseResult& result,
                 const PurchaseCommand& command) noexcept {
  return result.order_id.IsValid() && result.player_id == command.player_id &&
         result.stock_id == command.stock_id && result.item_id.IsValid() &&
         result.item_instance_id.IsValid() && result.charged_amount >= 0 &&
         result.balance_after >= 0 && result.quantity_after > 0 &&
         (!result.expires_at_unix_seconds.has_value() ||
          *result.expires_at_unix_seconds >= 0);
}

ServiceResult<std::optional<PurchaseResult>> ValidateClaim(
    const PurchaseOperationClaim& claim, const PurchaseCommand& command) {
  if (claim.state == PurchaseOperationState::kNew) {
    if (claim.completed_result.has_value()) {
      return TransactionContractError("新操作携带了历史结果");
    }
    return std::optional<PurchaseResult>{};
  }
  if (!claim.completed_result.has_value() ||
      !ValidResult(*claim.completed_result, command)) {
    return TransactionContractError("幂等重放结果缺失或与请求不一致");
  }
  return claim.completed_result;
}

ServiceResult<std::int64_t> UnixSeconds(const SystemClock& clock) {
  const auto count = std::chrono::duration_cast<std::chrono::seconds>(
                         clock.Now().time_since_epoch())
                         .count();
  static_assert(sizeof(count) <= sizeof(std::int64_t));
  if (count < 0) {
    return TransactionContractError("系统时间早于 Unix epoch");
  }
  return static_cast<std::int64_t>(count);
}

bool ValidDebit(const WalletDebit& debit,
                const domain::PurchaseOffer& offer) noexcept {
  return debit.currency == offer.currency && debit.balance_before >= 0 &&
         debit.balance_after >= 0 && offer.unit_price <= debit.balance_before &&
         debit.balance_after == debit.balance_before - offer.unit_price;
}

ServiceResult<PurchaseResult> ExecuteNewPurchase(
    PurchaseTransaction& transaction, const PurchaseCommand& command,
    std::int64_t now) {
  auto offer_result = transaction.LoadOffer(command.stock_id);
  if (const auto* error = std::get_if<ServiceError>(&offer_result)) {
    return *error;
  }
  const auto offer = std::get<domain::PurchaseOffer>(offer_result);
  if (offer.stock_id != command.stock_id) {
    return TransactionContractError("目录返回了不同的销售规格");
  }

  auto items_result = transaction.LoadItems(offer);
  if (const auto* error = std::get_if<ServiceError>(&items_result)) {
    return *error;
  }
  const auto items =
      std::get<std::vector<domain::PurchaseOffer>>(std::move(items_result));
  if (items.empty() || items.size() > 10U ||
      items.front().item_id != offer.item_id || offer.unit_price < 0 ||
      offer.unit_price > std::numeric_limits<std::int32_t>::max()) {
    return TransactionContractError("商品部件数量或价格无效");
  }
  std::set<std::uint64_t> unique_items;
  for (const auto& item : items) {
    if (item.stock_id != offer.stock_id || item.currency != offer.currency ||
        !unique_items.insert(item.item_id.Value()).second) {
      return TransactionContractError("商品部件重复或与销售规格不一致");
    }
  }
  // All purchases lock shared components in the same order. Preserve native
  // order in the receipt.
  std::vector<std::size_t> lock_order(items.size());
  std::iota(lock_order.begin(), lock_order.end(), 0U);
  std::ranges::sort(lock_order, [&](auto left, auto right) {
    return items[left].item_id.Value() < items[right].item_id.Value();
  });
  std::vector<PurchaseItemGrant> grants(items.size());
  for (const auto index : lock_order) {
    const auto& item = items[index];
    auto owned_result =
        transaction.LockOwnedItem(command.player_id, item.item_id);
    if (const auto* error = std::get_if<ServiceError>(&owned_result)) {
      return *error;
    }
    const auto owned =
        std::get<std::optional<domain::OwnedItemSnapshot>>(owned_result);
    const auto effect_result =
        domain::CalculatePurchaseInventoryEffect(item, owned, now);
    if (const auto* error =
            std::get_if<domain::PurchaseRuleError>(&effect_result)) {
      return RuleError(*error);
    }
    grants[index] = {
        .offer = item,
        .effect = std::get<domain::PurchaseInventoryEffect>(effect_result),
        .item_instance_id = {}};
  }

  auto debit_result = transaction.DebitWallet(command.player_id, offer.currency,
                                              offer.unit_price);
  if (const auto* error = std::get_if<ServiceError>(&debit_result)) {
    return *error;
  }
  const auto debit = std::get<WalletDebit>(debit_result);
  if (!ValidDebit(debit, offer)) {
    return TransactionContractError("钱包扣款前后值不满足成交价格");
  }

  for (auto& grant : grants) {
    auto inventory_result = transaction.ApplyInventory(
        command.player_id, grant.offer, grant.effect, now);
    if (const auto* error = std::get_if<ServiceError>(&inventory_result)) {
      return *error;
    }
    grant.item_instance_id = std::get<domain::ItemInstanceId>(inventory_result);
    if (!grant.item_instance_id.IsValid()) {
      return TransactionContractError("库存写入没有返回有效实例标识");
    }
  }
  const auto& effect = grants.front().effect;
  const auto item_instance_id = grants.front().item_instance_id;
  auto order_result = transaction.CreateSettledOrderAndAudits(
      command, offer, grants, debit, now);
  if (const auto* error = std::get_if<ServiceError>(&order_result)) {
    return *error;
  }
  const auto order_id = std::get<domain::OrderId>(order_result);
  if (!order_id.IsValid()) {
    return TransactionContractError("订单写入没有返回有效订单标识");
  }
  if (auto error =
          transaction.CompleteOperation(command.operation_id, order_id)) {
    return *error;
  }

  return PurchaseResult{.order_id = order_id,
                        .player_id = command.player_id,
                        .stock_id = offer.stock_id,
                        .item_id = offer.item_id,
                        .item_instance_id = item_instance_id,
                        .currency = offer.currency,
                        .charged_amount = offer.unit_price,
                        .balance_after = debit.balance_after,
                        .quantity_after = effect.quantity_after,
                        .expires_at_unix_seconds = effect.expires_after};
}

}  // namespace

PurchaseService::PurchaseService(const SystemClock& clock,
                                 PurchaseUnitOfWork& unit_of_work) noexcept
    : clock_(clock), unit_of_work_(unit_of_work) {}

ServiceResult<PurchaseResult> PurchaseService::Purchase(
    const PurchaseCommand& command) const {
  if (!command.player_id.IsValid() || !command.stock_id.IsValid()) {
    return InvalidCommand();
  }
  return TryPurchase(command, true);
}

ServiceResult<PurchaseResult> PurchaseService::TryPurchase(
    const PurchaseCommand& command, bool retry_on_commit_failure) const {
  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction =
      std::get<std::unique_ptr<PurchaseTransaction>>(std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("事务工厂返回空对象");
  }

  auto claim_result = transaction->ClaimOperation(command);
  if (const auto* error = std::get_if<ServiceError>(&claim_result)) {
    return *error;
  }
  auto replay_result =
      ValidateClaim(std::get<PurchaseOperationClaim>(claim_result), command);
  if (const auto* error = std::get_if<ServiceError>(&replay_result)) {
    return *error;
  }
  auto replay =
      std::get<std::optional<PurchaseResult>>(std::move(replay_result));
  if (replay.has_value()) {
    if (auto error = transaction->Commit()) {
      return *error;
    }
    return *replay;
  }

  const auto time_result = UnixSeconds(clock_);
  if (const auto* error = std::get_if<ServiceError>(&time_result)) {
    return *error;
  }
  auto result = ExecuteNewPurchase(*transaction, command,
                                   std::get<std::int64_t>(time_result));
  if (std::holds_alternative<ServiceError>(result)) {
    return result;
  }
  if (auto error = transaction->Commit()) {
    if (retry_on_commit_failure && error->is_retryable) {
      // Release the failed connection and resolve the original key in a new
      // transaction. ClaimOperation reads the committed receipt before
      // charging.
      transaction.reset();
      return TryPurchase(command, false);
    }
    return *error;
  }
  return result;
}

}  // namespace bf::application
