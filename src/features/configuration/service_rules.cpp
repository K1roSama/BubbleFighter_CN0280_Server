#include "features/configuration/service_rules.h"

#include <array>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include "application/utf8.h"

namespace bf::application {
namespace {

constexpr std::array<std::uint64_t, 11U> kNeedles{
    100382U, 100383U, 100385U, 100473U, 100598U, 100640U,
    100650U, 100976U, 101291U, 102261U, 102303U};
constexpr std::uint64_t kSuperNeedle = 100383U;
constexpr std::size_t kMaximumRewardPool = 32U;
constexpr std::size_t kMaximumMysteryOffers = 10'000U;
constexpr std::size_t kMaximumWorkshopRules = 10'000U;

ServiceError ContractError(std::string message) {
  return {.code = ErrorCode::kInternal,
          .safe_message = "已发布扩展业务规则损坏：" + std::move(message)};
}

bool ValidItem(domain::ItemId item_id) noexcept {
  return item_id.IsValid() &&
         item_id.Value() <= std::numeric_limits<std::uint32_t>::max();
}

bool ValidStock(domain::StockId stock_id) noexcept {
  return stock_id.Value() >= 1'000'000U && stock_id.Value() < 2'000'000U;
}

std::optional<ServiceError> ValidateBubbles(const BubbleServiceRules& rules) {
  if (rules.selection_duration <= std::chrono::seconds::zero() ||
      rules.selection_duration > std::chrono::hours{1} ||
      rules.super_needle_item_id.Value() != kSuperNeedle ||
      rules.needles.size() != kNeedles.size()) {
    return ContractError("泡泡基础字段不符合 build 752 契约");
  }
  for (std::size_t index = 0U; index < rules.needles.size(); ++index) {
    const auto& needle = rules.needles[index];
    const auto expected_cost =
        needle.item_id.Value() == kSuperNeedle ? 10U : 1U;
    if (needle.item_id.Value() != kNeedles[index] ||
        needle.cost != expected_cost) {
      return ContractError("魔法针顺序或消耗不符合客户端分支");
    }
  }
  if (rules.reward_pool.size() < 5U ||
      rules.reward_pool.size() > kMaximumRewardPool) {
    return ContractError("泡泡奖励池数量越界");
  }
  std::set<std::uint64_t> stocks;
  for (const auto& reward : rules.reward_pool) {
    if (!ValidStock(reward.stock_id) || !ValidItem(reward.item_id) ||
        reward.quantity == 0U ||
        !stocks.insert(reward.stock_id.Value()).second ||
        (reward.duration.has_value() &&
         (*reward.duration <= std::chrono::seconds::zero() ||
          reward.quantity != 1U))) {
      return ContractError("泡泡奖励快照的 ID、数量、期限或唯一性无效");
    }
  }
  return std::nullopt;
}

std::optional<ServiceError> ValidateMystery(
    const MysteryShopServiceRules& rules) {
  if (rules.refresh_item_id.Value() != 190128U ||
      rules.refresh_quantity != 1U || rules.selection_size == 0U ||
      rules.selection_size > 32U ||
      rules.selection_ttl <= std::chrono::seconds::zero() ||
      rules.selection_ttl > std::chrono::hours{24} ||
      rules.offers.size() < rules.selection_size ||
      rules.offers.size() > kMaximumMysteryOffers) {
    return ContractError("神秘商店基础字段或候选数量无效");
  }
  std::set<std::uint64_t> stocks;
  std::set<std::uint64_t> items;
  std::uint64_t previous_stock = 0U;
  for (const auto& offer : rules.offers) {
    const auto code_points = domain::CountUtf8CodePoints(offer.item_name);
    if (!ValidStock(offer.stock_id) || !ValidItem(offer.item_id) ||
        offer.stock_id.Value() <= previous_stock ||
        !stocks.insert(offer.stock_id.Value()).second ||
        !code_points.has_value() || *code_points == 0U || *code_points > 128U ||
        offer.price == 0U || offer.price > 10'000U ||
        offer.duration < std::chrono::days{1} ||
        offer.duration > std::chrono::days{30} ||
        offer.duration.count() %
                std::chrono::seconds{std::chrono::days{1}}.count() !=
            0) {
      return ContractError("神秘商店候选的 ID、排序、文本、价格或期限无效");
    }
    previous_stock = offer.stock_id.Value();
    items.insert(offer.item_id.Value());
  }
  if (items.size() < rules.selection_size) {
    return ContractError("神秘商店不同物品候选不足");
  }
  return std::nullopt;
}

std::optional<ServiceError> ValidateWorkshop(
    const WorkshopServiceRules& rules) {
  if (rules.compound_card_item_id.Value() != 100622U ||
      rules.compound_min == 0U || rules.compound_min > rules.compound_max ||
      rules.compound_max > 32U ||
      rules.compound_duration <= std::chrono::seconds::zero() ||
      rules.decompose_card_item_id.Value() != 100623U ||
      rules.decompose_max == 0U || rules.decompose_max > 32U ||
      !ValidItem(rules.decompose_reward_item_id) ||
      rules.permanent_material_item_id.Value() != 190151U ||
      rules.item_rules.empty() ||
      rules.item_rules.size() > kMaximumWorkshopRules) {
    return ContractError("工坊基础字段无效");
  }
  if (rules.compound_policy.has_value()) {
    const auto& policy = *rules.compound_policy;
    if (policy.version != 2U || policy.policy_epoch == 0U ||
        policy.input_count != 5U ||
        policy.protection_material_item_id.Value() != 190110U ||
        policy.minimum_remaining_days != 20U ||
        policy.maximum_remaining_days != 10'000U ||
        policy.permanent_threshold_days != 720U ||
        policy.minimum_percent != 50U || policy.maximum_percent != 100U ||
        policy.probability_policy != "uniform-input-independent-percent-v1" ||
        policy.protected_merge_policy !=
            "add-retained-after-generated-threshold-v1" ||
        policy.catalog_sha256.size() != 64U ||
        policy.catalog_sha256.find_first_not_of("0123456789abcdef") !=
            std::string::npos) {
      return ContractError("工坊132新政策与已恢复公开约束或本服政策标识不一致");
    }
  }
  std::uint64_t previous_item = 0U;
  for (const auto& item : rules.item_rules) {
    if (!ValidItem(item.item_id) || item.item_id.Value() <= previous_item ||
        (item.compound_reward_candidate && !item.compound) ||
        (rules.compound_policy.has_value() &&
         (item.native_is_gashapon > 1U || item.native_is_compound > 1U ||
          item.native_duration_type.empty() ||
          item.compound != (item.native_is_compound == 1U))) ||
        (!item.compound && !item.decompose && !item.permanent)) {
      return ContractError("工坊物品规则未严格排序或没有允许动作");
    }
    previous_item = item.item_id.Value();
  }
  return std::nullopt;
}

}  // namespace

ServiceRulesService::ServiceRulesService(ServiceRulesReader& reader) noexcept
    : reader_(reader) {}

ServiceResult<PublishedServiceRules> ServiceRulesService::Load() const {
  auto result = reader_.LoadPublished();
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto rules =
      std::get<std::optional<PublishedServiceRules>>(std::move(result));
  if (!rules.has_value()) {
    return ServiceError{.code = ErrorCode::kInvalidState,
                        .safe_message = "尚未发布可用扩展业务规则"};
  }
  if (rules->configuration_version_id == 0U) {
    return ContractError("配置版本标识为零");
  }
  if (auto error = ValidateBubbles(rules->bubbles)) {
    return *error;
  }
  if (auto error = ValidateMystery(rules->mystery_shop)) {
    return *error;
  }
  if (auto error = ValidateWorkshop(rules->workshop)) {
    return *error;
  }
  return std::move(*rules);
}

}  // namespace bf::application
