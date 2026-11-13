#include "persistence/mysql/gift_repository.h"

#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <utility>

#include "features/shop/purchase.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
using application::ErrorCode;
using application::GiftView;
using application::ServiceError;
template <typename T>
using Result = application::ServiceResult<T>;

ServiceError Failure(ErrorCode code, std::string message) {
  return {.code = code, .safe_message = std::move(message)};
}

template <typename T, typename Work>
Result<T> Transaction(MySqlConnectionPool& pool, Work work) {
  auto acquired = pool.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    auto result = work(tx);
    if (!std::holds_alternative<ServiceError>(result)) tx.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("礼物/兑换券事务失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
  } catch (const std::exception&) {
    spdlog::error("礼物/兑换券事务无法完成");
  }
  return Failure(ErrorCode::kStorageUnavailable, "礼物或兑换券服务暂时不可用");
}

constexpr auto kGiftColumns =
    "SELECT "
    "g.`GiftId`,g.`SenderId`,g.`RecipientId`,s.`Nickname`,r.`Nickname`,g.`ItemId`,g.`StockId`,g.`Quantity`,g.`DurationSeconds`,floor(UNIX_TIMESTAMP(g.`CreatedAt`)),floor(UNIX_TIMESTAMP(g.`ProcessedAt`)),g.`GiftState`,g.`Message`,g.`Thanks`,g.`ThankKind`"
    " FROM `PlayerGifts` g JOIN `PlayerProfiles` s ON s.`PlayerId`=g.`SenderId` JOIN "
    "`PlayerProfiles` r ON r.`PlayerId`=g.`RecipientId` ";

GiftView View(const bf::infrastructure::mysql::Row& row) {
  return {.id = row[0].As<std::uint32_t>(),
          .sender = domain::PlayerId{row[1].As<std::uint64_t>()},
          .recipient = domain::PlayerId{row[2].As<std::uint64_t>()},
          .sender_name = row[3].As<std::string>(),
          .recipient_name = row[4].As<std::string>(),
          .item = row[5].As<std::uint32_t>(),
          .stock = row[6].As<std::uint32_t>(),
          .quantity = row[7].As<std::uint32_t>(),
          .duration_seconds = row[8].IsNull()
                                  ? std::nullopt
                                  : std::optional{row[8].As<std::int64_t>()},
          .created_at = row[9].As<std::int64_t>(),
          .processed_at = row[10].IsNull()
                              ? std::nullopt
                              : std::optional{row[10].As<std::int64_t>()},
          .state = row[11].As<std::uint16_t>(),
          .message = row[12].As<std::string>(),
          .thanks = row[13].As<std::string>(),
          .thank_kind = row[14].As<std::uint16_t>()};
}

std::uint32_t Unread(bf::infrastructure::mysql::WriteTransaction& tx,
                     domain::PlayerId player) {
  return tx
      .Exec(
          "SELECT count(*) FROM `PlayerGifts` WHERE `RecipientId`=$1 AND `GiftState`=0",
          bf::infrastructure::mysql::Parameters{tx, player.Value()})[0][0]
      .As<std::uint32_t>();
}

