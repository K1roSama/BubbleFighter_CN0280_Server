#include "persistence/mysql/mystery_shop_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "features/shop/purchase.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

using Json = nlohmann::json;

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

application::ServiceError MakeSqlError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

application::ServiceError Corrupt(std::string message) {
  return MakeError(application::ErrorCode::kInternal,
                   "神秘商店持久化快照损坏：" + std::move(message));
}

std::int64_t UnixSeconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

Json OfferJson(const application::MysteryOfferRule& offer) {
  return {{"stock_id", offer.stock_id.Value()},
          {"item_id", offer.item_id.Value()},
          {"item_name", offer.item_name},
          {"price", offer.price},
          {"duration_seconds", offer.duration.count()}};
}

Json OffersJson(const std::vector<application::MysteryOfferRule>& offers) {
  Json output = Json::array();
  for (const auto& offer : offers) {
    output.push_back(OfferJson(offer));
  }
  return output;
}

application::ServiceResult<std::vector<application::MysteryOfferRule>>
ParseOffers(std::string_view text) {
  try {
    const auto document = Json::parse(text, nullptr, false, true);
    if (!document.is_array() || document.empty() || document.size() > 32U) {
      return Corrupt("选品不是一至三十二项数组");
    }
    std::vector<application::MysteryOfferRule> offers;
    offers.reserve(document.size());
    std::set<std::uint64_t> stocks;
    std::set<std::uint64_t> items;
    for (const auto& value : document) {
      if (!value.is_object() || !value.at("stock_id").is_number_unsigned() ||
          !value.at("item_id").is_number_unsigned() ||
          !value.at("item_name").is_string() ||
          !value.at("price").is_number_unsigned() ||
          !value.at("duration_seconds").is_number_unsigned()) {
        return Corrupt("选品字段类型无效");
      }
      const auto stock = value.at("stock_id").get<std::uint64_t>();
      const auto item = value.at("item_id").get<std::uint64_t>();
      const auto price = value.at("price").get<std::uint64_t>();
      const auto duration = value.at("duration_seconds").get<std::uint64_t>();
      if (stock == 0U || item == 0U || price == 0U ||
          price > std::numeric_limits<std::uint32_t>::max() || duration == 0U ||
          duration >
              static_cast<std::uint64_t>(std::chrono::seconds::max().count()) ||
          !stocks.insert(stock).second || !items.insert(item).second) {
        return Corrupt("选品字段范围或唯一性无效");
      }
      offers.push_back({.stock_id = domain::StockId{stock},
                        .item_id = domain::ItemId{item},
                        .item_name = value.at("item_name").get<std::string>(),
                        .price = static_cast<std::uint32_t>(price),
                        .duration = std::chrono::seconds{
                            static_cast<std::chrono::seconds::rep>(duration)}});
    }
    return offers;
  } catch (const Json::exception&) {
    return Corrupt("选品 JSON 缺少字段或无法解析");
  }
}

application::ServiceResult<std::uint32_t> PurchasedMask(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, std::uint64_t generation,
    const std::vector<application::MysteryOfferRule>& offers) {
  const auto rows = transaction.Exec(
      "SELECT `IdempotencyKey` FROM `OperationRequests` WHERE `PlayerId`=$1 AND "
      "`OperationType`='mystery_purchase' AND `OperationStatus`='succeeded' AND `IdempotencyKey` "
      "LIKE $2",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            std::to_string(generation) + ":%"});
  std::set<std::string> keys;
  for (const auto& row : rows) {
    keys.insert(row[0].As<std::string>());
  }
  std::uint32_t mask = 0U;
  for (std::size_t index = 0U; index < offers.size(); ++index) {
    const auto key = std::to_string(generation) + ":" +
                     std::to_string(offers[index].stock_id.Value());
    if (keys.contains(key)) {
      mask |= (1U << index);
    }
  }
  return mask;
}

