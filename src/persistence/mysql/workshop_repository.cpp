#include "persistence/mysql/workshop_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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

application::ServiceError MakeSqlError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

std::int64_t UnixSeconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::seconds>(
             value.time_since_epoch())
      .count();
}

Json ItemArray(std::span<const domain::ItemId> items) {
  Json result = Json::array();
  for (const auto item : items) {
    result.push_back(item.Value());
  }
  return result;
}

struct StoredItem final {
  domain::ItemInstanceId instance_id;
  std::int64_t quantity{0};
  std::optional<std::int64_t> expires_at;
  bool active{false};
  bool equipped{false};
  std::optional<std::int64_t> expires_at_microseconds;
};

std::optional<StoredItem> LockItem(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, std::int64_t now) {
  const auto rows = transaction.Exec(
      "SELECT item.`ItemInstanceId`,item.`Quantity`,CASE WHEN item.`ExpiresAt` IS NULL THEN NULL "
      "ELSE ROUND(UNIX_TIMESTAMP(item.`ExpiresAt`)) END,(item.`ExpiresAt` IS NULL OR "
      "item.`ExpiresAt`>FROM_UNIXTIME($3)),EXISTS(SELECT 1 FROM `PlayerEquipment` equipment WHERE"
      " equipment.`PlayerId`=$1 AND equipment.`ItemInstanceId`=item.`ItemInstanceId`),CASE WHEN "
      "item.`ExpiresAt` IS NULL THEN NULL ELSE floor(UNIX_TIMESTAMP(item.`ExpiresAt`)*1000000) "
      "END FROM `PlayerItems` item WHERE item.`PlayerId`=$1 AND item.`ItemId`=$2 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            item_id.Value(), now});
  if (rows.empty()) {
    return std::nullopt;
  }
  return StoredItem{
      .instance_id = domain::ItemInstanceId{rows[0][0].As<std::uint64_t>()},
      .quantity = rows[0][1].As<std::int64_t>(),
      .expires_at = rows[0][2].IsNull()
                        ? std::nullopt
                        : std::optional{rows[0][2].As<std::int64_t>()},
      .active = rows[0][3].As<bool>(),
      .equipped = rows[0][4].As<bool>(),
      .expires_at_microseconds =
          rows[0][5].IsNull() ? std::nullopt
                              : std::optional{rows[0][5].As<std::int64_t>()}};
}

constexpr std::int64_t kMicrosecondsPerDay = 86'400'000'000LL;

// Exact floor(value*percent/100), avoiding multiplication of the large complete
// duration.
std::int64_t PercentOf(std::int64_t value, std::uint32_t percent) {
  return (value / 100LL) * percent + (value % 100LL) * percent / 100LL;
}

Json CompoundPolicySnapshot(const application::WorkshopCompoundPolicy& policy) {
  return Json{{"version", policy.version},
              {"policy_epoch", policy.policy_epoch},
              {"input_count", policy.input_count},
              {"protection_material_item_id",
               policy.protection_material_item_id.Value()},
              {"minimum_remaining_days", policy.minimum_remaining_days},
              {"maximum_remaining_days", policy.maximum_remaining_days},
              {"permanent_threshold_days", policy.permanent_threshold_days},
              {"minimum_percent", policy.minimum_percent},
              {"maximum_percent", policy.maximum_percent},
              {"probability_policy", policy.probability_policy},
              {"protected_merge_policy", policy.protected_merge_policy},
              {"catalog_sha256", policy.catalog_sha256}};
}

std::optional<application::ServiceError> LoadCompoundCatalog(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::WorkshopCompoundPolicy& policy, Json& catalog) {
  const auto rows = transaction.Exec(
      "SELECT `Payload` FROM `ConfigurationVersions` WHERE `ConfigurationKind`='catalog' AND "
      "`ReleaseStatus`='published' AND LOWER(HEX(`ContentDigest`))=$1 FOR SHARE",
      bf::infrastructure::mysql::Parameters{transaction,
                                            policy.catalog_sha256});
  if (rows.size() != 1U) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成装备目录发布版本不可用");
  }
  catalog = Json::parse(rows[0][0].As<std::string>());
  catalog.erase("payload_sha256");
  const auto digest =
      transaction.Exec("SELECT LOWER(SHA2($1,256))=$2",
                       bf::infrastructure::mysql::Parameters{
                           transaction, catalog.dump(), policy.catalog_sha256});
  if (digest.empty() || !digest[0][0].As<bool>()) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成装备目录发布快照摘要不一致");
  }
  return std::nullopt;
}