Result<domain::PurchaseOffer> Offer(
    bf::infrastructure::mysql::WriteTransaction& tx, std::uint64_t stock,
    bool require_enabled = true) {
  const auto rows = tx.Exec(
      "SELECT "
      "o.`ItemId`,o.`CurrencyCode`,o.`UnitPrice`,o.`GrantQuantity`,o.`DurationSeconds`,t.`StackPolicy`,t.`DurationPolicy`,t.`MaxQuantity`,t.`MaxDurationSeconds`"
      " FROM `ShopOffers` o JOIN `ItemTemplates` t USING(`ItemId`) JOIN `ConfigurationVersions` v"
      " ON v.`ConfigurationVersionId`=o.`ConfigurationVersionId` JOIN `ConfigurationVersions` tv "
      "ON tv.`ConfigurationVersionId`=t.`ConfigurationVersionId` WHERE `StockId`=$1 AND (NOT $2 "
      "OR o.`IsEnabled`) AND t.`IsGrantable` AND v.`ReleaseStatus`='published' AND "
      "tv.`ReleaseStatus`='published' FOR SHARE OF o,t,v,tv",
      bf::infrastructure::mysql::Parameters{tx, stock, require_enabled});
  if (rows.empty())
    return Failure(ErrorCode::kInvalidInput, "礼物商品不存在或已停用");
  const auto& row = rows[0];
  const auto currency = row[1].As<std::string>();
  const auto stack = row[5].As<std::string>();
  const auto duration = row[6].As<std::string>();
  if ((currency != "lucci" && currency != "bubble_cash" &&
       currency != "treasure_bubble") ||
      (stack != "merge_quantity" && stack != "single_instance") ||
      (duration != "permanent" && duration != "expiring" &&
       duration != "offer_defined"))
    return Failure(ErrorCode::kInvalidState, "礼物商品定义损坏");
  return domain::PurchaseOffer{
      .stock_id = domain::StockId{stock},
      .item_id = domain::ItemId{row[0].As<std::uint64_t>()},
      .currency = currency == "lucci" ? domain::CurrencyCode::kLucci
                  : currency == "bubble_cash"
                      ? domain::CurrencyCode::kBubbleCash
                      : domain::CurrencyCode::kTreasureBubble,
      .unit_price = row[2].As<std::int64_t>(),
      .grant_quantity = row[3].As<std::int64_t>(),
      .duration_seconds = row[4].IsNull()
                              ? std::nullopt
                              : std::optional{row[4].As<std::int64_t>()},
      .stack_policy = stack == "merge_quantity"
                          ? domain::ItemStackPolicy::kMergeQuantity
                          : domain::ItemStackPolicy::kSingleInstance,
      .duration_policy =
          duration == "permanent"  ? domain::ItemDurationPolicy::kPermanent
          : duration == "expiring" ? domain::ItemDurationPolicy::kExpiring
                                   : domain::ItemDurationPolicy::kOfferDefined,
      .max_quantity = row[7].As<std::int64_t>(),
      .max_duration_seconds = row[8].IsNull()
                                  ? std::nullopt
                                  : std::optional{row[8].As<std::int64_t>()}};
}

struct GiftComponent final {
  std::uint32_t ordinal;
  std::uint64_t source_stock;
  domain::PurchaseOffer goods;
};

// Same real component definitions as self-purchase; no synthetic StockSet
// inventory item.
Result<std::vector<GiftComponent>> Components(
    bf::infrastructure::mysql::WriteTransaction& tx,
    const domain::PurchaseOffer& sale) {
  if (sale.stock_id.Value() < 2'000'000U)
    return std::vector<GiftComponent>{{1U, sale.stock_id.Value(), sale}};
  if (sale.stock_id.Value() >= 3'000'000U)
    return Failure(ErrorCode::kInvalidInput, "套装销售编号无效");
  // Lock the parent too: a publisher must not add/remove members while we take
  // the snapshot.
  if (tx.Exec(
            "SELECT `StockId` FROM `ShopBundles` WHERE `StockId`=$1 FOR SHARE",
            bf::infrastructure::mysql::Parameters{tx, sale.stock_id.Value()})
          .empty())
    return Failure(ErrorCode::kInvalidState, "套装定义缺失");
  const auto rows = tx.Exec(
      "SELECT "
      "m.`Ordinal`,m.`SourceStockId`,m.`ItemId`,m.`GrantQuantity`,m.`DurationSeconds`,t.`StackPolicy`,t.`DurationPolicy`,t.`MaxQuantity`,t.`MaxDurationSeconds`,t.`IsGrantable`,v.`ReleaseStatus`"
      " FROM `ShopBundleItems` m JOIN `ItemTemplates` t USING(`ItemId`) JOIN "
      "`ConfigurationVersions` v ON v.`ConfigurationVersionId`=t.`ConfigurationVersionId` WHERE "
      "m.`StockId`=$1 ORDER BY m.`Ordinal` FOR SHARE OF m,t,v",
      bf::infrastructure::mysql::Parameters{tx, sale.stock_id.Value()});
  if (rows.empty() || rows.size() > 10U)
    return Failure(ErrorCode::kInvalidState, "套装部件不完整");
  std::vector<GiftComponent> result;
  for (const auto& row : rows) {
    const auto stack = row[5].As<std::string>();
    const auto duration = row[6].As<std::string>();
    if (row[0].As<std::uint32_t>() != result.size() + 1U ||
        !row[9].As<bool>() || row[10].As<std::string>() != "published" ||
        (stack != "merge_quantity" && stack != "single_instance") ||
        (duration != "permanent" && duration != "expiring" &&
         duration != "offer_defined"))
      return Failure(ErrorCode::kInvalidState, "套装部件已停用或定义损坏");
    auto goods = sale;
    goods.item_id = domain::ItemId{row[2].As<std::uint64_t>()};
    goods.grant_quantity = row[3].As<std::int64_t>();
    goods.duration_seconds = row[4].IsNull()
                                 ? std::nullopt
                                 : std::optional{row[4].As<std::int64_t>()};
    goods.stack_policy = stack == "merge_quantity"
                             ? domain::ItemStackPolicy::kMergeQuantity
                             : domain::ItemStackPolicy::kSingleInstance;
    goods.duration_policy =
        duration == "permanent"  ? domain::ItemDurationPolicy::kPermanent
        : duration == "expiring" ? domain::ItemDurationPolicy::kExpiring
                                 : domain::ItemDurationPolicy::kOfferDefined;
    goods.max_quantity = row[7].As<std::int64_t>();
    goods.max_duration_seconds = row[8].IsNull()
                                     ? std::nullopt
                                     : std::optional{row[8].As<std::int64_t>()};
    result.push_back(
        {row[0].As<std::uint32_t>(), row[1].As<std::uint64_t>(), goods});
  }
  // shop_offers carries the first real component for existing order/receipt
  // compatibility.
  if (result.front().goods.item_id != sale.item_id ||
      result.front().goods.grant_quantity != sale.grant_quantity ||
      result.front().goods.duration_seconds != sale.duration_seconds)
    return Failure(ErrorCode::kInvalidState, "套装主规格与部件定义冲突");
  return result;
}