application::ServiceResult<application::MysterySelectionSnapshot>
ParseSelection(bf::infrastructure::mysql::TransactionBase& transaction,
               const bf::infrastructure::mysql::Row& row,
               domain::PlayerId player_id) {
  try {
    auto parsed = ParseOffers(row[1].As<std::string_view>());
    if (const auto* error = std::get_if<application::ServiceError>(&parsed)) {
      return *error;
    }
    auto offers =
        std::get<std::vector<application::MysteryOfferRule>>(std::move(parsed));
    const auto generation = row[0].As<std::uint64_t>();
    auto mask = PurchasedMask(transaction, player_id, generation, offers);
    if (const auto* error = std::get_if<application::ServiceError>(&mask)) {
      return *error;
    }
    return application::MysterySelectionSnapshot{
        .player_id = player_id,
        .generation = generation,
        .expires_at =
            std::chrono::system_clock::time_point{
                std::chrono::seconds{row[2].As<std::int64_t>()}},
        .offers = std::move(offers),
        .purchased_mask = std::get<std::uint32_t>(mask),
        .rules_version_id = row[3].As<std::uint64_t>()};
  } catch (const bf::infrastructure::mysql::ConversionError&) {
    return Corrupt("选品关系字段无法转换");
  }
}

application::ServiceResult<std::vector<domain::ItemId>> PermanentItems(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id) {
  const auto rows = transaction.Exec(
      "SELECT `ItemId` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ExpiresAt` IS NULL ORDER BY "
      "`ItemId`",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  std::vector<domain::ItemId> result;
  result.reserve(rows.size());
  for (const auto& row : rows) {
    result.emplace_back(row[0].As<std::uint64_t>());
  }
  return result;
}

application::ServiceResult<application::MysterySelectionSnapshot>
CreateSelection(bf::infrastructure::mysql::TransactionBase& transaction,
                domain::PlayerId player_id, std::uint64_t generation,
                std::uint64_t rules_version_id,
                const application::MysteryShopServiceRules& rules,
                std::int64_t now) {
  auto owned_result = PermanentItems(transaction, player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&owned_result)) {
    return *error;
  }
  auto selected_result = application::SelectMysteryOffers(
      player_id, generation,
      std::get<std::vector<domain::ItemId>>(owned_result), rules);
  if (const auto* error =
          std::get_if<application::ServiceError>(&selected_result)) {
    return *error;
  }
  auto selected = std::get<std::vector<application::MysteryOfferRule>>(
      std::move(selected_result));
  const auto expires = now + rules.selection_ttl.count();
  transaction.Exec(
      "INSERT INTO "
      "`MysteryShopSelections`(`PlayerId`,`Generation`,`OfferSnapshot`,`ExpiresAt`,`ConfigurationVersionId`,`CreatedAt`)"
      " VALUES($1,$2,$3,FROM_UNIXTIME($4),$5,FROM_UNIXTIME($6)) ON CONFLICT(`PlayerId`) DO UPDATE"
      " SET "
      "`Generation`=VALUES(`Generation`),`OfferSnapshot`=VALUES(`OfferSnapshot`),`ExpiresAt`=VALUES(`ExpiresAt`),`ConfigurationVersionId`=VALUES(`ConfigurationVersionId`),`CreatedAt`=VALUES(`CreatedAt`)",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), generation,
          OffersJson(selected).dump(), expires, rules_version_id, now});
  return application::MysterySelectionSnapshot{
      .player_id = player_id,
      .generation = generation,
      .expires_at =
          std::chrono::system_clock::time_point{std::chrono::seconds{expires}},
      .offers = std::move(selected),
      .purchased_mask = 0U,
      .rules_version_id = rules_version_id};
}