/** @brief
 * v2每件材料再次核对原raw资格与发布目录的真实行，绝不信任客户端或单个compound
 * bool。 */
std::optional<application::ServiceError> VerifyCompoundTemplate(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::ItemId item_id, const application::WorkshopCompoundPolicy& policy,
    const Json& rules, const Json& catalog,
    std::int64_t remaining_microseconds) {
  const auto& flags = rules.at("policy").at("workshop").at("item_flags");
  const auto found = std::ranges::find_if(flags, [item_id](const auto& value) {
    return value.at("item_id") == item_id.Value();
  });
  if (found == flags.end() || found->at("compound") != true) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "所选装备不支持合成");
  }
  const auto& raw = found->at("raw_client_fields");
  if (raw.at("isGashapon") != 1U || raw.at("isCompound") != 1U ||
      raw.at("durationType") != "day") {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "所选装备没有原版合成资格");
  }
  const auto rows = transaction.Exec(
      "SELECT "
      "t.`StackPolicy`,t.`DurationPolicy`,t.`MaxQuantity`,t.`MaxDurationSeconds`,t.`ClientFields`,t.`IsGrantable`,t.`ItemName`,t.`SlotCode`"
      " FROM `ItemTemplates` t JOIN `ConfigurationVersions` c USING(`ConfigurationVersionId`) "
      "WHERE t.`ItemId`=$1 AND c.`ConfigurationKind`='catalog' AND c.`ReleaseStatus`='published' "
      "AND LOWER(HEX(c.`ContentDigest`))=$2 FOR SHARE OF t,c",
      bf::infrastructure::mysql::Parameters{transaction, item_id.Value(),
                                            policy.catalog_sha256});
  if (rows.size() != 1U) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成装备的已发布目录版本不可用");
  }
  const auto& row = rows.front();
  const auto& templates = catalog.at("item_templates");
  const auto source =
      std::ranges::find_if(templates, [item_id](const auto& value) {
        return value.at("item_id") == item_id.Value();
      });
  const auto duration =
      row[3].IsNull() ? Json(nullptr) : Json(row[3].As<std::int64_t>());
  const auto slot =
      row[7].IsNull() ? Json(nullptr) : Json(row[7].As<std::string>());
  if (source == templates.end() ||
      row[0].As<std::string>() != "single_instance" ||
      row[1].As<std::string>() == "permanent" ||
      row[2].As<std::int64_t>() != 1 || row[3].IsNull() ||
      row[3].As<std::int64_t>() <= 0 || !row[5].As<bool>() ||
      source->at("stack_policy") != row[0].As<std::string>() ||
      source->at("duration_policy") != row[1].As<std::string>() ||
      source->at("max_quantity") != row[2].As<std::int64_t>() ||
      source->at("max_duration_seconds") != duration ||
      source->at("is_grantable") != row[5].As<bool>() ||
      source->at("item_name") != row[6].As<std::string>() ||
      source->at("slot_code") != slot ||
      source->at("client_fields") != Json::parse(row[4].As<std::string>())) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成装备目录行与不可变发布快照不一致");
  }
  const auto maximum = row[3].As<std::int64_t>();
  if (maximum > std::numeric_limits<std::int64_t>::max() / 1'000'000LL ||
      remaining_microseconds > maximum * 1'000'000LL) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成材料期限超出装备已发布上限");
  }
  return std::nullopt;
}

/** @brief
 * 保留保护槽的精确实例并按公开50..100%边界设置剩余期，流水冻结前后期限。 */