void FreezeComponents(bf::infrastructure::mysql::WriteTransaction& tx,
                      std::uint64_t gift,
                      const std::vector<GiftComponent>& items) {
  for (const auto& item : items) {
    const auto& goods = item.goods;
    const auto stack =
        goods.stack_policy == domain::ItemStackPolicy::kMergeQuantity
            ? "merge_quantity"
            : "single_instance";
    const auto duration =
        goods.duration_policy == domain::ItemDurationPolicy::kPermanent
            ? "permanent"
        : goods.duration_policy == domain::ItemDurationPolicy::kExpiring
            ? "expiring"
            : "offer_defined";
    tx.Exec(
        "INSERT INTO "
        "`PlayerGiftItems`(`GiftId`,`Ordinal`,`SourceStockId`,`ItemId`,`GrantQuantity`,`DurationSeconds`,`StackPolicy`,`DurationPolicy`,`MaxQuantity`,`MaxDurationSeconds`)"
        " VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
        bf::infrastructure::mysql::Parameters{
            tx, gift, item.ordinal, item.source_stock, goods.item_id.Value(),
            goods.grant_quantity, goods.duration_seconds, stack, duration,
            goods.max_quantity, goods.max_duration_seconds});
  }
}

Result<std::vector<domain::PurchaseOffer>> FrozenComponents(
    bf::infrastructure::mysql::WriteTransaction& tx, const GiftView& gift) {
  const auto count =
      tx.Exec("SELECT `ComponentCount` FROM `PlayerGifts` WHERE `GiftId`=$1",
              bf::infrastructure::mysql::Parameters{tx, gift.id})[0][0]
          .As<std::uint32_t>();
  const auto rows = tx.Exec(
      "SELECT "
      "i.`Ordinal`,i.`ItemId`,i.`GrantQuantity`,i.`DurationSeconds`,i.`StackPolicy`,i.`DurationPolicy`,i.`MaxQuantity`,i.`MaxDurationSeconds`,t.`IsGrantable`,v.`ReleaseStatus`,t.`StackPolicy`,t.`DurationPolicy`,t.`MaxQuantity`,t.`MaxDurationSeconds`,i.`SnapshotKind`"
      " FROM `PlayerGiftItems` i JOIN `ItemTemplates` t USING(`ItemId`) JOIN "
      "`ConfigurationVersions` v ON v.`ConfigurationVersionId`=t.`ConfigurationVersionId` WHERE "
      "i.`GiftId`=$1 ORDER BY i.`Ordinal` FOR SHARE OF i,t,v",
      bf::infrastructure::mysql::Parameters{tx, gift.id});
  if (count == 0U || count > 10U ||
      static_cast<std::uint32_t>(rows.size()) != count)
    return Failure(ErrorCode::kConflict, "礼物缺少完整成交快照，不能部分领取");
  std::vector<domain::PurchaseOffer> result;
  for (const auto& row : rows) {
    const auto kind = row[14].As<std::string>();
    const bool legacy = kind == "legacy_single";
    if ((kind != "frozen" && !legacy) ||
        (legacy && (count != 1U || gift.stock >= 2'000'000U)))
      return Failure(ErrorCode::kConflict, "礼物成交快照类型损坏");
    // Old singles never recorded historical policies/caps. Only quantity/time
    // are frozen; use today's published template, so disablement blocks now but
    // is not permanent data loss.
    const auto stack = row[legacy ? 10 : 4].As<std::string>();
    const auto duration = row[legacy ? 11 : 5].As<std::string>();
    if (row[0].As<std::uint32_t>() != result.size() + 1U ||
        !row[8].As<bool>() || row[9].As<std::string>() != "published" ||
        (!legacy && (stack != row[10].As<std::string>() ||
                     duration != row[11].As<std::string>())) ||
        (stack != "merge_quantity" && stack != "single_instance") ||
        (duration != "permanent" && duration != "expiring" &&
         duration != "offer_defined"))
      return Failure(ErrorCode::kConflict, "礼物模板已停用或库存策略发生冲突");
    const auto duration_column = legacy ? 13 : 7;
    auto maximum_duration =
        row[duration_column].IsNull()
            ? std::nullopt
            : std::optional{row[duration_column].As<std::int64_t>()};
    if (!legacy && maximum_duration) {
      if (row[13].IsNull())
        return Failure(ErrorCode::kConflict, "礼物模板期限规则发生冲突");
      maximum_duration =
          std::min(*maximum_duration, row[13].As<std::int64_t>());
    }
    result.push_back(
        {.stock_id = domain::StockId{gift.stock},
         .item_id = domain::ItemId{row[1].As<std::uint64_t>()},
         .unit_price = 0,
         .grant_quantity = row[2].As<std::int64_t>(),
         .duration_seconds = row[3].IsNull()
                                 ? std::nullopt
                                 : std::optional{row[3].As<std::int64_t>()},
         .stack_policy = stack == "merge_quantity"
                             ? domain::ItemStackPolicy::kMergeQuantity
                             : domain::ItemStackPolicy::kSingleInstance,
         .duration_policy = duration == "permanent"
                                ? domain::ItemDurationPolicy::kPermanent
                            : duration == "expiring"
                                ? domain::ItemDurationPolicy::kExpiring
                                : domain::ItemDurationPolicy::kOfferDefined,
         .max_quantity = legacy ? row[12].As<std::int64_t>()
                                : std::min(row[6].As<std::int64_t>(),
                                           row[12].As<std::int64_t>()),
         .max_duration_seconds = maximum_duration});
  }
  if (result.front().item_id.Value() != gift.item ||
      result.front().grant_quantity != gift.quantity ||
      result.front().duration_seconds != gift.duration_seconds)
    return Failure(ErrorCode::kConflict, "礼物主记录与成交快照冲突");
  // Every asset writer takes the player lock first; template order keeps row
  // locking stable too.
  std::ranges::sort(result, {},
                    [](const auto& goods) { return goods.item_id.Value(); });
  return result;
}

