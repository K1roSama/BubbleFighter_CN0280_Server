#include "persistence/mysql/service_rules_repository.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <variant>

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

application::ServiceError InvalidRules(std::string message) {
  return MakeError(application::ErrorCode::kInternal,
                   "已发布扩展业务规则损坏：" + std::move(message));
}

template <typename Integer>
std::optional<Integer> ReadInteger(const Json& value, std::string_view field,
                                   std::uint64_t maximum) {
  const auto found = value.find(field);
  if (found == value.end() || !found->is_number_unsigned()) {
    return std::nullopt;
  }
  const auto raw = found->get<std::uint64_t>();
  if (raw > maximum) {
    return std::nullopt;
  }
  return static_cast<Integer>(raw);
}

std::optional<std::uint32_t> PositiveU32(const Json& value,
                                         std::string_view field) {
  auto result = ReadInteger<std::uint32_t>(
      value, field, std::numeric_limits<std::uint32_t>::max());
  if (!result.has_value() || *result == 0U) {
    return std::nullopt;
  }
  return result;
}

application::ServiceResult<application::PublishedServiceRules> ParseRules(
    std::uint64_t configuration_id, std::string_view payload_text) {
  const auto document = Json::parse(payload_text, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    return InvalidRules("payload 不是严格 JSON 对象");
  }
  try {
    const auto schema = PositiveU32(document, "schema_version");
    if (!schema.has_value() || (*schema != 1U && *schema != 2U) ||
        document.at("publisher_version") !=
            (*schema == 1U ? "service-rules-publish-v1"
                           : "service-rules-publish-v2")) {
      return InvalidRules("扩展业务规则版本不受支持");
    }
    if (*schema == 2U) {
      const auto& source = document.at("source");
      if (source.at("workshop_native_items") !=
              "client/Packs/ConfigSh_00000.pkn::Config/ShopItem2.xml" ||
          source.at("workshop_native_items_sha256") !=
              "a4def217ffbea7d4dca5886e4e4f3f0a4db655d2a17bb52db0de5fcc5220e446" ||
          source.at("workshop_native_pack_sha256") !=
              "cb758d4bfedd90c2ac4bff6da4bf749bf3ddee4fa02de116d2df5d977bd1d50b" ||
          source.at("workshop_native_client_sha256") !=
              "a452332eed583206b8b568a0e3beeaa35ce324227ec839eb0fc2e701eb67c290") {
        return InvalidRules(
            "工坊132原ShopItem资格事实来源与当前已复核客户端不一致");
      }
    }
    const auto& policy = document.at("policy");
    const auto& bubble_json = policy.at("bubbles");
    const auto initial =
        ReadInteger<std::uint32_t>(bubble_json, "initial_balance",
                                   std::numeric_limits<std::uint32_t>::max());
    const auto selection = PositiveU32(bubble_json, "selection_seconds");
    const auto super_needle = PositiveU32(bubble_json, "super_needle_item_id");
    if (!initial.has_value() || !selection.has_value() ||
        !super_needle.has_value() || !bubble_json.at("needles").is_array() ||
        !bubble_json.at("reward_pool").is_array()) {
      return InvalidRules("泡泡字段类型或范围无效");
    }
    application::BubbleServiceRules bubbles{
        .initial_balance = *initial,
        .selection_duration = std::chrono::seconds{*selection},
        .needles = {},
        .super_needle_item_id = domain::ItemId{*super_needle},
        .reward_pool = {}};
    for (const auto& value : bubble_json.at("needles")) {
      const auto item_id = PositiveU32(value, "item_id");
      const auto cost = PositiveU32(value, "cost");
      if (!item_id.has_value() || !cost.has_value()) {
        return InvalidRules("魔法针字段类型或范围无效");
      }
      bubbles.needles.push_back(
          {.item_id = domain::ItemId{*item_id}, .cost = *cost});
    }
    for (const auto& value : bubble_json.at("reward_pool")) {
      const auto stock_id = PositiveU32(value, "stock_id");
      const auto item_id = PositiveU32(value, "item_id");
      const auto quantity = PositiveU32(value, "quantity");
      if (!stock_id.has_value() || !item_id.has_value() ||
          !quantity.has_value()) {
        return InvalidRules("泡泡奖励字段类型或范围无效");
      }
      std::optional<std::chrono::seconds> duration;
      const auto& duration_json = value.at("duration_seconds");
      if (!duration_json.is_null()) {
        if (!duration_json.is_number_unsigned() ||
            duration_json.get<std::uint64_t>() == 0U ||
            duration_json.get<std::uint64_t>() >
                static_cast<std::uint64_t>(
                    std::chrono::seconds::max().count())) {
          return InvalidRules("泡泡奖励期限类型或范围无效");
        }
        duration = std::chrono::seconds{
            duration_json.get<std::chrono::seconds::rep>()};
      }
      bubbles.reward_pool.push_back({.stock_id = domain::StockId{*stock_id},
                                     .item_id = domain::ItemId{*item_id},
                                     .quantity = *quantity,
                                     .duration = duration});
    }

    const auto& mystery_json = policy.at("mystery_shop");
    const auto refresh_item = PositiveU32(mystery_json, "refresh_item_id");
    const auto refresh_quantity = PositiveU32(mystery_json, "refresh_quantity");
    const auto selection_size = PositiveU32(mystery_json, "selection_size");
    const auto ttl = PositiveU32(mystery_json, "selection_ttl_seconds");
    if (!refresh_item.has_value() || !refresh_quantity.has_value() ||
        !selection_size.has_value() || !ttl.has_value() ||
        !mystery_json.at("offers").is_array()) {
      return InvalidRules("神秘商店字段类型或范围无效");
    }
    application::MysteryShopServiceRules mystery{
        .refresh_item_id = domain::ItemId{*refresh_item},
        .refresh_quantity = *refresh_quantity,
        .selection_size = *selection_size,
        .selection_ttl = std::chrono::seconds{*ttl},
        .offers = {}};
    for (const auto& value : mystery_json.at("offers")) {
      const auto stock_id = PositiveU32(value, "stock_id");
      const auto item_id = PositiveU32(value, "item_id");
      const auto price = PositiveU32(value, "price");
      const auto duration = PositiveU32(value, "duration_seconds");
      const auto name = value.find("item_name");
      if (!stock_id.has_value() || !item_id.has_value() || !price.has_value() ||
          !duration.has_value() || name == value.end() || !name->is_string()) {
        return InvalidRules("神秘商店候选字段类型或范围无效");
      }
      mystery.offers.push_back({.stock_id = domain::StockId{*stock_id},
                                .item_id = domain::ItemId{*item_id},
                                .item_name = name->get<std::string>(),
                                .price = *price,
                                .duration = std::chrono::seconds{*duration}});
    }

    const auto& workshop_json = policy.at("workshop");
    const auto compound_card =
        PositiveU32(workshop_json, "compound_card_item_id");
    const auto compound_min = PositiveU32(workshop_json, "compound_min");
    const auto compound_max = PositiveU32(workshop_json, "compound_max");
    const auto compound_duration =
        PositiveU32(workshop_json, "compound_duration_seconds");
    const auto decompose_card =
        PositiveU32(workshop_json, "decompose_card_item_id");
    const auto decompose_max = PositiveU32(workshop_json, "decompose_max");
    const auto decompose_reward =
        PositiveU32(workshop_json, "decompose_reward_item_id");
    const auto permanent_material =
        PositiveU32(workshop_json, "permanent_material_item_id");
    if (!compound_card.has_value() || !compound_min.has_value() ||
        !compound_max.has_value() || !compound_duration.has_value() ||
        !decompose_card.has_value() || !decompose_max.has_value() ||
        !decompose_reward.has_value() || !permanent_material.has_value() ||
        !workshop_json.at("item_flags").is_array()) {
      return InvalidRules("工坊字段类型或范围无效");
    }
    application::WorkshopServiceRules workshop{
        .compound_card_item_id = domain::ItemId{*compound_card},
        .compound_min = *compound_min,
        .compound_max = *compound_max,
        .compound_duration = std::chrono::seconds{*compound_duration},
        .decompose_card_item_id = domain::ItemId{*decompose_card},
        .decompose_max = *decompose_max,
        .decompose_reward_item_id = domain::ItemId{*decompose_reward},
        .permanent_material_item_id = domain::ItemId{*permanent_material},
        .item_rules = {}};
    if (*schema == 2U) {
      const auto& value = workshop_json.at("compound_policy");
      const auto version = PositiveU32(value, "version");
      const auto epoch = PositiveU32(value, "policy_epoch");
      const auto count = PositiveU32(value, "input_count");
      const auto protection = PositiveU32(value, "protection_material_item_id");
      const auto minimum_days = PositiveU32(value, "minimum_remaining_days");
      const auto maximum_days = PositiveU32(value, "maximum_remaining_days");
      const auto threshold = PositiveU32(value, "permanent_threshold_days");
      const auto minimum_percent = PositiveU32(value, "minimum_percent");
      const auto maximum_percent = PositiveU32(value, "maximum_percent");
      if (!version.has_value() || !epoch.has_value() || !count.has_value() ||
          !protection.has_value() || !minimum_days.has_value() ||
          !maximum_days.has_value() || !threshold.has_value() ||
          !minimum_percent.has_value() || !maximum_percent.has_value()) {
        return InvalidRules("工坊132新政策字段类型或范围无效");
      }
      workshop.compound_policy = application::WorkshopCompoundPolicy{
          .version = *version,
          .policy_epoch = *epoch,
          .input_count = *count,
          .protection_material_item_id = domain::ItemId{*protection},
          .minimum_remaining_days = *minimum_days,
          .maximum_remaining_days = *maximum_days,
          .permanent_threshold_days = *threshold,
          .minimum_percent = *minimum_percent,
          .maximum_percent = *maximum_percent,
          .probability_policy =
              value.at("probability_policy").get<std::string>(),
          .protected_merge_policy =
              value.at("protected_merge_policy").get<std::string>(),
          .catalog_sha256 = value.at("catalog_sha256").get<std::string>()};
    } else if (workshop_json.contains("compound_policy")) {
      return InvalidRules("旧版本不能混入工坊132新政策");
    }
    for (const auto& value : workshop_json.at("item_flags")) {
      const auto item_id = PositiveU32(value, "item_id");
      const auto compound_group = ReadInteger<std::uint32_t>(
          value, "compound_group", std::numeric_limits<std::uint32_t>::max());
      const auto reward_candidate = value.find("compound_reward_candidate");
      const auto compound = value.find("compound");
      const auto decompose = value.find("decompose");
      const auto permanent = value.find("permanent");
      if (!item_id.has_value() || !compound_group.has_value() ||
          reward_candidate == value.end() || !reward_candidate->is_boolean() ||
          compound == value.end() || !compound->is_boolean() ||
          decompose == value.end() || !decompose->is_boolean() ||
          permanent == value.end() || !permanent->is_boolean()) {
        return InvalidRules("工坊物品标志字段类型或范围无效");
      }
      application::WorkshopItemRule rule{
          .item_id = domain::ItemId{*item_id},
          .compound_group = *compound_group,
          .compound_reward_candidate = reward_candidate->get<bool>(),
          .compound = compound->get<bool>(),
          .decompose = decompose->get<bool>(),
          .permanent = permanent->get<bool>()};
      if (*schema == 2U) {
        const auto& raw = value.at("raw_client_fields");
        const auto gashapon = ReadInteger<std::uint32_t>(raw, "isGashapon", 1U);
        const auto native_compound =
            ReadInteger<std::uint32_t>(raw, "isCompound", 1U);
        if (!gashapon.has_value() || !native_compound.has_value() ||
            !raw.at("durationType").is_string()) {
          return InvalidRules("工坊原ShopItem资格事实缺失或无效");
        }
        rule.native_is_gashapon = *gashapon;
        rule.native_is_compound = *native_compound;
        rule.native_duration_type = raw.at("durationType").get<std::string>();
      }
      workshop.item_rules.push_back(std::move(rule));
    }
    return application::PublishedServiceRules{
        .configuration_version_id = configuration_id,
        .bubbles = std::move(bubbles),
        .mystery_shop = std::move(mystery),
        .workshop = std::move(workshop)};
  } catch (const Json::exception&) {
    return InvalidRules("必需 JSON 字段缺失或类型不符");
  }
}

}  // namespace

MySqlServiceRulesReader::MySqlServiceRulesReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::optional<application::PublishedServiceRules>>
MySqlServiceRulesReader::LoadPublished() {
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
    const auto rows = transaction.Exec(
        "SELECT `ConfigurationVersionId`,`Payload` FROM `ConfigurationVersions` WHERE "
        "`ConfigurationKind`='service_rules' AND `ReleaseStatus`='published' ORDER BY "
        "`ConfigurationVersionId`");
    if (rows.empty()) {
      transaction.Commit();
      return std::optional<application::PublishedServiceRules>{};
    }
    if (rows.size() != 1U) {
      return InvalidRules("同时存在多个 published 版本");
    }
    const auto configuration_id = rows[0][0].As<std::uint64_t>();
    auto parsed = ParseRules(configuration_id, rows[0][1].As<std::string>());
    if (const auto* error = std::get_if<application::ServiceError>(&parsed)) {
      return *error;
    }
    transaction.Commit();
    cache_ = std::get<application::PublishedServiceRules>(std::move(parsed));
    return cache_;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取扩展业务规则时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取扩展业务规则 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 拒绝扩展业务规则查询");
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取扩展业务规则时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