void RetainProtectedItem(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, const StoredItem& item,
    std::int64_t now_microseconds, std::int64_t retained_microseconds,
    std::string_view operation_id) {
  const auto retained_deadline = now_microseconds + retained_microseconds;
  // 100% protection retains the existing asset exactly. Its fact is frozen in
  // the workshop receipt; an inventory transaction must represent a real
  // change.
  if (*item.expires_at_microseconds == retained_deadline) {
    return;
  }
  transaction.Exec(
      "UPDATE `PlayerItems` SET "
      "`ExpiresAt`=FROM_UNIXTIME($2/1000000),`SourceCode`='workshop_protected' WHERE "
      "`ItemInstanceId`=$1",
      bf::infrastructure::mysql::Parameters{
          transaction, item.instance_id.Value(), retained_deadline});
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
      " "
      "VALUES($1,$2,$3,0,FROM_UNIXTIME($4/1000000),FROM_UNIXTIME($5/1000000),'workshop_protected',$6,FROM_UNIXTIME($7/1000000))",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), item_id.Value(),
          item.instance_id.Value(), *item.expires_at_microseconds,
          retained_deadline, operation_id, now_microseconds});
}

void ConsumeCompoundInput(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, const StoredItem& item,
    std::int64_t now_microseconds, std::string_view operation_id) {
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
      " "
      "VALUES($1,$2,$3,-1,FROM_UNIXTIME($4/1000000),NULL,'workshop_input_consume',$5,FROM_UNIXTIME($6/1000000))",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), item_id.Value(),
          item.instance_id.Value(), *item.expires_at_microseconds, operation_id,
          now_microseconds});
  transaction.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
                   bf::infrastructure::mysql::Parameters{
                       transaction, item.instance_id.Value()});
}

/** @brief 132从原注册物品产出的期限；显式永久来自新生成期限>720公开条件。 */
std::optional<application::ServiceError> GrantCompound(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id,
    std::int64_t now_microseconds, std::int64_t generated_microseconds,
    bool permanent, std::string_view operation_id) {
  const auto templates = transaction.Exec(
      "SELECT `MaxDurationSeconds` FROM `ItemTemplates` WHERE `ItemId`=$1 AND "
      "`StackPolicy`='single_instance' AND `MaxQuantity`=1 AND `IsGrantable` FOR SHARE",
      bf::infrastructure::mysql::Parameters{transaction, item_id.Value()});
  if (templates.size() != 1U || templates[0][0].IsNull()) {
    return MakeError(application::ErrorCode::kConflict,
                     "合成结果装备目录不可用");
  }
  auto old =
      LockItem(transaction, player_id, item_id, now_microseconds / 1'000'000LL);
  const auto before =
      old.has_value() ? old->expires_at_microseconds : std::nullopt;
  const auto base = before.has_value() ? std::max(now_microseconds, *before)
                                       : now_microseconds;
  std::optional<std::int64_t> after;
  if (!permanent) {
    const auto maximum = templates[0][0].As<std::int64_t>();
    if (maximum > std::numeric_limits<std::int64_t>::max() / 1'000'000LL ||
        maximum <= 0 ||
        maximum * 1'000'000LL >
            std::numeric_limits<std::int64_t>::max() - now_microseconds ||
        generated_microseconds <= 0 ||
        generated_microseconds > maximum * 1'000'000LL ||
        base - now_microseconds >
            maximum * 1'000'000LL - generated_microseconds) {
      return MakeError(application::ErrorCode::kConflict,
                       "合成结果期限超出装备已发布上限");
    }
    after = base + generated_microseconds;
  }
  const auto stored = transaction.Exec(
      "INSERT INTO "
      "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
      "VALUES($1,$2,1,FROM_UNIXTIME($3/1000000),CASE WHEN $4 IS NULL THEN NULL ELSE "
      "FROM_UNIXTIME($4/1000000) END,'workshop_reward') ON CONFLICT(`PlayerId`,`ItemId`) DO "
      "UPDATE SET `ExpiresAt`=VALUES(`ExpiresAt`),`SourceCode`='workshop_reward' RETURNING "
      "`ItemInstanceId`",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            item_id.Value(), now_microseconds,
                                            after});
  const auto instance = stored[0][0].As<std::uint64_t>();
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
      " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5/1000000) END,CASE"
      " WHEN $6 IS NULL THEN NULL ELSE FROM_UNIXTIME($6/1000000) "
      "END,'workshop_reward',$7,FROM_UNIXTIME($8/1000000))",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), item_id.Value(), instance,
          old.has_value() ? 0 : 1, before, after, operation_id,
          now_microseconds});
  return std::nullopt;
}