Result<bool> Grant(bf::infrastructure::mysql::WriteTransaction& tx,
                   domain::PlayerId player, const domain::PurchaseOffer& goods,
                   const std::string& business, std::string_view reason,
                   bool preview = false) {
  const auto now =
      tx.Exec("SELECT floor(UNIX_TIMESTAMP(CURRENT_TIMESTAMP(6)))")[0][0]
          .As<std::int64_t>();
  const auto owned_rows = tx.Exec(
      "SELECT `ItemInstanceId`,`Quantity`,floor(UNIX_TIMESTAMP(`ExpiresAt`)) FROM `PlayerItems` "
      "WHERE `PlayerId`=$1 AND `ItemId`=$2 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                            goods.item_id.Value()});
  std::optional<domain::OwnedItemSnapshot> owned;
  if (!owned_rows.empty()) {
    const auto& row = owned_rows[0];
    owned = domain::OwnedItemSnapshot{
        .item_instance_id = domain::ItemInstanceId{row[0].As<std::uint64_t>()},
        .item_id = goods.item_id,
        .quantity = row[1].As<std::int64_t>(),
        .expires_at_unix_seconds =
            row[2].IsNull() ? std::nullopt
                            : std::optional{row[2].As<std::int64_t>()}};
  }
  const auto effect_result =
      domain::CalculatePurchaseInventoryEffect(goods, owned, now);
  const auto* effect =
      std::get_if<domain::PurchaseInventoryEffect>(&effect_result);
  if (!effect)
    return Failure(ErrorCode::kConflict,
                   "库存已拥有永久道具或数量、期限达到上限");
  if (preview) return false;
  const auto inserted = tx.Exec(
      "INSERT INTO "
      "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
      "VALUES($1,$2,$3,FROM_UNIXTIME($4),FROM_UNIXTIME($5),$6) ON CONFLICT(`PlayerId`,`ItemId`) "
      "DO UPDATE SET `Quantity`=VALUES(`Quantity`),`ExpiresAt`=VALUES(`ExpiresAt`) RETURNING "
      "`ItemInstanceId`",
      bf::infrastructure::mysql::Parameters{
          tx, player.Value(), goods.item_id.Value(), effect->quantity_after,
          now, effect->expires_after, reason});
  tx.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`)"
      " VALUES($1,$2,$3,$4,FROM_UNIXTIME($5),FROM_UNIXTIME($6),$7,$8)",
      bf::infrastructure::mysql::Parameters{
          tx, player.Value(), goods.item_id.Value(),
          inserted[0][0].As<std::uint64_t>(), effect->quantity_delta,
          effect->expires_before, effect->expires_after, reason, business});
  return true;
}

bool TextValid(const std::string& text) {
  return text.size() <= 1024U &&
         std::count_if(
             text.begin(), text.end(),
             [](unsigned char ch) { return (ch & 0xc0U) != 0x80U; }) <= 256 &&
         std::ranges::none_of(text, [](unsigned char value) {
           return value < 0x20U && value != '\n' && value != '\r';
         });
}
}  // namespace

Result<application::GiftPage> MySqlGiftStore::List(domain::PlayerId player,
                                                   bool sent,
                                                   std::uint32_t offset,
                                                   std::uint32_t count) {
  if (!player.IsValid() || count == 0U || count > 100U || offset > 1'000'000U)
    return Failure(ErrorCode::kInvalidInput, "礼物分页参数无效");
  return Transaction<application::GiftPage>(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> Result<application::GiftPage> {
        const auto condition =
            sent ? "g.`SenderId`=$1 AND NOT g.`SenderHidden`"
                 : "g.`RecipientId`=$1 AND NOT g.`RecipientHidden`";
        application::GiftPage result;
        result.total =
            tx.Exec(std::string{"SELECT count(*) FROM `PlayerGifts` g WHERE "} +
                        condition,
                    bf::infrastructure::mysql::Parameters{tx,
                                                          player.Value()})[0][0]
                .As<std::uint32_t>();
        result.unread = Unread(tx, player);
        const auto rows =
            tx.Exec(std::string{kGiftColumns} + "WHERE " + condition +
                        " ORDER BY `GiftId` DESC LIMIT $3 OFFSET $2",
                    bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                          offset, count});
        for (const auto& row : rows) result.gifts.push_back(View(row));
        return result;
      });
}

Result<GiftView> MySqlGiftStore::Detail(domain::PlayerId player,
                                        std::uint32_t gift) {
  return Transaction<GiftView>(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx) -> Result<GiftView> {
        const auto rows = tx.Exec(
            std::string{kGiftColumns} +
                "WHERE `GiftId`=$1 AND (g.`SenderId`=$2 OR g.`RecipientId`=$2)",
            bf::infrastructure::mysql::Parameters{tx, gift, player.Value()});
        if (rows.empty())
          return Failure(ErrorCode::kPermissionDenied,
                         "礼物不存在或不属于当前角色");
        return View(rows[0]);
      });
}

Result<GiftView> MySqlGiftStore::Send(
    const application::SendGiftCommand& command) {
  if (!command.sender.IsValid() || !command.stock.IsValid() ||
      command.recipient.empty() || !TextValid(command.message))
    return Failure(ErrorCode::kInvalidInput, "赠送请求无效");
  return Transaction<GiftView>(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx) -> Result<GiftView> {
        const auto recipients = tx.Exec(
            "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `Nickname`=$1 AND `IsCharacterCreated`",
            bf::infrastructure::mysql::Parameters{tx, command.recipient});
        if (recipients.empty())
          return Failure(ErrorCode::kInvalidInput, "找不到收件角色");
        const domain::PlayerId recipient{recipients[0][0].As<std::uint64_t>()};
        if (recipient == command.sender)
          return Failure(ErrorCode::kInvalidInput, "不能赠送给自己");
        const auto locks = tx.Exec(
            "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId` IN ($1,$2) AND "
            "`IsCharacterCreated` ORDER BY `PlayerId` FOR UPDATE",
            bf::infrastructure::mysql::Parameters{tx, command.sender.Value(),
                                                  recipient.Value()});
        if (locks.size() != 2U)
          return Failure(ErrorCode::kInvalidState, "赠送角色尚未创建");
        const auto business = command.operation.ToUuidString();
        const auto replay =
            tx.Exec(std::string{kGiftColumns} + "WHERE g.`BusinessId`=$1",
                    bf::infrastructure::mysql::Parameters{tx, business});
        if (!replay.empty()) {
          const auto saved = View(replay[0]);
          if (saved.sender != command.sender || saved.recipient != recipient ||
              saved.stock != command.stock.Value() ||
              saved.message != command.message)
            return Failure(ErrorCode::kConflict, "赠送操作标识已用于其他请求");
          return saved;
        }
        if (!tx.Exec(
                   "SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR "
                   "(`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)",
                   bf::infrastructure::mysql::Parameters{
                       tx, command.sender.Value(), recipient.Value()})
                 .empty())
          return Failure(ErrorCode::kPermissionDenied, "无法向该角色赠送礼物");
        auto loaded = Offer(tx, command.stock.Value());
        if (const auto* error = std::get_if<ServiceError>(&loaded))
          return *error;
        const auto& goods = std::get<domain::PurchaseOffer>(loaded);
        if (goods.currency == domain::CurrencyCode::kTreasureBubble ||
            goods.unit_price <= 0)
          return Failure(ErrorCode::kPermissionDenied, "该销售规格不能赠送");
        auto expanded = Components(tx, goods);
        if (const auto* error = std::get_if<ServiceError>(&expanded))
          return *error;
        const auto& items = std::get<std::vector<GiftComponent>>(expanded);
        auto ordered = items;
        std::ranges::sort(ordered, {}, [](const auto& item) {
          return item.goods.item_id.Value();
        });
        for (const auto& item : ordered) {
          auto preview =
              Grant(tx, recipient, item.goods, business, "gift", true);
          if (const auto* error = std::get_if<ServiceError>(&preview))
            return *error;
          // Overlap may be on the last component, or inside a different
          // bundle/single-item gift.
          if (item.goods.stack_policy ==
                  domain::ItemStackPolicy::kSingleInstance &&
              !item.goods.duration_seconds &&
              !tx.Exec(
                     "SELECT 1 FROM `PlayerGifts` g JOIN `PlayerGiftItems` i USING(`GiftId`) WHERE "
                     "g.`RecipientId`=$1 AND g.`GiftState`=0 AND i.`ItemId`=$2 AND "
                     "(i.`StackPolicy`='single_instance' OR i.`SnapshotKind`='legacy_single') AND "
                     "i.`DurationSeconds` IS NULL",
                     bf::infrastructure::mysql::Parameters{
                         tx, recipient.Value(), item.goods.item_id.Value()})
                   .empty())
            return Failure(ErrorCode::kConflict, "收件箱中已有该永久套装部件");
        }
        const auto wallet = tx.Exec(
            "UPDATE `PlayerWallets` SET `Balance`=`Balance`-$3,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
            "`PlayerId`=$1 AND `CurrencyCode`=$2 AND `Balance` >= $3 RETURNING `Balance`",
            bf::infrastructure::mysql::Parameters{
                tx, command.sender.Value(),
                domain::CurrencyCodeName(goods.currency), goods.unit_price});
        if (wallet.empty())
          return Failure(ErrorCode::kInsufficientBalance, "赠送余额不足");
        const auto balance = wallet[0][0].As<std::int64_t>();
        const auto saved = tx.Exec(
            "INSERT INTO "
            "`PlayerGifts`(`BusinessId`,`SenderId`,`RecipientId`,`StockId`,`ItemId`,`Quantity`,`DurationSeconds`,`CurrencyCode`,`Price`,`Message`,`ComponentCount`)"
            " VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) RETURNING `GiftId`",
            bf::infrastructure::mysql::Parameters{
                tx, business, command.sender.Value(), recipient.Value(),
                command.stock.Value(), goods.item_id.Value(),
                goods.grant_quantity, goods.duration_seconds,
                domain::CurrencyCodeName(goods.currency), goods.unit_price,
                command.message, items.size()});
        FreezeComponents(tx, saved[0][0].As<std::uint64_t>(), items);
        tx.Exec(
            "INSERT INTO "
            "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
            " VALUES($1,$2,$3,$4,$5,'gift_send',$6)",
            bf::infrastructure::mysql::Parameters{
                tx, command.sender.Value(),
                domain::CurrencyCodeName(goods.currency), -goods.unit_price,
                balance + goods.unit_price, balance, business});
        return View(tx.Exec(std::string{kGiftColumns} + "WHERE `GiftId`=$1",
                            bf::infrastructure::mysql::Parameters{
                                tx, saved[0][0].As<std::uint64_t>()})[0]);
      });
}

