#include "features/shop/purchase.h"

#include <algorithm>
#include <limits>

#include "features/inventory/life_item.h"

namespace bf::domain {
namespace {

bool ValidCommonOffer(const PurchaseOffer& offer,
                      std::int64_t now_unix_seconds) noexcept {
  return offer.stock_id.IsValid() && offer.item_id.IsValid() &&
         now_unix_seconds >= 0 && offer.unit_price >= 0 &&
         offer.unit_price <= std::numeric_limits<std::int32_t>::max() &&
         offer.grant_quantity > 0 && offer.max_quantity > 0 &&
         (!offer.max_duration_seconds.has_value() ||
          *offer.max_duration_seconds > 0) &&
         (!offer.duration_seconds.has_value() || *offer.duration_seconds > 0);
}

bool ValidOwnedItem(const PurchaseOffer& offer,
                    const std::optional<OwnedItemSnapshot>& owned) noexcept {
  return !owned.has_value() ||
         (owned->item_instance_id.IsValid() &&
          owned->item_id == offer.item_id &&
          (owned->quantity > 0 ||
           (owned->quantity == 0 && IsLifeItem(offer.item_id.Value()) &&
            offer.stack_policy == ItemStackPolicy::kMergeQuantity &&
            offer.duration_policy == ItemDurationPolicy::kPermanent &&
            offer.max_quantity == kLifeMaximum &&
            !owned->expires_at_unix_seconds)));
}

std::variant<PurchaseInventoryEffect, PurchaseRuleError>
CalculateQuantityEffect(
    const PurchaseOffer& offer,
    const std::optional<OwnedItemSnapshot>& owned) noexcept {
  if (offer.duration_policy != ItemDurationPolicy::kPermanent ||
      offer.duration_seconds.has_value() ||
      offer.max_duration_seconds.has_value()) {
    return PurchaseRuleError::kInvalidOffer;
  }
  const std::int64_t quantity_before = owned.has_value() ? owned->quantity : 0;
  if ((owned.has_value() && owned->expires_at_unix_seconds.has_value()) ||
      offer.grant_quantity > offer.max_quantity - quantity_before) {
    return owned.has_value() && owned->expires_at_unix_seconds.has_value()
               ? PurchaseRuleError::kInvalidInventory
               : PurchaseRuleError::kQuantityLimitExceeded;
  }
  return PurchaseInventoryEffect{
      .quantity_after = quantity_before + offer.grant_quantity,
      .quantity_delta = offer.grant_quantity,
      .expires_before = std::nullopt,
      .expires_after = std::nullopt};
}

std::variant<PurchaseInventoryEffect, PurchaseRuleError> CalculateSingleEffect(
    const PurchaseOffer& offer, const std::optional<OwnedItemSnapshot>& owned,
    std::int64_t now_unix_seconds) noexcept {
  if (offer.grant_quantity != 1 || offer.max_quantity != 1) {
    return PurchaseRuleError::kInvalidOffer;
  }

  if (!offer.duration_seconds.has_value()) {
    if (offer.duration_policy == ItemDurationPolicy::kExpiring ||
        (offer.duration_policy == ItemDurationPolicy::kPermanent &&
         offer.max_duration_seconds.has_value()) ||
        (offer.duration_policy == ItemDurationPolicy::kOfferDefined &&
         !offer.max_duration_seconds.has_value())) {
      return PurchaseRuleError::kInvalidOffer;
    }
    if (owned.has_value() && !owned->expires_at_unix_seconds.has_value()) {
      return PurchaseRuleError::kAlreadyOwnedPermanent;
    }
    return PurchaseInventoryEffect{
        .quantity_after = 1,
        .quantity_delta = owned.has_value() ? 0 : 1,
        .expires_before =
            owned.has_value() ? owned->expires_at_unix_seconds : std::nullopt,
        .expires_after = std::nullopt};
  }

  if (offer.duration_policy == ItemDurationPolicy::kPermanent ||
      !offer.max_duration_seconds.has_value()) {
    return PurchaseRuleError::kInvalidOffer;
  }
  if (owned.has_value() && !owned->expires_at_unix_seconds.has_value()) {
    return PurchaseRuleError::kAlreadyOwnedPermanent;
  }

  const std::int64_t base =
      owned.has_value() && owned->expires_at_unix_seconds.has_value()
          ? std::max(now_unix_seconds, *owned->expires_at_unix_seconds)
          : now_unix_seconds;
  if (*offer.duration_seconds >
          std::numeric_limits<std::int64_t>::max() - base ||
      *offer.max_duration_seconds >
          std::numeric_limits<std::int64_t>::max() - now_unix_seconds) {
    return PurchaseRuleError::kInvalidOffer;
  }
  const auto expires_after = base + *offer.duration_seconds;
  if (expires_after > now_unix_seconds + *offer.max_duration_seconds) {
    return PurchaseRuleError::kDurationLimitExceeded;
  }
  return PurchaseInventoryEffect{
      .quantity_after = 1,
      .quantity_delta = owned.has_value() ? 0 : 1,
      .expires_before =
          owned.has_value() ? owned->expires_at_unix_seconds : std::nullopt,
      .expires_after = expires_after};
}

}  // namespace

std::variant<PurchaseInventoryEffect, PurchaseRuleError>
CalculatePurchaseInventoryEffect(const PurchaseOffer& offer,
                                 const std::optional<OwnedItemSnapshot>& owned,
                                 std::int64_t now_unix_seconds) noexcept {
  if (!ValidCommonOffer(offer, now_unix_seconds)) {
    return PurchaseRuleError::kInvalidOffer;
  }
  if (!ValidOwnedItem(offer, owned)) {
    return PurchaseRuleError::kInvalidInventory;
  }
  if (offer.stack_policy == ItemStackPolicy::kMergeQuantity) {
    return CalculateQuantityEffect(offer, owned);
  }
  return CalculateSingleEffect(offer, owned, now_unix_seconds);
}

std::string_view PurchaseRuleErrorName(PurchaseRuleError error) noexcept {
  switch (error) {
    case PurchaseRuleError::kInvalidOffer:
      return "invalid_offer";
    case PurchaseRuleError::kInvalidInventory:
      return "invalid_inventory";
    case PurchaseRuleError::kAlreadyOwnedPermanent:
      return "already_owned_permanent";
    case PurchaseRuleError::kQuantityLimitExceeded:
      return "quantity_limit_exceeded";
    case PurchaseRuleError::kDurationLimitExceeded:
      return "duration_limit_exceeded";
  }
  return "unknown";
}

std::string_view CurrencyCodeName(CurrencyCode currency) noexcept {
  switch (currency) {
    case CurrencyCode::kLucci:
      return "lucci";
    case CurrencyCode::kBubbleCash:
      return "bubble_cash";
    case CurrencyCode::kTreasureBubble:
      return "treasure_bubble";
  }
  return "unknown";
}

}  // namespace bf::domain