std::optional<application::ServiceError> Consume(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, std::uint32_t quantity,
    std::int64_t now, std::string_view operation_id, std::string_view reason,
    bool require_unequipped) {
  auto item = LockItem(transaction, player_id, item_id, now);
  if (!item.has_value() || !item->active || item->quantity < quantity) {
    return MakeError(application::ErrorCode::kConflict, "工坊材料不足或已过期");
  }
  if (require_unequipped && item->equipped) {
    return MakeError(application::ErrorCode::kConflict, "请先卸下要操作的装备");
  }
  const auto after = item->quantity - quantity;
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
      " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $5"
      " IS NULL OR $6=0 THEN NULL ELSE FROM_UNIXTIME($5) END,$7,$8,FROM_UNIXTIME($9))",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), item_id.Value(),
          item->instance_id.Value(), -static_cast<std::int64_t>(quantity),
          item->expires_at, after, reason, operation_id, now});
  if (after == 0) {
    transaction.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
                     bf::infrastructure::mysql::Parameters{
                         transaction, item->instance_id.Value()});
  } else {
    transaction.Exec(
        "UPDATE `PlayerItems` SET `Quantity`=$2 WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{
            transaction, item->instance_id.Value(), after});
  }
  return std::nullopt;
}

std::optional<application::ServiceError> Grant(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, std::uint32_t quantity,
    std::optional<std::chrono::seconds> duration, std::int64_t now,
    std::string_view operation_id) {
  const auto templates = transaction.Exec(
      "SELECT `StackPolicy`,`MaxQuantity`,`MaxDurationSeconds` FROM `ItemTemplates` template JOIN"
      " `ConfigurationVersions` configuration ON "
      "configuration.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE `ItemId`=$1"
      " AND `IsGrantable` AND configuration.`ReleaseStatus`='published'",
      bf::infrastructure::mysql::Parameters{transaction, item_id.Value()});
  if (templates.size() != 1U) {
    return MakeError(application::ErrorCode::kConflict, "工坊奖励道具不可用");
  }
  const auto stack = templates[0][0].As<std::string_view>();
  const auto max_quantity = templates[0][1].As<std::int64_t>();
  const auto max_duration =
      templates[0][2].IsNull()
          ? std::nullopt
          : std::optional{templates[0][2].As<std::int64_t>()};
  auto old = LockItem(transaction, player_id, item_id, now);
  if (old.has_value() && !old->active) {
    transaction.Exec(
        "DELETE FROM `PlayerEquipment` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              old->instance_id.Value()});
  }
  const auto expires_before = old.has_value() ? old->expires_at : std::nullopt;
  std::int64_t quantity_after = 1;
  std::int64_t quantity_delta = 0;
  std::optional<std::int64_t> expires_after;
  if (stack == "merge_quantity") {
    if (duration.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "数量型工坊奖励携带了期限");
    }
    const auto active_quantity =
        old.has_value() && old->active ? old->quantity : 0;
    if (active_quantity > max_quantity - quantity) {
      return MakeError(application::ErrorCode::kConflict,
                       "工坊奖励数量已达到上限");
    }
    quantity_after = active_quantity + quantity;
    quantity_delta = quantity;
  } else if (stack == "single_instance") {
    if (quantity != 1U || !duration.has_value() || !max_duration.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "限时工坊奖励模板契约无效");
    }
    if (old.has_value() && old->active && !old->expires_at.has_value()) {
      expires_after = std::nullopt;
    } else {
      const auto base =
          old.has_value() && old->active && old->expires_at.has_value()
              ? std::max(now, *old->expires_at)
              : now;
      if (duration->count() > *max_duration ||
          base - now > *max_duration - duration->count()) {
        return MakeError(application::ErrorCode::kConflict,
                         "工坊奖励期限已达到上限");
      }
      expires_after = base + duration->count();
    }
    quantity_delta = old.has_value() && old->active ? 0 : 1;
  } else {
    return MakeError(application::ErrorCode::kInternal,
                     "工坊奖励使用未知库存策略");
  }
  const auto stored = transaction.Exec(
      "INSERT INTO "
      "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
      "VALUES($1,$2,$3,FROM_UNIXTIME($4),CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) "
      "END,'workshop_reward') ON CONFLICT(`PlayerId`,`ItemId`) DO UPDATE SET "
      "`Quantity`=VALUES(`Quantity`),`ExpiresAt`=VALUES(`ExpiresAt`),`SourceCode`='workshop_reward'"
      " RETURNING `ItemInstanceId`",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            item_id.Value(), quantity_after,
                                            now, expires_after});
  const auto instance = stored[0][0].As<std::uint64_t>();
  if (quantity_delta != 0 || expires_before != expires_after) {
    transaction.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
        " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $6"
        " IS NULL THEN NULL ELSE FROM_UNIXTIME($6) END,'workshop_reward',$7,FROM_UNIXTIME($8))",
        bf::infrastructure::mysql::Parameters{
            transaction, player_id.Value(), item_id.Value(), instance,
            quantity_delta, expires_before, expires_after, operation_id, now});
  }
  return std::nullopt;
}

