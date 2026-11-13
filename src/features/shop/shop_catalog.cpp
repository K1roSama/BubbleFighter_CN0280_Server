#include "features/shop/shop_catalog.h"

#include <limits>
#include <set>
#include <string>
#include <utility>
#include <variant>

#include "application/utf8.h"

namespace bf::application {
namespace {

constexpr std::size_t kMaximumCards = 10'000U;
constexpr std::size_t kMaximumPricesPerCard = 64U;
constexpr std::size_t kMaximumCategories = 64U;
constexpr std::size_t kMaximumSubcategories = 256U;
constexpr std::size_t kMaximumLabelCodePoints = 128U;

ServiceError ContractError(std::string message) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::move(message)};
}

bool ValidText(std::string_view text) {
  const auto count = domain::CountUtf8CodePoints(text);
  return count.has_value() && *count > 0U && *count <= kMaximumLabelCodePoints;
}

std::optional<ServiceError> ValidateCards(
    const std::vector<ShopCardView>& cards,
    const std::set<std::uint64_t>& valid_items,
    const std::set<std::uint32_t>& wildcard_categories,
    std::set<std::uint64_t>& stock_ids) {
  if (cards.size() > kMaximumCards) {
    return ContractError("发布目录商品数量超过上限");
  }
  std::set<std::uint64_t> card_items;
  for (const auto& card : cards) {
    if (!card.item_id.IsValid() ||
        !card_items.insert(card.item_id.Value()).second) {
      return ContractError("发布目录商品卡片包含无效或重复 item_id");
    }
    if (!ValidText(card.item_name)) {
      return ContractError("发布目录商品卡片包含无效 UTF-8 名称");
    }
    if (card.main_category == 0U || card.subcategory == 0U ||
        (!valid_items.contains(
             (static_cast<std::uint64_t>(card.main_category) << 32U) |
             card.subcategory) &&
         !wildcard_categories.contains(card.main_category))) {
      return ContractError("发布目录商品卡片引用了未发布分类：item=" +
                           std::to_string(card.item_id.Value()) +
                           " main=" + std::to_string(card.main_category) +
                           " sub=" + std::to_string(card.subcategory));
    }
    if (card.prices.empty() || card.prices.size() > kMaximumPricesPerCard) {
      return ContractError("发布目录商品卡片销售规格数量越界");
    }
    for (const auto& price : card.prices) {
      const bool permanent =
          price.duration_type == ClientStockDurationType::kPermanent &&
          price.duration_value == std::numeric_limits<std::uint32_t>::max();
      const bool timed =
          price.duration_type == ClientStockDurationType::kTimedDays &&
          price.duration_value > 0U &&
          price.duration_value < std::numeric_limits<std::uint32_t>::max();
      if (!price.stock_id.IsValid() || price.item_id != card.item_id ||
          !stock_ids.insert(price.stock_id.Value()).second ||
          (!permanent && !timed)) {
        return ContractError("发布目录销售规格不满足 ID、期限或唯一性契约");
      }
    }
  }
  return std::nullopt;
}

}  // namespace

ShopCatalogService::ShopCatalogService(ShopCatalogReader& reader) noexcept
    : reader_(reader) {}

ServiceResult<PublishedShopCatalog> ShopCatalogService::Load() const {
  auto result = reader_.LoadPublished();
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto optional_catalog =
      std::get<std::optional<PublishedShopCatalog>>(std::move(result));
  if (!optional_catalog.has_value()) {
    return ServiceError{.code = ErrorCode::kInvalidState,
                        .safe_message = "尚未发布可用商城目录"};
  }
  auto catalog = std::move(*optional_catalog);
  if (catalog.revision == 0U || catalog.categories.empty() ||
      catalog.categories.size() > kMaximumCategories) {
    return ContractError("发布目录版本或分类数量无效");
  }

  std::set<std::uint32_t> main_ids;
  std::set<std::uint64_t> valid_items;
  std::set<std::uint32_t> wildcard_categories;
  for (const auto& category : catalog.categories) {
    if (category.category_id == 0U ||
        !main_ids.insert(category.category_id).second ||
        !ValidText(category.label) || category.subcategories.empty() ||
        category.subcategories.size() > kMaximumSubcategories) {
      return ContractError("发布目录主分类不满足 ID、文本或数量契约");
    }
    std::set<std::uint32_t> sub_ids;
    for (const auto& subcategory : category.subcategories) {
      if (subcategory.category_id == 0U ||
          !sub_ids.insert(subcategory.category_id).second ||
          !ValidText(subcategory.label) ||
          (subcategory.match_mode != 0U && subcategory.match_mode != 2U)) {
        return ContractError("发布目录子分类不满足 ID、文本或模式契约");
      }
      valid_items.insert(
          (static_cast<std::uint64_t>(category.category_id) << 32U) |
          subcategory.category_id);
      if (subcategory.match_mode == 2U) {
        wildcard_categories.insert(category.category_id);
      }
    }
  }

  std::set<std::uint64_t> stock_ids;
  if (auto error = ValidateCards(catalog.standard_cards, valid_items,
                                 wildcard_categories, stock_ids)) {
    return *error;
  }
  if (auto error = ValidateCards(catalog.cash_cards, valid_items,
                                 wildcard_categories, stock_ids)) {
    return *error;
  }
  return catalog;
}

}  // namespace bf::application