Result<application::GiftActionResult> MySqlGiftStore::Act(
    domain::PlayerId player, std::uint32_t gift, application::GiftAction action,
    const std::string& thanks, std::uint16_t thank_kind) {
  if (!TextValid(thanks) ||
      (action == application::GiftAction::kThank &&
       (thanks.empty() || thank_kind < 2U || thank_kind > 5U)))
    return Failure(ErrorCode::kInvalidInput, "感谢留言无效");
  return Transaction<application::GiftActionResult>(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> Result<application::GiftActionResult> {
        tx.Exec(
            "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
            bf::infrastructure::mysql::Parameters{tx, player.Value()});
        const auto rows = tx.Exec(
            std::string{kGiftColumns} + "WHERE `GiftId`=$1 FOR UPDATE OF g",
            bf::infrastructure::mysql::Parameters{tx, gift});
        if (rows.empty())
          return Failure(ErrorCode::kInvalidInput, "礼物不存在");
        auto value = View(rows[0]);
        const bool sent = action == application::GiftAction::kHideSent;
        if ((sent ? value.sender : value.recipient) != player)
          return Failure(ErrorCode::kPermissionDenied,
                         "不能操作其他角色的礼物");
        bool changed = false;
        if (action == application::GiftAction::kClaim ||
            action == application::GiftAction::kDiscard) {
          const auto desired =
              action == application::GiftAction::kClaim ? 1U : 2U;
          if (value.state != 0U && value.state != desired)
            return Failure(ErrorCode::kConflict,
                           "礼物已经处理，不能再次领取或丢弃");
          if (value.state == 0U) {
            if (desired == 1U) {
              auto loaded = FrozenComponents(tx, value);
              if (const auto* error = std::get_if<ServiceError>(&loaded))
                return *error;
              const auto business =
                  tx.Exec(
                        "SELECT `BusinessId` FROM `PlayerGifts` WHERE `GiftId`=$1",
                        bf::infrastructure::mysql::Parameters{tx, gift})[0][0]
                      .As<std::string>();
              // The gift/player locks serialize claim/discard/replay. All
              // grants, per-item audit lines and the terminal state commit
              // together; any last-component failure aborts.
              for (const auto& goods :
                   std::get<std::vector<domain::PurchaseOffer>>(loaded)) {
                auto granted = Grant(tx, player, goods, business, "gift_claim");
                if (const auto* error = std::get_if<ServiceError>(&granted))
                  return *error;
              }
            }
            tx.Exec(
                "UPDATE `PlayerGifts` SET `GiftState`=$2,`ProcessedAt`=CURRENT_TIMESTAMP(6) WHERE "
                "`GiftId`=$1",
                bf::infrastructure::mysql::Parameters{tx, gift, desired});
            changed = true;
          }
        } else if (action == application::GiftAction::kThank) {
          if (value.state != 1U ||
              (value.thank_kind != 0U &&
               (value.thanks != thanks || value.thank_kind != thank_kind)))
            return Failure(ErrorCode::kConflict,
                           "感谢留言仅能在领取后提交一次");
          changed = value.thanks != thanks;
          tx.Exec(
              "UPDATE `PlayerGifts` SET `Thanks`=$2,`ThankKind`=$3 WHERE `GiftId`=$1",
              bf::infrastructure::mysql::Parameters{tx, gift, thanks,
                                                    thank_kind});
        } else {
          if (!sent && value.state == 0U)
            return Failure(ErrorCode::kConflict, "请先领取或明确丢弃礼物");
          tx.Exec(
              sent
                  ? "UPDATE `PlayerGifts` SET `SenderHidden`=true WHERE `GiftId`=$1"
                  : "UPDATE `PlayerGifts` SET `RecipientHidden`=true WHERE `GiftId`=$1",
              bf::infrastructure::mysql::Parameters{tx, gift});
          changed = true;
        }
        value =
            View(tx.Exec(std::string{kGiftColumns} + "WHERE `GiftId`=$1",
                         bf::infrastructure::mysql::Parameters{tx, gift})[0]);
        return application::GiftActionResult{std::move(value),
                                             Unread(tx, player), changed};
      });
}