std::optional<application::ServiceError> MakePermanent(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id, domain::ItemId item_id, std::int64_t now,
    std::string_view operation_id) {
  auto item = LockItem(transaction, player_id, item_id, now);
  if (!item.has_value() || !item->active) {
    return MakeError(application::ErrorCode::kConflict,
                     "没有该装备或装备已过期");
  }
  if (item->equipped) {
    return MakeError(application::ErrorCode::kConflict, "请先卸下要操作的装备");
  }
  if (!item->expires_at.has_value()) {
    return MakeError(application::ErrorCode::kConflict, "该装备已经是永久");
  }
  transaction.Exec(
      "UPDATE `PlayerItems` SET `ExpiresAt`=NULL,`SourceCode`='workshop_permanent' WHERE "
      "`ItemInstanceId`=$1",
      bf::infrastructure::mysql::Parameters{transaction,
                                            item->instance_id.Value()});
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
      " VALUES($1,$2,$3,0,FROM_UNIXTIME($4),NULL,'workshop_permanent',$5,FROM_UNIXTIME($6))",
      bf::infrastructure::mysql::Parameters{
          transaction, player_id.Value(), item_id.Value(),
          item->instance_id.Value(), *item->expires_at, operation_id, now});
  return std::nullopt;
}

}  // namespace

