#include "persistence/mysql/shop_catalog_repository.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

application::ServiceError InvalidCatalog(std::string message) {
  return MakeError(application::ErrorCode::kInternal,
                   "已发布商城目录损坏：" + std::move(message));
}

std::optional<std::uint32_t> CheckedU32(std::int64_t value) {
  if (value <= 0 || static_cast<std::uint64_t>(value) >
                        std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> CheckedNonnegativeU32(std::int64_t value) {
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

struct CatalogCardBuilder final {
  std::uint64_t item_id{0U};
  application::ShopCardView card;
};

application::ServiceResult<std::vector<application::ShopCardView>> ReadCards(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::int64_t configuration_id, std::string_view currency) {
  const auto rows = transaction.Exec(
      "SELECT "
      "template.`ItemId`,template.`ItemName`,(JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.published_main_category'))),(JSON_UNQUOTE(JSON_EXTRACT(template.`ClientFields`,'$.published_subcategory'))),offer.`StockId`,offer.`UnitPrice`,offer.`DurationSeconds`"
      " FROM `ItemTemplates` AS template JOIN `ShopOffers` AS offer ON "
      "offer.`ItemId`=template.`ItemId` WHERE template.`ConfigurationVersionId`=$1 AND "
      "offer.`ConfigurationVersionId`=$1 AND offer.`CurrencyCode`=$2 AND offer.`IsEnabled` AND "
      "template.`IsGrantable` ORDER BY template.`ItemId`,offer.`StockId`",
      bf::infrastructure::mysql::Parameters{transaction, configuration_id,
                                            currency});

  std::vector<application::ShopCardView> cards;
  CatalogCardBuilder current;
  for (const auto& row : rows) {
    const auto item_id = row[0].As<std::uint64_t>();
    if (current.item_id != item_id) {
      if (current.item_id != 0U) {
        cards.push_back(std::move(current.card));
      }
      const auto main_category = CheckedU32(row[2].As<std::int64_t>());
      const auto subcategory = CheckedU32(row[3].As<std::int64_t>());
      if (!main_category.has_value() || !subcategory.has_value()) {
        return InvalidCatalog("商品分类超出协议范围");
      }
      current = {.item_id = item_id,
                 .card = {.item_id = domain::ItemId{item_id},
                          .item_name = row[1].As<std::string>(),
                          .main_category = *main_category,
                          .subcategory = *subcategory,
                          .prices = {}}};
    }
    const auto price = CheckedU32(row[5].As<std::int64_t>());
    if (!price.has_value()) {
      return InvalidCatalog("商品价格超出协议范围");
    }
    application::ShopPriceView price_view{
        .stock_id = domain::StockId{row[4].As<std::uint64_t>()},
        .item_id = domain::ItemId{item_id},
        .duration_type = application::ClientStockDurationType::kPermanent,
        .duration_value = std::numeric_limits<std::uint32_t>::max(),
        .price = *price};
    if (!row[6].IsNull()) {
      const auto seconds = row[6].As<std::int64_t>();
      if (seconds <= 0 || seconds % 86'400 != 0) {
        return InvalidCatalog("限时商品期限不是正整数天");
      }
      const auto days = CheckedU32(seconds / 86'400);
      if (!days.has_value() ||
          *days == std::numeric_limits<std::uint32_t>::max()) {
        return InvalidCatalog("限时商品天数超出协议范围");
      }
      price_view.duration_type =
          application::ClientStockDurationType::kTimedDays;
      price_view.duration_value = *days;
    }
    current.card.prices.push_back(price_view);
  }
  if (current.item_id != 0U) {
    cards.push_back(std::move(current.card));
  }
  const auto bundles = transaction.Exec(
      "SELECT "
      "offer.`StockId`,bundle.`DisplayName`,offer.`UnitPrice`,bundle.`DurationType`,bundle.`DurationValue`"
      " FROM `ShopBundles` bundle JOIN `ShopOffers` offer USING(`StockId`) JOIN "
      "`ConfigurationVersions` version ON "
      "version.`ConfigurationVersionId`=offer.`ConfigurationVersionId` WHERE "
      "version.`ConfigurationKind`='shop_bundles' AND version.`ReleaseStatus`='published' AND "
      "offer.`IsEnabled` AND offer.`CurrencyCode`=$1 ORDER BY offer.`StockId`",
      bf::infrastructure::mysql::Parameters{transaction, currency});
  for (const auto& row : bundles) {
    const domain::ItemId id{row[0].As<std::uint64_t>()};
    cards.push_back(
        {.item_id = id,
         .item_name = row[1].As<std::string>(),
         .main_category = 100U,
         .subcategory = 107U,
         .prices = {{.stock_id = domain::StockId{id.Value()},
                     .item_id = id,
                     .duration_type =
                         static_cast<application::ClientStockDurationType>(
                             row[3].As<unsigned>()),
                     .duration_value = row[4].As<std::uint32_t>(),
                     .price = row[2].As<std::uint32_t>()}},
         .is_bundle = true});
  }
  return cards;
}

}  // namespace

MySqlShopCatalogReader::MySqlShopCatalogReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::optional<application::PublishedShopCatalog>>
MySqlShopCatalogReader::LoadPublished() {
  std::scoped_lock cache_lock{cache_mutex_};
  if (cache_.has_value()) {
    return cache_;
  }
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto versions = transaction.Exec(
        "SELECT "
        "`ConfigurationVersionId`,(JSON_UNQUOTE(JSON_EXTRACT(`Payload`,'$.catalog_revision'))) FROM"
        " `ConfigurationVersions` WHERE `ConfigurationKind`='catalog' AND "
        "`ReleaseStatus`='published' ORDER BY `ConfigurationVersionId`");
    if (versions.empty()) {
      transaction.Commit();
      return std::optional<application::PublishedShopCatalog>{};
    }
    if (versions.size() != 1U) {
      return InvalidCatalog("同时存在多个 published 版本");
    }
    const auto configuration_id = versions[0][0].As<std::int64_t>();
    const auto revision_signed = versions[0][1].As<std::int64_t>();
    if (revision_signed <= 0) {
      return InvalidCatalog("目录 revision 不是正整数");
    }
    // A bundle publication also changes S2C800. Include its digest in the
    // client cache revision.
    const auto bundle_versions = transaction.Exec(
        "SELECT CAST(CONV(LEFT(HEX(`ContentDigest`),15),16,10) AS UNSIGNED) FROM "
        "`ConfigurationVersions` WHERE `ConfigurationKind`='shop_bundles' AND "
        "`ReleaseStatus`='published'");
    if (bundle_versions.size() > 1U) {
      return InvalidCatalog("同时存在多个套装 published 版本");
    }
    auto revision = static_cast<std::uint64_t>(revision_signed);
    if (!bundle_versions.empty()) {
      revision ^= bundle_versions[0][0].As<std::uint64_t>();
      if (revision == 0U) {
        revision = 1U;
      }
    }

    const auto category_rows = transaction.Exec(
        "SELECT c.CategoryId,c.Label,s.SubcategoryId,s.Label,s.MatchMode FROM ConfigurationVersions"
        " v CROSS JOIN JSON_TABLE(v.Payload,'$.categories[*]' COLUMNS (CategoryOrder FOR "
        "ORDINALITY,CategoryId BIGINT PATH '$.category_id',Label VARCHAR(255) PATH "
        "'$.label',Subcategories JSON PATH '$.subcategories')) c CROSS JOIN "
        "JSON_TABLE(c.Subcategories,'$[*]' COLUMNS (SubcategoryOrder FOR ORDINALITY,SubcategoryId "
        "BIGINT PATH '$.category_id',Label VARCHAR(255) PATH '$.label',MatchMode BIGINT PATH "
        "'$.match_mode')) s WHERE v.ConfigurationVersionId=$1 ORDER BY "
        "c.CategoryOrder,s.SubcategoryOrder",
        bf::infrastructure::mysql::Parameters{transaction, configuration_id});
    std::vector<application::ShopCategoryView> categories;
    std::uint32_t current_category = 0U;
    for (const auto& row : category_rows) {
      const auto category_id = CheckedU32(row[0].As<std::int64_t>());
      const auto subcategory_id = CheckedU32(row[2].As<std::int64_t>());
      const auto match_mode = CheckedNonnegativeU32(row[4].As<std::int64_t>());
      if (!category_id.has_value() || !subcategory_id.has_value() ||
          !match_mode.has_value()) {
        return InvalidCatalog("分类字段超出协议范围");
      }
      if (current_category != *category_id) {
        categories.push_back({.category_id = *category_id,
                              .label = row[1].As<std::string>(),
                              .subcategories = {}});
        current_category = *category_id;
      } else if (categories.empty() ||
                 categories.back().label != row[1].As<std::string>()) {
        return InvalidCatalog("同一主分类出现冲突标签或非连续顺序");
      }
      categories.back().subcategories.push_back(
          {.category_id = *subcategory_id,
           .label = row[3].As<std::string>(),
           .match_mode = *match_mode});
    }

    auto standard_result = ReadCards(transaction, configuration_id, "lucci");
    if (const auto* error =
            std::get_if<application::ServiceError>(&standard_result)) {
      return *error;
    }
    auto cash_result = ReadCards(transaction, configuration_id, "bubble_cash");
    if (const auto* error =
            std::get_if<application::ServiceError>(&cash_result)) {
      return *error;
    }
    application::PublishedShopCatalog catalog{
        .revision = revision,
        .standard_cards = std::get<std::vector<application::ShopCardView>>(
            std::move(standard_result)),
        .cash_cards = std::get<std::vector<application::ShopCardView>>(
            std::move(cash_result)),
        .categories = std::move(categories)};
    transaction.Commit();
    cache_ = std::move(catalog);
    return cache_;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取商城目录时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取商城目录 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 拒绝商城目录查询");
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取商城目录时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