application::ServiceResult<application::MysterySelectionSnapshot>
EnsureSelection(bf::infrastructure::mysql::TransactionBase& transaction,
                domain::PlayerId player_id, std::uint64_t rules_version_id,
                const application::MysteryShopServiceRules& rules,
                std::int64_t now) {
  const auto rows = transaction.Exec(
      "SELECT "
      "`Generation`,`OfferSnapshot`,floor(UNIX_TIMESTAMP(`ExpiresAt`)),`ConfigurationVersionId`,`ExpiresAt`>FROM_UNIXTIME($2)"
      " FROM `MysteryShopSelections` WHERE `PlayerId`=$1 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            now});
  if (!rows.empty() && rows[0][4].As<bool>()) {
    return ParseSelection(transaction, rows[0], player_id);
  }
  const auto generation =
      rows.empty() ? 1U : rows[0][0].As<std::uint64_t>() + 1U;
  return CreateSelection(transaction, player_id, generation, rules_version_id,
                         rules, now);
}

application::ServiceResult<domain::PurchaseInventoryEffect> InventoryEffect(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, const application::MysteryOfferRule& selected,
    std::int64_t now,
    std::optional<domain::ItemInstanceId>& existing_instance) {
  const auto templates = transaction.Exec(
      "SELECT `StackPolicy`,`DurationPolicy`,`MaxQuantity`,`MaxDurationSeconds` FROM "
      "`ItemTemplates` template JOIN `ConfigurationVersions` configuration ON "
      "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE `ItemId`=$1"
      " AND `IsGrantable` AND configuration.`ReleaseStatus`='published'",
      bf::infrastructure::mysql::Parameters{transaction,
                                            selected.item_id.Value()});
  if (templates.size() != 1U) {
    return MakeError(application::ErrorCode::kConflict,
                     "神秘商店物品当前不可发放");
  }
  const auto stack_text = templates[0][0].As<std::string_view>();
  const auto duration_text = templates[0][1].As<std::string_view>();
  const auto stack =
      stack_text == "merge_quantity"
          ? std::optional{domain::ItemStackPolicy::kMergeQuantity}
      : stack_text == "single_instance"
          ? std::optional{domain::ItemStackPolicy::kSingleInstance}
          : std::nullopt;
  const auto duration_policy =
      duration_text == "permanent"
          ? std::optional{domain::ItemDurationPolicy::kPermanent}
      : duration_text == "expiring"
          ? std::optional{domain::ItemDurationPolicy::kExpiring}
      : duration_text == "offer_defined"
          ? std::optional{domain::ItemDurationPolicy::kOfferDefined}
          : std::nullopt;
  if (!stack.has_value() || !duration_policy.has_value()) {
    return Corrupt("物品模板包含未知库存策略");
  }
  const domain::PurchaseOffer offer{
      .stock_id = selected.stock_id,
      .item_id = selected.item_id,
      .currency = domain::CurrencyCode::kLucci,
      .unit_price = selected.price,
      .grant_quantity = 1,
      .duration_seconds = selected.duration.count(),
      .stack_policy = *stack,
      .duration_policy = *duration_policy,
      .max_quantity = templates[0][2].As<std::int64_t>(),
      .max_duration_seconds =
          templates[0][3].IsNull()
              ? std::nullopt
              : std::optional{templates[0][3].As<std::int64_t>()}};
  const auto items = transaction.Exec(
      "SELECT `ItemInstanceId`,`ItemId`,`Quantity`,CASE WHEN `ExpiresAt` IS NULL THEN NULL ELSE "
      "ROUND(UNIX_TIMESTAMP(`ExpiresAt`)) END FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 FOR"
      " UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            selected.item_id.Value()});
  std::optional<domain::OwnedItemSnapshot> owned;
  if (!items.empty()) {
    existing_instance = domain::ItemInstanceId{items[0][0].As<std::uint64_t>()};
    owned = domain::OwnedItemSnapshot{
        .item_instance_id = *existing_instance,
        .item_id = domain::ItemId{items[0][1].As<std::uint64_t>()},
        .quantity = items[0][2].As<std::int64_t>(),
        .expires_at_unix_seconds =
            items[0][3].IsNull()
                ? std::nullopt
                : std::optional{items[0][3].As<std::int64_t>()}};
  }
  auto calculated = domain::CalculatePurchaseInventoryEffect(offer, owned, now);
  if (const auto* error = std::get_if<domain::PurchaseRuleError>(&calculated)) {
    const auto message =
        *error == domain::PurchaseRuleError::kAlreadyOwnedPermanent
            ? "该永久物品已经拥有"
            : "神秘商店物品数量或期限已达到上限";
    return MakeError(application::ErrorCode::kConflict, message);
  }
  return std::get<domain::PurchaseInventoryEffect>(calculated);
}

}  // namespace

MySqlMysteryShopRepository::MySqlMysteryShopRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::MysterySelectionSnapshot>
MySqlMysteryShopRepository::LoadOrCreate(
    domain::PlayerId player_id, std::uint64_t rules_version_id,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point observed_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "神秘商店玩家不存在");
    }
    auto result = EnsureSelection(transaction, player_id, rules_version_id,
                                  rules, UnixSeconds(observed_at));
    if (std::holds_alternative<application::ServiceError>(result)) {
      return result;
    }
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取神秘商店选品时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("读取神秘商店选品", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取神秘商店选品时发生未分类错误");
  }
}