MySqlWorkshopRepository::MySqlWorkshopRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::WorkshopResult>
MySqlWorkshopRepository::Execute(const application::WorkshopCommand& command) {
  const bool compound =
      command.action == application::WorkshopAction::kCompound;
  if (compound &&
      (!command.compound_policy.has_value() || command.inputs.size() != 5U ||
       command.option > 5U || command.required_material_id.Value() != 100622U ||
       command.required_material_quantity != 1U ||
       command.reward_duration != std::chrono::seconds{0} ||
       std::ranges::find(command.inputs, command.reward_item_id) ==
           command.inputs.end())) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "合成命令不符合五材料和保护槽契约");
  }
  if (compound) {
    const auto& policy = *command.compound_policy;
    if (policy.version != 2U || policy.input_count != 5U ||
        policy.policy_epoch == 0U ||
        policy.protection_material_item_id.Value() != 190110U ||
        policy.minimum_remaining_days != 20U ||
        policy.maximum_remaining_days != 10'000U ||
        policy.permanent_threshold_days != 720U ||
        policy.probability_policy != "uniform-input-independent-percent-v1" ||
        policy.protected_merge_policy !=
            "add-retained-after-generated-threshold-v1" ||
        policy.catalog_sha256.size() != 64U ||
        policy.catalog_sha256.find_first_not_of("0123456789abcdef") !=
            std::string::npos ||
        policy.minimum_percent != 50U || policy.maximum_percent != 100U ||
        command.compound_percent < 50U || command.compound_percent > 100U ||
        (command.option == 0U ? command.protection_percent != 0U
                              : command.protection_percent < 50U ||
                                    command.protection_percent > 100U)) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "合成百分比政策或选项无效");
    }
    auto sorted = command.inputs;
    std::ranges::sort(sorted);
    if (std::ranges::adjacent_find(sorted) != sorted.end()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "合成材料模板重复");
    }
  }
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto now = UnixSeconds(command.now);
    const auto now_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            command.now.time_since_epoch())
            .count();
    const auto maximum_input = 10'001LL * kMicrosecondsPerDay;
    if (compound &&
        (now_microseconds < 0 ||
         now_microseconds >
             std::numeric_limits<std::int64_t>::max() - maximum_input)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "合成时钟超出本服已发布期限范围");
    }
    const auto player = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.player_id.Value()});
    if (player.empty()) {
      return MakeError(application::ErrorCode::kInvalidState, "工坊玩家不存在");
    }
    const auto rules = transaction.Exec(
        "SELECT `Payload`,LOWER(HEX(`ContentDigest`)) FROM `ConfigurationVersions` WHERE "
        "`ConfigurationVersionId`=$1 AND `ConfigurationKind`='service_rules' AND "
        "`ReleaseStatus`='published' FOR SHARE",
        bf::infrastructure::mysql::Parameters{transaction,
                                              command.rules_version_id});
    if (rules.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "工坊规则版本已不可用");
    }
    const auto rules_payload = Json::parse(rules[0][0].As<std::string>());
    Json compound_catalog;
    if (compound) {
      const auto digest =
          transaction.Exec("SELECT LOWER(SHA2($1,256))=$2",
                           bf::infrastructure::mysql::Parameters{
                               transaction, rules_payload.dump(),
                               rules[0][1].As<std::string>()});
      if (digest.empty() || !digest[0][0].As<bool>()) {
        return MakeError(application::ErrorCode::kConflict,
                         "合成新政策发布快照摘要不一致");
      }
      if (auto error = LoadCompoundCatalog(
              transaction, *command.compound_policy, compound_catalog)) {
        return *error;
      }
    }
    if (compound &&
        (rules_payload.at("schema_version") != 2U ||
         rules_payload.at("policy").at("workshop").at("compound_policy") !=
             CompoundPolicySnapshot(*command.compound_policy))) {
      return MakeError(application::ErrorCode::kConflict,
                       "合成命令与不可变新政策不一致");
    }
    const auto operation = command.operation_id.ToUuidString();
    const auto canonical =
        "workshop:" + std::to_string(command.player_id.Value()) + ":" +
        std::to_string(static_cast<std::uint16_t>(command.action)) + ":" +
        ItemArray(command.inputs).dump() + ":" +
        std::to_string(command.option) + ":" +
        std::to_string(command.reward_item_id.Value()) + ":" +
        (compound ? CompoundPolicySnapshot(*command.compound_policy).dump()
                  : "legacy") +
        ":" + std::to_string(command.compound_percent) + ":" +
        std::to_string(command.protection_percent);
    transaction.Exec(
        "INSERT INTO "
        "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
        " VALUES($1,$2,'workshop_operation',$1,UNHEX(SHA2($3,256)))",
        bf::infrastructure::mysql::Parameters{
            transaction, operation, command.player_id.Value(), canonical});

    std::vector<StoredItem> locked_inputs;
    Json compound_inputs = Json::array();
    std::int64_t total_remaining_microseconds = 0;
    for (const auto input : command.inputs) {
      auto item = LockItem(transaction, command.player_id, input, now);
      if (!item.has_value() || !item->active) {
        return MakeError(application::ErrorCode::kConflict,
                         "没有该装备或装备已过期");
      }
      if (item->equipped) {
        return MakeError(application::ErrorCode::kConflict,
                         "请先卸下要操作的装备");
      }
      if (compound) {
        const auto& policy = *command.compound_policy;
        if (item->quantity != 1 || !item->expires_at_microseconds.has_value() ||
            *item->expires_at_microseconds <= now_microseconds ||
            *item->expires_at_microseconds > now_microseconds + maximum_input) {
          return MakeError(application::ErrorCode::kConflict,
                           "合成材料必须是一件有效限时装备");
        }
        const auto remaining =
            *item->expires_at_microseconds - now_microseconds;
        if (auto error = VerifyCompoundTemplate(transaction, input, policy,
                                                rules_payload, compound_catalog,
                                                remaining)) {
          return *error;
        }
        // D11570以0.5秒的日数epsilon再截断；账本保留完整剩余微秒，资格不把它改成整天。
        const auto whole_days = (remaining + 500'000LL) / kMicrosecondsPerDay;
        if (whole_days < policy.minimum_remaining_days ||
            whole_days > policy.maximum_remaining_days ||
            remaining >
                (static_cast<std::int64_t>(policy.maximum_remaining_days) +
                 1LL) *
                    kMicrosecondsPerDay) {
          return MakeError(application::ErrorCode::kConflict,
                           "合成材料剩余期限不足或超过原版范围");
        }
        if (total_remaining_microseconds >
            std::numeric_limits<std::int64_t>::max() - remaining) {
          return MakeError(application::ErrorCode::kConflict,
                           "合成材料总期限溢出");
        }
        total_remaining_microseconds += remaining;
        compound_inputs.push_back(
            {{"item_id", input.Value()},
             {"item_instance_id", item->instance_id.Value()},
             {"expires_at_microseconds", *item->expires_at_microseconds},
             {"remaining_microseconds", remaining},
             {"whole_days", whole_days}});
        const auto& flags =
            rules_payload.at("policy").at("workshop").at("item_flags");
        const auto source =
            std::ranges::find_if(flags, [input](const auto& value) {
              return value.at("item_id") == input.Value();
            });
        compound_inputs.back()["raw_client_fields"] =
            source->at("raw_client_fields");
      }
      locked_inputs.push_back(*item);
    }
    if (auto error = Consume(transaction, command.player_id,
                             command.required_material_id,
                             command.required_material_quantity, now, operation,
                             "workshop_material_consume", true)) {
      return *error;
    }
    if (compound && command.option != 0U) {
      if (auto error = Consume(
              transaction, command.player_id,
              command.compound_policy->protection_material_item_id, 1U, now,
              operation, "workshop_protection_material_consume", true)) {
        return *error;
      }
    }

    application::WorkshopResult result{.action = command.action,
                                       .rewards = {},
                                       .consumed_inputs = {},
                                       .reward_days = 0.0};
    if (command.action == application::WorkshopAction::kPermanent) {
      if (auto error = MakePermanent(transaction, command.player_id,
                                     command.inputs.front(), now, operation)) {
        return *error;
      }
      result.rewards.push_back(command.inputs.front());
      result.reward_days = 36'500.0;
    } else if (compound) {
      const auto generated =
          PercentOf(total_remaining_microseconds, command.compound_percent);
      const bool permanent =
          generated > static_cast<std::int64_t>(
                          command.compound_policy->permanent_threshold_days) *
                          kMicrosecondsPerDay;
      for (std::size_t index = 0U; index < command.inputs.size(); ++index) {
        const auto input = command.inputs[index];
        if (command.option != 0U && index + 1U == command.option) {
          const auto remaining =
              *locked_inputs[index].expires_at_microseconds - now_microseconds;
          const auto retained =
              PercentOf(remaining, command.protection_percent);
          RetainProtectedItem(transaction, command.player_id, input,
                              locked_inputs[index], now_microseconds, retained,
                              operation);
        } else {
          ConsumeCompoundInput(transaction, command.player_id, input,
                               locked_inputs[index], now_microseconds,
                               operation);
          result.consumed_inputs.push_back(input);
        }
      }
      if (auto error = GrantCompound(transaction, command.player_id,
                                     command.reward_item_id, now_microseconds,
                                     generated, permanent, operation)) {
        return *error;
      }
      result.rewards.push_back(command.reward_item_id);
      result.reward_days =
          permanent ? 36'500.0
                    : static_cast<double>(generated) / kMicrosecondsPerDay;
    } else {
      for (const auto input : command.inputs) {
        if (auto error = Consume(transaction, command.player_id, input, 1U, now,
                                 operation, "workshop_input_consume", true)) {
          return *error;
        }
      }
      result.consumed_inputs = command.inputs;
      {
        if (auto error =
                Grant(transaction, command.player_id, command.reward_item_id,
                      static_cast<std::uint32_t>(command.inputs.size()),
                      std::nullopt, now, operation)) {
          return *error;
        }
        result.rewards.assign(command.inputs.size(), command.reward_item_id);
      }
    }
    Json input_snapshot{
        {"inputs", ItemArray(command.inputs)},
        {"option", command.option},
        {"material_item_id", command.required_material_id.Value()},
        {"material_quantity", command.required_material_quantity}};
    Json reward_snapshot{{"rewards", ItemArray(result.rewards)},
                         {"reward_days", result.reward_days}};
    if (compound) {
      const auto generated =
          PercentOf(total_remaining_microseconds, command.compound_percent);
      input_snapshot["compound"] = Json{
          {"policy", CompoundPolicySnapshot(*command.compound_policy)},
          {"observed_at_microseconds", now_microseconds},
          {"registered_items", compound_inputs},
          {"protected_slot", command.option},
          {"compound_percent", command.compound_percent},
          {"protection_percent", command.protection_percent},
          {"total_remaining_microseconds", total_remaining_microseconds},
          {"protection_material_item_id",
           command.compound_policy->protection_material_item_id.Value()},
          {"protection_material_quantity", command.option == 0U ? 0U : 1U}};
      reward_snapshot["compound"] = Json{
          {"generated_microseconds", generated},
          {"permanent_from_generated_threshold",
           generated > static_cast<std::int64_t>(
                           command.compound_policy->permanent_threshold_days) *
                           kMicrosecondsPerDay},
          {"consumed_inputs", ItemArray(result.consumed_inputs)},
          {"bonus_rewards_restored", false}};
      const auto awarded =
          LockItem(transaction, command.player_id, command.reward_item_id, now);
      reward_snapshot["compound"]["result_instance_id"] =
          awarded->instance_id.Value();
      reward_snapshot["compound"]["result_expires_at_microseconds"] =
          awarded->expires_at_microseconds.has_value()
              ? Json(*awarded->expires_at_microseconds)
              : Json(nullptr);
      if (command.option != 0U) {
        const auto slot = command.option - 1U;
        const auto retained = PercentOf(
            *locked_inputs[slot].expires_at_microseconds - now_microseconds,
            command.protection_percent);
        const auto protected_item =
            LockItem(transaction, command.player_id, command.inputs[slot], now);
        reward_snapshot["compound"]["protected_item_id"] =
            command.inputs[slot].Value();
        reward_snapshot["compound"]["protected_instance_id"] =
            protected_item->instance_id.Value();
        reward_snapshot["compound"]["protected_retained_microseconds"] =
            retained;
        reward_snapshot["compound"]["protected_final_expires_at_microseconds"] =
            protected_item->expires_at_microseconds.has_value()
                ? Json(*protected_item->expires_at_microseconds)
                : Json(nullptr);
      }
    }
    transaction.Exec(
        "INSERT INTO "
        "`WorkshopOperations`(`WorkshopOperationId`,`PlayerId`,`OperationCode`,`InputSnapshot`,`RewardSnapshot`,`OperationStatus`,`RulesVersionId`,`CreatedAt`,`CompletedAt`)"
        " VALUES($1,$2,$3,$4,$5,'succeeded',$6,FROM_UNIXTIME($7),FROM_UNIXTIME($7))",
        bf::infrastructure::mysql::Parameters{
            transaction, operation, command.player_id.Value(),
            static_cast<std::uint16_t>(command.action), input_snapshot.dump(),
            reward_snapshot.dump(), command.rules_version_id, now});
    transaction.Exec(
        "UPDATE `OperationRequests` SET "
        "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=FROM_UNIXTIME($3) WHERE "
        "`OperationRequestId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, operation,
                                              "workshop:" + operation, now});
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return MakeError(application::ErrorCode::kConflict, "工坊操作重复");
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "执行工坊操作时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return MakeSqlError("执行工坊操作", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "执行工坊操作时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
