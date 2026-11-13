#include "persistence/mysql/purchase_repository.h"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

using Digest = std::array<std::uint8_t, 32U>;

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

bf::infrastructure::mysql::BytesView AsBytesView(const Digest& digest) {
  return {reinterpret_cast<const std::byte*>(digest.data()), digest.size()};
}

std::optional<Digest> RequestDigest(
    const application::PurchaseCommand& command) {
  const auto canonical = std::string{"purchase_offer:"} +
                         std::to_string(command.player_id.Value()) + ":" +
                         std::to_string(command.stock_id.Value());
  Digest digest{};
  unsigned int length = 0U;
  if (EVP_Digest(canonical.data(), canonical.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1 ||
      length != static_cast<unsigned int>(digest.size())) {
    return std::nullopt;
  }
  return digest;
}

bool DigestMatches(const bf::infrastructure::mysql::Bytes& saved,
                   const Digest& expected) {
  if (saved.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    if (std::to_integer<std::uint8_t>(saved[index]) != expected[index]) {
      return false;
    }
  }
  return true;
}

std::optional<domain::CurrencyCode> CurrencyFromDatabase(
    std::string_view value) {
  if (value == "lucci") {
    return domain::CurrencyCode::kLucci;
  }
  if (value == "bubble_cash") {
    return domain::CurrencyCode::kBubbleCash;
  }
  if (value == "treasure_bubble") {
    return domain::CurrencyCode::kTreasureBubble;
  }
  return std::nullopt;
}

std::optional<domain::ItemStackPolicy> StackPolicyFromDatabase(
    std::string_view value) {
  if (value == "merge_quantity") {
    return domain::ItemStackPolicy::kMergeQuantity;
  }
  if (value == "single_instance") {
    return domain::ItemStackPolicy::kSingleInstance;
  }
  return std::nullopt;
}

std::optional<domain::ItemDurationPolicy> DurationPolicyFromDatabase(
    std::string_view value) {
  if (value == "permanent") {
    return domain::ItemDurationPolicy::kPermanent;
  }
  if (value == "expiring") {
    return domain::ItemDurationPolicy::kExpiring;
  }
  if (value == "offer_defined") {
    return domain::ItemDurationPolicy::kOfferDefined;
  }
  return std::nullopt;
}

class MySqlPurchaseTransaction final : public application::PurchaseTransaction {
 public:
  explicit MySqlPurchaseTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::PurchaseOperationClaim>
  ClaimOperation(const application::PurchaseCommand& command) override {
    const auto digest = RequestDigest(command);
    if (!digest.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "无法生成购买请求摘要");
    }
    const auto operation_id = command.operation_id.ToUuidString();
    try {
      // 玩家父记录是同一玩家所有资产写事务的稳定锁，消除“均未见库存行后同时插入”的窗口。
      const auto player = transaction_.Exec(
          "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                command.player_id.Value()});
      if (player.empty()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "购买玩家不存在");
      }
      const auto inserted = transaction_.Exec(
          "INSERT INTO "
          "`OperationRequests`(`OperationRequestId`,`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`)"
          " VALUES($1,$2,'purchase_offer',$1,$3) "
          // 同一操作同时受 UUID 主键和玩家作用域幂等键约束。并发重放时 MySQL
          // 可能先观察到任一唯一约束，因此必须让两类冲突都进入下方的锁定与一致性校验。
          "ON CONFLICT DO NOTHING RETURNING `OperationRequestId`",
          bf::infrastructure::mysql::Parameters{transaction_, operation_id,
                                                command.player_id.Value(),
                                                AsBytesView(*digest)});
      const auto rows = transaction_.Exec(
          "SELECT "
          "`PlayerId`,`OperationType`,`IdempotencyKey`,`RequestDigest`,`OperationStatus`,`ResultReference`"
          " FROM `OperationRequests` WHERE `OperationRequestId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_, operation_id});
      if (rows.size() != 1U ||
          rows[0][0].As<std::uint64_t>() != command.player_id.Value() ||
          rows[0][1].As<std::string_view>() != "purchase_offer" ||
          rows[0][2].As<std::string>() != operation_id) {
        return MakeError(application::ErrorCode::kConflict,
                         "购买操作标识已用于不同请求");
      }
      const auto saved_digest =
          rows[0][3].As<bf::infrastructure::mysql::Bytes>();
      if (!DigestMatches(saved_digest, *digest)) {
        return MakeError(application::ErrorCode::kConflict,
                         "购买操作标识的请求摘要冲突");
      }
      const auto status = rows[0][4].As<std::string_view>();
      if (!inserted.empty()) {
        if (status != "pending") {
          return MakeError(application::ErrorCode::kInternal,
                           "新购买操作没有处于 pending 状态");
        }
        return application::PurchaseOperationClaim{
            .state = application::PurchaseOperationState::kNew,
            .completed_result = std::nullopt};
      }
      if (status != "succeeded" || rows[0][5].IsNull()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "购买操作仍未完成或已失败");
      }
      const auto result = transaction_.Exec(
          "SELECT "
          "orders.`OrderId`,orders.`PlayerId`,item.`StockId`,item.`ItemId`,(JSON_UNQUOTE(JSON_EXTRACT(item.`OfferSnapshot`,'$.item_instance_id'))),orders.`CurrencyCode`,orders.`TotalAmount`,(JSON_UNQUOTE(JSON_EXTRACT(item.`OfferSnapshot`,'$.balance_after'))),(JSON_UNQUOTE(JSON_EXTRACT(item.`OfferSnapshot`,'$.quantity_after'))),CASE"
          " WHEN JSON_CONTAINS_PATH(CAST(item.`OfferSnapshot` AS JSON),'one','$.expires_after') AND "
          "JSON_UNQUOTE(JSON_EXTRACT(item.`OfferSnapshot`,'$.expires_after')) <> '' THEN "
          "(JSON_UNQUOTE(JSON_EXTRACT(item.`OfferSnapshot`,'$.expires_after'))) ELSE NULL END FROM "
          "`PurchaseOrders` AS orders JOIN `PurchaseOrderItems` AS item ON "
          "item.`OrderId`=orders.`OrderId` WHERE orders.`OperationRequestId`=$1 AND "
          "orders.`OrderStatus`='settled' AND item.`LineNumber`=1 AND orders.`OrderId`=$2",
          bf::infrastructure::mysql::Parameters{transaction_, operation_id,
                                                rows[0][5].As<std::string>()});
      if (result.size() != 1U) {
        return MakeError(application::ErrorCode::kInternal,
                         "已完成购买缺少稳定结果快照");
      }
      const auto currency =
          CurrencyFromDatabase(result[0][5].As<std::string_view>());
      if (!currency.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "已完成购买包含未知币种");
      }
      std::optional<std::int64_t> expires_at;
      if (!result[0][9].IsNull()) {
        expires_at = result[0][9].As<std::int64_t>();
      }
      return application::PurchaseOperationClaim{
          .state = application::PurchaseOperationState::kCompletedReplay,
          .completed_result = application::PurchaseResult{
              .order_id = domain::OrderId{result[0][0].As<std::uint64_t>()},
              .player_id = domain::PlayerId{result[0][1].As<std::uint64_t>()},
              .stock_id = domain::StockId{result[0][2].As<std::uint64_t>()},
              .item_id = domain::ItemId{result[0][3].As<std::uint64_t>()},
              .item_instance_id =
                  domain::ItemInstanceId{result[0][4].As<std::uint64_t>()},
              .currency = *currency,
              .charged_amount = result[0][6].As<std::int64_t>(),
              .balance_after = result[0][7].As<std::int64_t>(),
              .quantity_after = result[0][8].As<std::int64_t>(),
              .expires_at_unix_seconds = expires_at}};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "占用购买操作时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("占用购买操作", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "占用购买操作时发生未分类错误");
    }
  }

  application::ServiceResult<domain::PurchaseOffer> LoadOffer(
      domain::StockId stock_id) override {
    try {
      const auto rows = transaction_.Exec(
          "SELECT "
          "offer.`StockId`,offer.`ItemId`,offer.`CurrencyCode`,offer.`UnitPrice`,offer.`GrantQuantity`,offer.`DurationSeconds`,template.`StackPolicy`,template.`DurationPolicy`,template.`MaxQuantity`,template.`MaxDurationSeconds`"
          " FROM `ShopOffers` AS offer JOIN `ItemTemplates` AS template ON "
          "template.`ItemId`=offer.`ItemId` JOIN `ConfigurationVersions` AS configuration ON "
          "configuration.`ConfigurationVersionId`=offer.`ConfigurationVersionId` JOIN "
          "`ConfigurationVersions` AS template_version ON "
          "template_version.`ConfigurationVersionId`=template.`ConfigurationVersionId` AND "
          "template_version.`ReleaseStatus`='published' WHERE offer.`StockId`=$1 AND "
          "offer.`IsEnabled` AND template.`IsGrantable` AND configuration.`ReleaseStatus`='published'"
          " FOR SHARE OF offer,template",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                stock_id.Value()});
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kInvalidInput,
                         "销售规格不存在或已停用");
      }
      const auto currency =
          CurrencyFromDatabase(rows[0][2].As<std::string_view>());
      const auto stack =
          StackPolicyFromDatabase(rows[0][6].As<std::string_view>());
      const auto duration =
          DurationPolicyFromDatabase(rows[0][7].As<std::string_view>());
      if (!currency.has_value() || !stack.has_value() ||
          !duration.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "销售规格包含未知领域枚举");
      }
      std::optional<std::int64_t> duration_seconds;
      std::optional<std::int64_t> maximum_duration;
      if (!rows[0][5].IsNull()) {
        duration_seconds = rows[0][5].As<std::int64_t>();
      }
      if (!rows[0][9].IsNull()) {
        maximum_duration = rows[0][9].As<std::int64_t>();
      }
      return domain::PurchaseOffer{
          .stock_id = domain::StockId{rows[0][0].As<std::uint64_t>()},
          .item_id = domain::ItemId{rows[0][1].As<std::uint64_t>()},
          .currency = *currency,
          .unit_price = rows[0][3].As<std::int64_t>(),
          .grant_quantity = rows[0][4].As<std::int64_t>(),
          .duration_seconds = duration_seconds,
          .stack_policy = *stack,
          .duration_policy = *duration,
          .max_quantity = rows[0][8].As<std::int64_t>(),
          .max_duration_seconds = maximum_duration};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取购买规格时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("读取购买规格", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取购买规格时发生未分类错误");
    }
  }

  application::ServiceResult<std::vector<domain::PurchaseOffer>> LoadItems(
      const domain::PurchaseOffer& offer) override {
    if (offer.stock_id.Value() < 2'000'000U) {
      return std::vector<domain::PurchaseOffer>{offer};
    }
    try {
      const auto rows = transaction_.Exec(
          "SELECT "
          "member.`ItemId`,member.`GrantQuantity`,member.`DurationSeconds`,template.`StackPolicy`,template.`DurationPolicy`,template.`MaxQuantity`,template.`MaxDurationSeconds`,template.`IsGrantable`,version.`ReleaseStatus`"
          " FROM `ShopBundleItems` member JOIN `ItemTemplates` template USING(`ItemId`) JOIN "
          "`ConfigurationVersions` version ON "
          "version.`ConfigurationVersionId`=template.`ConfigurationVersionId` WHERE "
          "member.`StockId`=$1 ORDER BY member.`Ordinal` FOR SHARE OF member,template",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                offer.stock_id.Value()});
      std::vector<domain::PurchaseOffer> items;
      for (const auto& row : rows) {
        const auto stack =
            StackPolicyFromDatabase(row[3].As<std::string_view>());
        const auto duration =
            DurationPolicyFromDatabase(row[4].As<std::string_view>());
        if (!stack || !duration || !row[7].As<bool>() ||
            row[8].As<std::string_view>() != "published") {
          return MakeError(application::ErrorCode::kInvalidState,
                           "套装部件已停用或定义损坏");
        }
        items.push_back(
            {.stock_id = offer.stock_id,
             .item_id = domain::ItemId{row[0].As<std::uint64_t>()},
             .currency = offer.currency,
             .unit_price = items.empty() ? offer.unit_price : 0,
             .grant_quantity = row[1].As<std::int64_t>(),
             .duration_seconds = row[2].IsNull()
                                     ? std::nullopt
                                     : std::optional{row[2].As<std::int64_t>()},
             .stack_policy = *stack,
             .duration_policy = *duration,
             .max_quantity = row[5].As<std::int64_t>(),
             .max_duration_seconds =
                 row[6].IsNull() ? std::nullopt
                                 : std::optional{row[6].As<std::int64_t>()}});
      }
      return items;
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("读取套装部件", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取套装部件失败");
    }
  }

  application::ServiceResult<std::optional<domain::OwnedItemSnapshot>>
  LockOwnedItem(domain::PlayerId player_id, domain::ItemId item_id) override {
    try {
      const auto rows = transaction_.Exec(
          "SELECT `ItemInstanceId`,`ItemId`,`Quantity`,CASE WHEN `ExpiresAt` IS NULL THEN NULL ELSE "
          "floor(UNIX_TIMESTAMP(`ExpiresAt`)) END FROM `PlayerItems` WHERE `PlayerId`=$1 AND "
          "`ItemId`=$2 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_, player_id.Value(),
                                                item_id.Value()});
      if (rows.empty()) {
        return std::optional<domain::OwnedItemSnapshot>{};
      }
      std::optional<std::int64_t> expires_at;
      if (!rows[0][3].IsNull()) {
        expires_at = rows[0][3].As<std::int64_t>();
      }
      return std::optional<domain::OwnedItemSnapshot>{domain::OwnedItemSnapshot{
          .item_instance_id =
              domain::ItemInstanceId{rows[0][0].As<std::uint64_t>()},
          .item_id = domain::ItemId{rows[0][1].As<std::uint64_t>()},
          .quantity = rows[0][2].As<std::int64_t>(),
          .expires_at_unix_seconds = expires_at}};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定购买库存时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("锁定购买库存", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定购买库存时发生未分类错误");
    }
  }

  application::ServiceResult<application::WalletDebit> DebitWallet(
      domain::PlayerId player_id, domain::CurrencyCode currency,
      std::int64_t amount) override {
    try {
      const auto rows = transaction_.Exec(
          "UPDATE `PlayerWallets` SET `Balance`=`Balance`-$3,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
          "`PlayerId`=$1 AND `CurrencyCode`=$2 AND `Balance` >= $3 RETURNING `Balance`+$3,`Balance`",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id.Value(),
              domain::CurrencyCodeName(currency), amount});
      if (rows.empty()) {
        const auto exists = transaction_.Exec(
            "SELECT EXISTS(SELECT 1 FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`=$2)",
            bf::infrastructure::mysql::Parameters{
                transaction_, player_id.Value(),
                domain::CurrencyCodeName(currency)});
        if (exists[0][0].As<bool>()) {
          return MakeError(application::ErrorCode::kInsufficientBalance,
                           "钱包余额不足");
        }
        return MakeError(application::ErrorCode::kInternal,
                         "购买玩家缺少目标币种钱包");
      }
      return application::WalletDebit{
          .currency = currency,
          .balance_before = rows[0][0].As<std::int64_t>(),
          .balance_after = rows[0][1].As<std::int64_t>()};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "扣减购买钱包时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("扣减购买钱包", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "扣减购买钱包时发生未分类错误");
    }
  }

  application::ServiceResult<domain::ItemInstanceId> ApplyInventory(
      domain::PlayerId player_id, const domain::PurchaseOffer& offer,
      const domain::PurchaseInventoryEffect& effect,
      std::int64_t now_unix_seconds) override {
    try {
      const auto updated = transaction_.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=$3,`ExpiresAt`=CASE WHEN $4 IS NULL THEN NULL ELSE "
          "FROM_UNIXTIME($4) END WHERE `PlayerId`=$1 AND `ItemId`=$2 RETURNING `ItemInstanceId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id.Value(), offer.item_id.Value(),
              effect.quantity_after, effect.expires_after});
      if (!updated.empty()) {
        return domain::ItemInstanceId{updated[0][0].As<std::uint64_t>()};
      }
      const auto inserted = transaction_.Exec(
          "INSERT INTO "
          "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
          "VALUES($1,$2,$3,FROM_UNIXTIME($4),CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) "
          "END,'purchase') RETURNING `ItemInstanceId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id.Value(), offer.item_id.Value(),
              effect.quantity_after, now_unix_seconds, effect.expires_after});
      return domain::ItemInstanceId{inserted[0][0].As<std::uint64_t>()};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "写入购买库存时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("写入购买库存", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "写入购买库存时发生未分类错误");
    }
  }

  application::ServiceResult<domain::OrderId> CreateSettledOrderAndAudits(
      const application::PurchaseCommand& command,
      const domain::PurchaseOffer& offer,
      std::span<const application::PurchaseItemGrant> items,
      const application::WalletDebit& debit,
      std::int64_t now_unix_seconds) override {
    const auto operation_id = command.operation_id.ToUuidString();
    try {
      const auto order = transaction_.Exec(
          "INSERT INTO "
          "`PurchaseOrders`(`PlayerId`,`OperationRequestId`,`OrderStatus`,`CurrencyCode`,`TotalAmount`,`CreatedAt`,`SettledAt`)"
          " VALUES($1,$2,'settled',$3,$4,FROM_UNIXTIME($5),FROM_UNIXTIME($5)) RETURNING `OrderId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, command.player_id.Value(), operation_id,
              domain::CurrencyCodeName(offer.currency), offer.unit_price,
              now_unix_seconds});
      const auto order_id = order[0][0].As<std::uint64_t>();
      transaction_.Exec(
          "INSERT INTO "
          "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
          " VALUES($1,$2,$3,$4,$5,'purchase',$6,FROM_UNIXTIME($7))",
          bf::infrastructure::mysql::Parameters{
              transaction_, command.player_id.Value(),
              domain::CurrencyCodeName(offer.currency), -offer.unit_price,
              debit.balance_before, debit.balance_after, operation_id,
              now_unix_seconds});
      int line = 0;
      for (const auto& grant : items) {
        transaction_.Exec(
            "INSERT INTO "
            "`PurchaseOrderItems`(`OrderId`,`LineNumber`,`StockId`,`ItemId`,`Quantity`,`UnitPrice`,`DurationSeconds`,`OfferSnapshot`)"
            " "
            "VALUES($1,$12,$2,$3,$4,$5,$6,JSON_OBJECT('stock_id',$2,'item_id',$3,'currency',$7,'grant_quantity',$4,'duration_seconds',$6,'item_instance_id',$8,'balance_after',$9,'quantity_after',$10,'expires_after',CASE"
            " WHEN $11 IS NULL THEN '' ELSE $11 END))",
            bf::infrastructure::mysql::Parameters{
                transaction_, order_id, grant.offer.stock_id.Value(),
                grant.offer.item_id.Value(), grant.offer.grant_quantity,
                grant.offer.unit_price, grant.offer.duration_seconds,
                domain::CurrencyCodeName(grant.offer.currency),
                grant.item_instance_id.Value(), debit.balance_after,
                grant.effect.quantity_after, grant.effect.expires_after,
                ++line});
        transaction_.Exec(
            "INSERT INTO "
            "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
            " VALUES($1,$2,$3,$4,CASE WHEN $5 IS NULL THEN NULL ELSE FROM_UNIXTIME($5) END,CASE WHEN $6"
            " IS NULL THEN NULL ELSE FROM_UNIXTIME($6) END,'purchase',$7,FROM_UNIXTIME($8))",
            bf::infrastructure::mysql::Parameters{
                transaction_, command.player_id.Value(),
                grant.offer.item_id.Value(), grant.item_instance_id.Value(),
                grant.effect.quantity_delta, grant.effect.expires_before,
                grant.effect.expires_after, operation_id, now_unix_seconds});
      }
      return domain::OrderId{order_id};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "写入购买订单与审计时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("写入购买订单与审计", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "写入购买订单与审计时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> CompleteOperation(
      const domain::OperationId& operation_id,
      domain::OrderId order_id) override {
    try {
      const auto rows = transaction_.Exec(
          "UPDATE `OperationRequests` SET "
          "`OperationStatus`='succeeded',`ResultReference`=$2,`CompletedAt`=CURRENT_TIMESTAMP(6) "
          "WHERE `OperationRequestId`=$1 AND `OperationStatus`='pending' RETURNING "
          "`OperationRequestId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, operation_id.ToUuidString(), order_id.Value()});
      if (rows.size() != 1U) {
        return MakeError(application::ErrorCode::kConflict,
                         "购买操作完成状态发生并发冲突");
      }
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "完成购买操作时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return MakeSqlError("完成购买操作", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "完成购买操作时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState, "购买事务已结束");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交购买事务时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      completed_ = true;
      return MakeSqlError("提交购买事务", error);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交购买事务时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool completed_{false};
};

}  // namespace

MySqlPurchaseUnitOfWork::MySqlPurchaseUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::unique_ptr<application::PurchaseTransaction>>
MySqlPurchaseUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::PurchaseTransaction>{
        std::make_unique<MySqlPurchaseTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始购买事务", true);
  }
}

}  // namespace bf::infrastructure::mysql