application::ServiceResult<application::MysterySelectionSnapshot>
MySqlMysteryShopRepository::Refresh(
    domain::OperationId operation_id, domain::PlayerId player_id,
    std::uint64_t expected_generation,
    const application::MysteryShopServiceRules& rules,
    std::uint64_t rules_version_id,
    std::chrono::system_clock::time_point observed_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now = UnixSeconds(observed_at);
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "神秘商店玩家不存在");
    }
    auto current_result =
        EnsureSelection(transaction, player_id, rules_version_id, rules, now);
    if (const auto* error =
            std::get_if<application::ServiceError>(&current_result)) {
      return *error;
    }
    const auto& current =
        std::get<application::MysterySelectionSnapshot>(current_result);
    if (current.generation != expected_generation) {
      return MakeError(application::ErrorCode::kConflict,
                       "商品列表已刷新，请重新打开神秘泡泡");
    }
    const auto operation = operation_id.ToUuidString();
    const auto canonical =
        "mystery_refresh:" + std::to_string(player_id.Value()) + ":" +
        std::to_string(expected_generation);
    transaction.Exec(
        "INSERT INTO "
        "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
        " VALUES($1,$2,'mystery_refresh',$1,UNHEX(SHA2($3,256)))",
        bf::infrastructure::mysql::Parameters{transaction, operation,
                                              player_id.Value(), canonical});
    const auto item = transaction.Exec(
        "SELECT `ItemInstanceId`,`Quantity`,CASE WHEN `ExpiresAt` IS NULL THEN NULL ELSE "
        "ROUND(UNIX_TIMESTAMP(`ExpiresAt`)) END FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 AND"
        " (`ExpiresAt` IS NULL OR `ExpiresAt`>FROM_UNIXTIME($3)) FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              rules.refresh_item_id.Value(),
                                              now});
    if (item.empty() ||
        item[0][1].As<std::int64_t>() < rules.refresh_quantity) {
      return MakeError(application::ErrorCode::kConflict,
                       "神秘商店刷新券不足或已过期");
    }
    const auto instance = item[0][0].As<std::uint64_t>();
    const auto before = item[0][1].As<std::int64_t>();
    const auto after = before - rules.refresh_quantity;
    const auto expires = item[0][2].IsNull()
                             ? std::nullopt
                             : std::optional{item[0][2].As<std::int64_t>()};
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $5"
        " IS NULL OR $6=0 THEN NULL ELSE FROM_UNIXTIME($5) "
        "END,'mystery_refresh_consume',$7,FROM_UNIXTIME($8))",
        bf::infrastructure::mysql::Parameters{
            transaction, player_id.Value(), rules.refresh_item_id.Value(),
            instance, -static_cast<std::int64_t>(rules.refresh_quantity),
            expires, after, operation, now});
    if (after == 0) {
      transaction.Exec(
          "DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, instance});
    } else {
      transaction.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=$2 WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, instance, after});
    }
    auto next_result =
        CreateSelection(transaction, player_id, current.generation + 1U,
                        rules_version_id, rules, now);
    if (const auto* error =
            std::get_if<application::ServiceError>(&next_result)) {
      return *error;
    }
    transaction.Exec(
        "UPDATE `OperationRequests` SET "
        "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=FROM_UNIXTIME($3) WHERE "
        "`OperationRequestId`=$1",
        bf::infrastructure::mysql::Parameters{
            transaction, operation,
            "mystery_generation:" + std::to_string(current.generation + 1U),
            now});
    transaction.Commit();
    return next_result;
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(application::ErrorCode::kConflict, "神秘商店刷新操作重复");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "刷新神秘商店时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("刷新神秘商店", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "刷新神秘商店时发生未分类错误");
  }
}

