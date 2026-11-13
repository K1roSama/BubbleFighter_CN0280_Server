#include "features/shop/mystery_shop_service.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError Invalid(std::string message) {
  return {.code = ErrorCode::kInvalidInput, .safe_message = std::move(message)};
}

ServiceError Contract(std::string message) {
  return {.code = ErrorCode::kInternal,
          .safe_message = "神秘商店事务返回无效状态：" + std::move(message)};
}

bool SameOffer(const MysteryOfferRule& left,
               const MysteryOfferRule& right) noexcept {
  return left.stock_id == right.stock_id && left.item_id == right.item_id &&
         left.item_name == right.item_name && left.price == right.price &&
         left.duration == right.duration;
}

bool ValidSnapshot(const MysterySelectionSnapshot& snapshot,
                   domain::PlayerId player_id,
                   const MysteryShopServiceRules& rules,
                   std::chrono::system_clock::time_point now) {
  if (snapshot.player_id != player_id || snapshot.generation == 0U ||
      snapshot.rules_version_id == 0U || snapshot.expires_at <= now ||
      snapshot.offers.size() != rules.selection_size ||
      rules.selection_size > 32U ||
      (rules.selection_size < 32U &&
       (snapshot.purchased_mask >> rules.selection_size) != 0U)) {
    return false;
  }
  std::set<std::uint64_t> stocks;
  std::set<std::uint64_t> items;
  for (const auto& offer : snapshot.offers) {
    const auto published = std::ranges::find(rules.offers, offer.stock_id,
                                             &MysteryOfferRule::stock_id);
    if (published == rules.offers.end() || !SameOffer(offer, *published) ||
        !stocks.insert(offer.stock_id.Value()).second ||
        !items.insert(offer.item_id.Value()).second) {
      return false;
    }
  }
  return true;
}

ServiceResult<PublishedServiceRules> LoadRules(ServiceRulesLoader& loader) {
  auto loaded = loader.Load();
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  return std::get<PublishedServiceRules>(std::move(loaded));
}

}  // namespace

ServiceResult<std::vector<MysteryOfferRule>> SelectMysteryOffers(
    domain::PlayerId player_id, std::uint64_t generation,
    std::span<const domain::ItemId> owned_permanent_items,
    const MysteryShopServiceRules& rules) {
  if (!player_id.IsValid() || generation == 0U || rules.selection_size == 0U ||
      rules.selection_size > 32U) {
    return Invalid("神秘商店选品参数无效");
  }
  std::set<std::uint64_t> owned;
  for (const auto item : owned_permanent_items) {
    if (item.IsValid()) {
      owned.insert(item.Value());
    }
  }
  std::vector<const MysteryOfferRule*> pool;
  pool.reserve(rules.offers.size());
  for (const auto& offer : rules.offers) {
    if (!owned.contains(offer.item_id.Value())) {
      pool.push_back(&offer);
    }
  }
  if (pool.empty()) {
    return ServiceError{.code = ErrorCode::kConflict,
                        .safe_message = "没有可展示的神秘商店商品"};
  }
  const auto mixed = player_id.Value() * 17U + generation * 6U;
  const auto start = static_cast<std::size_t>(mixed % pool.size());
  std::set<std::uint64_t> seen_items;
  std::vector<MysteryOfferRule> selected;
  selected.reserve(rules.selection_size);
  for (std::size_t offset = 0U;
       offset < pool.size() && selected.size() < rules.selection_size;
       ++offset) {
    const auto& offer = *pool[(start + offset) % pool.size()];
    if (seen_items.insert(offer.item_id.Value()).second) {
      selected.push_back(offer);
    }
  }
  if (selected.size() != rules.selection_size) {
    return ServiceError{.code = ErrorCode::kConflict,
                        .safe_message = "可展示的不同神秘商店物品不足"};
  }
  return selected;
}

MysteryShopService::MysteryShopService(
    const SystemClock& clock, ServiceRulesLoader& rules,
    MysteryShopRepository& repository) noexcept
    : clock_(clock), rules_(rules), repository_(repository) {}

ServiceResult<MysterySelectionSnapshot> MysteryShopService::State(
    domain::PlayerId player_id) const {
  if (!player_id.IsValid()) {
    return Invalid("玩家标识无效");
  }
  auto loaded = LoadRules(rules_);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  const auto& all = std::get<PublishedServiceRules>(loaded);
  const auto now = clock_.Now();
  auto result = repository_.LoadOrCreate(
      player_id, all.configuration_version_id, all.mystery_shop, now);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  if (!ValidSnapshot(std::get<MysterySelectionSnapshot>(result), player_id,
                     all.mystery_shop, now)) {
    return Contract("选品快照与发布规则不一致");
  }
  return result;
}

ServiceResult<MysterySelectionSnapshot> MysteryShopService::Refresh(
    domain::OperationId operation_id, domain::PlayerId player_id,
    std::uint64_t expected_generation) const {
  if (!player_id.IsValid() || expected_generation == 0U) {
    return Invalid("神秘商店刷新参数无效");
  }
  auto loaded = LoadRules(rules_);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  const auto& all = std::get<PublishedServiceRules>(loaded);
  const auto now = clock_.Now();
  auto result =
      repository_.Refresh(operation_id, player_id, expected_generation,
                          all.mystery_shop, all.configuration_version_id, now);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  const auto& snapshot = std::get<MysterySelectionSnapshot>(result);
  if (!ValidSnapshot(snapshot, player_id, all.mystery_shop, now) ||
      snapshot.generation <= expected_generation ||
      snapshot.purchased_mask != 0U) {
    return Contract("刷新结果代次或购买位图无效");
  }
  return result;
}

ServiceResult<MysteryPurchaseResult> MysteryShopService::Purchase(
    domain::OperationId operation_id, domain::PlayerId player_id,
    std::uint64_t expected_generation, domain::StockId stock_id) const {
  if (!player_id.IsValid() || expected_generation == 0U ||
      !stock_id.IsValid()) {
    return Invalid("神秘商店购买参数无效");
  }
  auto loaded = LoadRules(rules_);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) {
    return *error;
  }
  const auto& all = std::get<PublishedServiceRules>(loaded);
  const auto now = clock_.Now();
  auto result = repository_.Purchase(
      operation_id, player_id, expected_generation, stock_id,
      all.configuration_version_id, all.mystery_shop, now);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  const auto& purchased = std::get<MysteryPurchaseResult>(result);
  const auto selected = std::ranges::find(purchased.selection.offers, stock_id,
                                          &MysteryOfferRule::stock_id);
  if (!ValidSnapshot(purchased.selection, player_id, all.mystery_shop, now) ||
      purchased.selection.generation != expected_generation ||
      selected == purchased.selection.offers.end() ||
      purchased.reward_item_id != selected->item_id) {
    return Contract("购买结果与当前选品不一致");
  }
  return result;
}

}  // namespace bf::application
