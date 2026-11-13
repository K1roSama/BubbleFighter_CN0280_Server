#include "protocol/shop/shop_catalog_messages.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

std::uint32_t CheckedU32(std::uint64_t value, const char* field) {
  if (value == 0U || value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, field};
  }
  return static_cast<std::uint32_t>(value);
}

std::uint32_t CheckedCount(std::size_t value, const char* field) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, field};
  }
  return static_cast<std::uint32_t>(value);
}

void WritePrice(PacketWriter& writer, const application::ShopPriceView& price) {
  writer.WriteU32(
      CheckedU32(price.stock_id.Value(), "catalog stock ID out of range"));
  writer.WriteU32(
      CheckedU32(price.item_id.Value(), "catalog item ID out of range"));
  writer.WriteU32(1U);
  writer.WriteU16(static_cast<std::uint16_t>(price.duration_type));
  writer.WriteU32(price.duration_value);
  writer.WriteU32(price.price);
  writer.WriteU32(price.price);
}

void WriteCard(PacketWriter& writer, const application::ShopCardView& card) {
  if (card.prices.empty()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "catalog card does not have a primary price"};
  }
  writer.WriteU32(
      CheckedU32(card.item_id.Value(), "catalog card item ID out of range"));
  writer.WriteU32(card.is_bundle ? 1U : 0U);
  writer.WriteUtf16(Utf8ToUtf16(card.item_name));
  writer.WriteU32(0U);
  writer.WriteU32(card.main_category);
  writer.WriteU32(card.subcategory);
  writer.WriteU32(1U);
  writer.WriteU64(0U);
  writer.WriteU64(0U);
  WritePrice(writer, card.prices.front());
  writer.WriteU32(
      CheckedCount(card.prices.size(), "catalog price count out of range"));
  for (const auto& price : card.prices) {
    WritePrice(writer, price);
  }
}

void WriteCards(PacketWriter& writer,
                const std::vector<application::ShopCardView>& cards) {
  writer.WriteU32(
      CheckedCount(cards.size(), "catalog card count out of range"));
  for (const auto& card : cards) {
    WriteCard(writer, card);
  }
}

void WriteCommon(PacketWriter& writer, std::uint64_t revision,
                 const std::vector<application::ShopCardView>& cards) {
  if (revision == 0U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "catalog revision cannot be zero"};
  }
  writer.WriteU32(1U);
  writer.WriteU32(1U);
  writer.WriteU64(revision);
  WriteCards(writer, cards);
}

std::uint32_t SelectedCategory(const application::PublishedShopCatalog& catalog,
                               std::uint32_t requested) {
  const auto contains = [&](std::uint32_t value) {
    return std::ranges::any_of(catalog.categories, [&](const auto& category) {
      return category.category_id == value;
    });
  };
  if (contains(requested)) {
    return requested;
  }
  if (contains(300U)) {
    return 300U;
  }
  if (catalog.categories.empty()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "catalog categories cannot be empty"};
  }
  return catalog.categories.front().category_id;
}

}  // namespace

std::vector<std::uint8_t> EncodeShopCatalogResponse(
    const application::PublishedShopCatalog& catalog,
    std::uint32_t requested_selection) {
  PacketWriter writer;
  // Build 752 的 S2C800 是全部普通商品；币种由客户端 ItemInfo+60 决定。
  // S2C801 写入独立的 1200 目录，并不会合并到普通商城（B6B360/D06090）。
  auto cards = catalog.standard_cards;
  cards.insert(cards.end(), catalog.cash_cards.begin(),
               catalog.cash_cards.end());
  std::ranges::sort(cards, {},
                    [](const auto& card) { return card.item_id.Value(); });
  WriteCommon(writer, catalog.revision, cards);
  writer.WriteU64(catalog.revision);
  writer.WriteU32(CheckedCount(catalog.categories.size(),
                               "catalog category count out of range"));
  for (const auto& category : catalog.categories) {
    writer.WriteU32(category.category_id);
    writer.WriteUtf16(Utf8ToUtf16(category.label));
    writer.WriteU32(category.category_id);
    auto subcategories = category.subcategories;
    if (category.category_id == 100U &&
        std::ranges::none_of(subcategories, [](const auto& child) {
          return child.category_id == 107U;
        })) {
      // 原版服装主类默认选中 107；D007C0 找不到当前按钮就忽略所有子类点击。
      // 即便没有在售套装，也必须保留这个原生分类，不能按当前商品反推删除它。
      subcategories.push_back(
          {.category_id = 107U, .label = "套装", .match_mode = 0U});
      std::ranges::sort(subcategories, {},
                        &application::ShopSubcategoryView::category_id);
    }
    writer.WriteU32(CheckedCount(subcategories.size(),
                                 "catalog subcategory count out of range"));
    for (const auto& subcategory : subcategories) {
      writer.WriteU32(subcategory.category_id);
      writer.WriteUtf16(Utf8ToUtf16(subcategory.label));
      writer.WriteU32(subcategory.category_id);
      writer.WriteU32(subcategory.match_mode);
    }
  }
  writer.WriteU32(0U);
  writer.WriteU32(SelectedCategory(catalog, requested_selection));
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCashShopCatalogResponse(
    const application::PublishedShopCatalog& catalog) {
  PacketWriter writer;
  WriteCommon(writer, catalog.revision, catalog.cash_cards);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeShopCard(
    const application::ShopCardView& card) {
  PacketWriter writer;
  WriteCard(writer, card);
  return writer.Release();
}

}  // namespace bf::protocol