application::ServiceResult<application::MysteryPurchaseResult>
MySqlMysteryShopRepository::Purchase(
    domain::OperationId operation_id, domain::PlayerId player_id,
    std::uint64_t expected_generation, domain::StockId stock_id,
    std::uint64_t rules_version_id,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point observed_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now = UnixSeconds(observed_at);
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "神秘商店玩家不存在");
    }
    auto current_result =
        EnsureSelection(transaction, player_id, rules_version_id, rules, now);
    if (const auto* error =
            std::get_if<application::ServiceError>(&current_result)) {
      return *error;
    }
    auto current = std::get<application::MysterySelectionSnapshot>(
        std::move(current_result));
    if (current.generation != expected_generation) {
      return MakeError(application::ErrorCode::kConflict,
                       "商品列表已刷新，请重新打开神秘泡泡");
    }
    const auto selected = std::ranges::find(
        current.offers, stock_id, &application::MysteryOfferRule::stock_id);
    if (selected == current.offers.end()) {
      return MakeError(application::ErrorCode::kConflict,
                       "商品已过期或不是当前列表商品");
    }
    const auto& selected_value = *selected;
    const auto key = std::to_string(expected_generation) + ":" +
                     std::to_string(stock_id.Value());
    const auto replay = transaction.Exec(
        "SELECT `OperationStatus` FROM `OperationRequests` WHERE `PlayerId`=$1 AND "
        "`OperationType`='mystery_purchase' AND `IdempotencyKey`=$2 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              key});
    const auto index =
        static_cast<std::size_t>(selected - current.offers.begin());
    if (!replay.empty()) {
      if (replay[0][0].As<std::string_view>() != "succeeded") {
        return MakeError(application::ErrorCode::kInvalidState,
                         "神秘商店购买仍未完成");
      }
      current.purchased_mask |= (1U << index);
      transaction.Commit();
      return application::MysteryPurchaseResult{
          .selection = std::move(current),
          .reward_item_id = selected_value.item_id,
          .inventory_changed = false};
    }
    const auto operation = operation_id.ToUuidString();
    const auto canonical =
        "mystery_purchase:" + std::to_string(player_id.Value()) + ":" + key;
    transaction.Exec(
        "INSERT INTO "
        "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
        " VALUES($1,$2,'mystery_purchase',$3,UNHEX(SHA2($4,256)))",
        bf::infrastructure::mysql::Parameters{
            transaction, operation, player_id.Value(), key, canonical});

    std::optional<domain::ItemInstanceId> existing_instance;
    auto effect_result = InventoryEffect(transaction, player_id, selected_value,
                                         now, existing_instance);
    if (const auto* error =
            std::get_if<application::ServiceError>(&effect_result)) {
      return *error;
    }
    const auto effect =
        std::get<domain::PurchaseInventoryEffect>(effect_result);
    const auto wallet = transaction.Exec(
        "UPDATE `PlayerWallets` SET `Balance`=`Balance`-$2,`UpdatedAt`=FROM_UNIXTIME($3) WHERE "
        "`PlayerId`=$1 AND `CurrencyCode`='lucci' AND `Balance` >= $2 RETURNING "
        "`Balance`+$2,`Balance`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              selected_value.price, now});
    if (wallet.empty()) {
      return MakeError(application::ErrorCode::kInsufficientBalance,
                       "Lucci 余额不足");
    }
    bf::infrastructure::mysql::QueryResult stored;
    if (existing_instance.has_value()) {
      stored = transaction.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=$2,`ExpiresAt`=CASE WHEN $3 IS NULL THEN NULL ELSE "
          "FROM_UNIXTIME($3) END,`SourceCode`='mystery_purchase' WHERE `ItemInstanceId`=$1 RETURNING "
          "`ItemInstanceId`",
          bf::infrastructure::mysql::Parameters{
              transaction, existing_instance->Value(), effect.quantity_after,
              effect.expires_after});
    } else {
      stored = transaction.Exec(
          "INSERT INTO "
          "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
          "VALUES($1,$2,$3,FROM_UNIXTIME($4),FROM_UNIXTIME($5),'mystery_purchase') RETURNING "
          "`ItemInstanceId`",
          bf::infrastructure::mysql::Parameters{
              transaction, player_id.Value(), selected_value.item_id.Value(),
              effect.quantity_after, now, effect.expires_after});
    }
    const auto instance = stored[0][0].As<std::uint64_t>();
    const auto order = transaction.Exec(
        "INSERT INTO "
        "`PurchaseOrders`(`PlayerId`,`OperationRequestId`,`OrderStatus`,`CurrencyCode`,`TotalAmount`,`CreatedAt`,`SettledAt`)"
        " VALUES($1,$2,'settled','lucci',$3,FROM_UNIXTIME($4),FROM_UNIXTIME($4)) RETURNING "
        "`OrderId`",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              operation, selected_value.price,
                                              now});
    const auto order_id = order[0][0].As<std::uint64_t>();
    transaction.Exec(
        "INSERT INTO "
        "`PurchaseOrderItems`(`OrderId`,`LineNumber`,`StockId`,`ItemId`,`Quantity`,`UnitPrice`,`DurationSeconds`,`OfferSnapshot`)"
        " VALUES($1,1,(SELECT `StockId` FROM `ShopOffers` WHERE `StockId`=$2),$3,1,$4,$5,$6)",
        bf::infrastructure::mysql::Parameters{
            transaction, order_id, selected_value.stock_id.Value(),
            selected_value.item_id.Value(), selected_value.price,
            selected_value.duration.count(),
            Json{{"source", "mystery_shop"},
                 {"generation", expected_generation},
                 {"offer", OfferJson(selected_value)},
                 {"item_instance_id", instance},
                 {"balance_after", wallet[0][1].As<std::int64_t>()},
                 {"quantity_after", effect.quantity_after},
                 {"expires_after", effect.expires_after}}
                .dump()});
    transaction.Exec(
        "INSERT INTO "
        "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,'lucci',$2,$3,$4,'mystery_purchase',$5,FROM_UNIXTIME($6))",
        bf::infrastructure::mysql::Parameters{
            transaction, player_id.Value(),
            -static_cast<std::int64_t>(selected_value.price),
            wallet[0][0].As<std::int64_t>(), wallet[0][1].As<std::int64_t>(),
            operation, now});
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $6"
        " IS NULL THEN NULL ELSE FROM_UNIXTIME($6) END,'mystery_purchase',$7,FROM_UNIXTIME($8))",
        bf::infrastructure::mysql::Parameters{
            transaction, player_id.Value(), selected_value.item_id.Value(),
            instance, effect.quantity_delta, effect.expires_before,
            effect.expires_after, operation, now});
    transaction.Exec(
        "UPDATE `OperationRequests` SET "
        "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=FROM_UNIXTIME($3) WHERE "
        "`OperationRequestId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, operation,
                                              std::to_string(order_id), now});
    current.purchased_mask |= (1U << index);
    transaction.Commit();
    return application::MysteryPurchaseResult{
        .selection = std::move(current),
        .reward_item_id = selected_value.item_id,
        .inventory_changed = true};
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(application::ErrorCode::kConflict,
                     "神秘商店购买发生并发重复");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "购买神秘商店商品时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("购买神秘商店商品", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "购买神秘商店商品时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