Result<application::CouponResult> MySqlGiftStore::Redeem(
    domain::PlayerId player, const std::string& code) {
  if (code.size() < 8U || code.size() > 64U || !player.IsValid() ||
      std::ranges::any_of(code, [](unsigned char ch) {
        return !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= 'a' && ch <= 'z') || ch == '-');
      }))
    return Failure(ErrorCode::kInvalidInput, "兑换码格式无效");
  auto normalized = code;
  for (auto& ch : normalized)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  std::array<unsigned char, 32U> digest{};
  unsigned int length = 0U;
  if (EVP_Digest(normalized.data(), normalized.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1 ||
      length != digest.size())
    return Failure(ErrorCode::kInternal, "无法校验兑换码");
  constexpr char digits[] = "0123456789abcdef";
  std::string hash;
  for (const auto byte : digest) {
    hash += digits[byte >> 4U];
    hash += digits[byte & 15U];
  }
  return Transaction<application::CouponResult>(
      pool_,
      [&](bf::infrastructure::mysql::WriteTransaction& tx)
          -> Result<application::CouponResult> {
        const auto profile = tx.Exec(
            "SELECT `AccountId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
            "UPDATE",
            bf::infrastructure::mysql::Parameters{tx, player.Value()});
        if (profile.empty())
          return Failure(ErrorCode::kPermissionDenied, "角色尚未创建");
        const auto account = profile[0][0].As<std::uint64_t>();
        const auto coupons = tx.Exec(
            "SELECT `CouponId`,`StockId`,`CurrencyCode`,`CurrencyAmount`,`Enabled` AND "
            "CURRENT_TIMESTAMP(6) >= `StartsAt` AND CURRENT_TIMESTAMP(6) < `ExpiresAt` AND `UsedCount` "
            "< `MaximumUses` FROM `Coupons` WHERE `CodeHash`=$1 FOR UPDATE",
            bf::infrastructure::mysql::Parameters{tx, hash});
        if (coupons.empty())
          return Failure(ErrorCode::kInvalidInput, "兑换码不存在");
        const auto& coupon = coupons[0];
        const auto id = coupon[0].As<std::uint64_t>();
        const auto replay = tx.Exec(
            "SELECT `CurrencyAmount`,`ItemCount` FROM `CouponRedemptions` WHERE `CouponId`=$1 AND "
            "`AccountId`=$2",
            bf::infrastructure::mysql::Parameters{tx, id, account});
        if (!replay.empty())
          return application::CouponResult{replay[0][0].As<std::uint32_t>(),
                                           replay[0][1].As<std::uint32_t>(),
                                           false};
        if (!coupon[4].As<bool>())
          return Failure(ErrorCode::kConflict,
                         "兑换码尚未生效、已过期或已用完");
        const auto business = tx.Exec("SELECT UUID()")[0][0].As<std::string>();
        std::uint32_t item_count = 0U;
        if (!coupon[1].IsNull()) {
          auto loaded = Offer(tx, coupon[1].As<std::uint64_t>());
          if (const auto* error = std::get_if<ServiceError>(&loaded))
            return *error;
          const auto& goods = std::get<domain::PurchaseOffer>(loaded);
          auto expanded = Components(tx, goods);
          if (const auto* error = std::get_if<ServiceError>(&expanded))
            return *error;
          auto items =
              std::get<std::vector<GiftComponent>>(std::move(expanded));
          std::ranges::sort(items, {}, [](const auto& item) {
            return item.goods.item_id.Value();
          });
          // A coupon names a sale specification too. A StockSet must grant
          // every real component before its quota/receipt or wallet reward can
          // commit; 831 retains its scalar count.
          for (const auto& item : items) {
            const auto quantity = item.goods.grant_quantity;
            if (quantity <= 0 ||
                quantity > static_cast<std::int64_t>(
                               std::numeric_limits<std::uint32_t>::max()) -
                               item_count)
              return Failure(ErrorCode::kInvalidState,
                             "兑换券完整物品数量超出原生范围");
            auto granted =
                Grant(tx, player, item.goods, business, "coupon_redeem");
            if (const auto* error = std::get_if<ServiceError>(&granted))
              return *error;
            item_count += static_cast<std::uint32_t>(quantity);
          }
        }
        const auto amount = coupon[3].As<std::uint32_t>();
        if (amount != 0U) {
          const auto currency = coupon[2].As<std::string>();
          const auto wallet = tx.Exec(
              "UPDATE `PlayerWallets` SET `Balance`=`Balance`+$3,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
              "`PlayerId`=$1 AND `CurrencyCode`=$2 AND `Balance` <= 2147483647-$3 RETURNING `Balance`",
              bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                    currency, amount});
          if (wallet.empty())
            return Failure(ErrorCode::kConflict, "奖励钱包已达到上限");
          const auto balance = wallet[0][0].As<std::int64_t>();
          tx.Exec(
              "INSERT INTO "
              "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
              " VALUES($1,$2,$3,$4,$5,'coupon_redeem',$6)",
              bf::infrastructure::mysql::Parameters{
                  tx, player.Value(), currency, amount, balance - amount,
                  balance, business});
        }
        tx.Exec(
            "INSERT INTO "
            "`CouponRedemptions`(`CouponId`,`AccountId`,`PlayerId`,`BusinessId`,`CurrencyAmount`,`ItemCount`)"
            " VALUES($1,$2,$3,$4,$5,$6)",
            bf::infrastructure::mysql::Parameters{
                tx, id, account, player.Value(), business, amount, item_count});
        tx.Exec(
            "UPDATE `Coupons` SET `UsedCount`=`UsedCount`+1 WHERE `CouponId`=$1",
            bf::infrastructure::mysql::Parameters{tx, id});
        return application::CouponResult{amount, item_count, true};
      });
}
}  // namespace bf::infrastructure::mysql
