#include "persistence/mysql/native_title_repository.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
using application::ErrorCode;
using application::NativeTitleResult;

application::ServiceError Failure(ErrorCode code, const char* text) {
  return {.code = code,
          .safe_message = text,
          .correlation_id = {},
          .is_retryable = false};
}

std::int64_t Micros(std::chrono::system_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
      .count();
}

NativeTitleResult Reply(std::uint32_t title, std::int32_t code,
                        bool changed = false) {
  return {.result = code, .title_id = title, .changed = changed};
}
}  // namespace

MySqlNativeTitleRepository::MySqlNativeTitleRepository(
    MySqlConnectionPool& pool, const application::SystemClock& clock) noexcept
    : pool_(pool), clock_(clock) {}

application::ServiceResult<NativeTitleResult> MySqlNativeTitleRepository::Set(
    domain::PlayerId player, std::uint32_t title, std::uint32_t action) {
  if (!player.IsValid() || title == 0U || action > 1U)
    return Failure(ErrorCode::kInvalidInput, "称号穿戴身份或动作非法");
  auto acquired = pool_.Acquire();
  if (const auto* e = std::get_if<application::ServiceError>(&acquired))
    return *e;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto now = Micros(clock_.Now());
    if (tx.Exec(
              "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
              "UPDATE",
              bf::infrastructure::mysql::Parameters{tx, player.Value()})
            .empty())
      return Reply(title, -3);
    const auto item = tx.Exec(
        "SELECT i.`ItemInstanceId`,d.`TitleType` FROM `PlayerItems` i JOIN `NativeTitleDefinitions`"
        " d ON d.`TitleId`=i.`ItemId` JOIN `ItemTemplates` t ON t.`ItemId`=i.`ItemId` JOIN "
        "`ConfigurationVersions` v ON v.`ConfigurationVersionId`=t.`ConfigurationVersionId` WHERE "
        "i.`PlayerId`=$1 AND i.`ItemId`=$2 AND i.`Quantity`=1 AND (i.`ExpiresAt` IS NULL OR "
        "i.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) AND v.`ReleaseStatus`='published' FOR UPDATE OF"
        " i",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), title, now});
    if (item.empty()) return Reply(title, -3);
    if (action == 0U && item[0][1].As<std::uint32_t>() == 11U &&
        tx.Exec("SELECT `PlayerId` FROM `CoupleMembers` WHERE `PlayerId`=$1",
                bf::infrastructure::mysql::Parameters{tx, player.Value()})
            .empty())
      return Reply(title, -13);
    const auto instance = item[0][0].As<std::uint64_t>();
    const auto previous = tx.Exec(
        "SELECT `ItemInstanceId` FROM `NativeTitleEquipment` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, player.Value()});
    const bool same =
        !previous.empty() && previous[0][0].As<std::uint64_t>() == instance;
    const bool changed = action == 0U ? !same : same;
    if (changed) {
      if (action == 0U)
        static_cast<void>(tx.Exec(
            "INSERT INTO `NativeTitleEquipment`(`PlayerId`,`ItemInstanceId`,`TitleId`) VALUES($1,$2,$3)"
            " ON CONFLICT(`PlayerId`) DO UPDATE SET "
            "`ItemInstanceId`=VALUES(`ItemInstanceId`),`TitleId`=VALUES(`TitleId`),`SelectedAt`=SYSDATE(6)",
            bf::infrastructure::mysql::Parameters{tx, player.Value(), instance,
                                                  title}));
      else
        static_cast<void>(tx.Exec(
            "DELETE FROM `NativeTitleEquipment` WHERE `PlayerId`=$1 AND `ItemInstanceId`=$2",
            bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                  instance}));
      static_cast<void>(tx.Exec(
          "INSERT INTO "
          "`NativeTitleSelectionEvents`(`PlayerId`,`TitleId`,`ItemInstanceId`,`ActionFlag`) "
          "VALUES($1,$2,$3,$4)",
          bf::infrastructure::mysql::Parameters{tx, player.Value(), title,
                                                instance, action}));
    }
    tx.Commit();
    return Reply(title, action == 0U ? 1 : 2, changed);
  } catch (const std::exception& e) {
    spdlog::warn("native title set unconfirmed: {}", e.what());
    return Failure(ErrorCode::kStorageUnavailable, "称号穿戴事务未确认");
  }
}

application::ServiceResult<NativeTitleResult> MySqlNativeTitleRepository::Claim(
    domain::PlayerId player, std::uint32_t title,
    const domain::OperationId& operation) {
  if (!player.IsValid() || title == 0U)
    return Failure(ErrorCode::kInvalidInput, "称号兑换身份非法");
  auto acquired = pool_.Acquire();
  if (const auto* e = std::get_if<application::ServiceError>(&acquired))
    return *e;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto now = Micros(clock_.Now());
    if (tx.Exec(
              "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
              "UPDATE",
              bf::infrastructure::mysql::Parameters{tx, player.Value()})
            .empty())
      return Reply(title, -3);
    const auto replay = tx.Exec(
        "SELECT `PlayerId`,`TitleId` FROM `NativeTitleClaims` WHERE `ClaimId`=$1",
        bf::infrastructure::mysql::Parameters{tx, operation.ToUuidString()});
    if (!replay.empty()) {
      if (replay[0][0].As<std::uint64_t>() != player.Value() ||
          replay[0][1].As<std::uint32_t>() != title)
        return Failure(ErrorCode::kConflict, "称号兑换UUID不能迁移身份或称号");
      tx.Commit();
      return Reply(title, 0);
    }
    const auto definition = tx.Exec(
        "SELECT `StockId`,`ClaimEnabled` FROM `NativeTitleDefinitions` WHERE `TitleId`=$1",
        bf::infrastructure::mysql::Parameters{tx, title});
    if (definition.empty()) return Reply(title, -3);
    if (!definition[0][1].As<bool>()) return Reply(title, -1);
    const auto owned = tx.Exec(
        "SELECT `ItemInstanceId`,`Quantity`,`ExpiresAt` FROM `PlayerItems` WHERE `PlayerId`=$1 AND "
        "`ItemId`=$2 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), title});
    if (!owned.empty())
      return Reply(title,
                   4);  // 原重复回执不再扣勋章；已过期实例先按原808合法删弃。
    const auto conditions = tx.Exec(
        "SELECT r.`MedalKey`,r.`RequiredCount`,coalesce(b.AvailableCount,0) FROM "
        "`NativeTitleRequirements` r LEFT JOIN `NativeMedalBalances` b ON b.`PlayerId`=$2 AND "
        "b.`MedalKey`=r.`MedalKey` WHERE r.`TitleId`=$1 ORDER BY r.`MedalKey`",
        bf::infrastructure::mysql::Parameters{tx, title, player.Value()});
    if (conditions.empty())
      return Reply(title, -1);  // 空条件或0/0哨兵的额外原服资格仍缺。
    for (const auto& row : conditions) {
      const auto balance = row[2].As<std::int64_t>();
      if (balance < row[1].As<std::int64_t>() || balance > 0x7fffffffLL)
        return Reply(title, -1);
    }
    // 原stock线宽字段只在不可变配置payload中保存；shop_offers没有client_fields。
    // 同时核对关系数据与原发布快照，不借商城的本服价格给勋章兑换收费。
    const auto offer = tx.Exec(
        "SELECT "
        "o.`GrantQuantity`,o.`DurationSeconds`,JSON_UNQUOTE(JSON_EXTRACT(s.Element,'$.client_duration_type')),JSON_UNQUOTE(JSON_EXTRACT(s.Element,'$.client_duration_value')),t.`StackPolicy`,t.`IsGrantable`,JSON_UNQUOTE(JSON_EXTRACT(s.Element,'$.grant_quantity')),JSON_EXTRACT(s.Element,'$.duration_seconds'),t.`MaxQuantity`"
        " FROM `ShopOffers` o JOIN `ItemTemplates` t ON t.`ItemId`=o.`ItemId` AND "
        "t.`ConfigurationVersionId`=o.`ConfigurationVersionId` JOIN `ConfigurationVersions` v ON "
        "v.`ConfigurationVersionId`=o.`ConfigurationVersionId` CROSS JOIN "
        "JSON_TABLE(JSON_EXTRACT(v.`Payload`,'$.shop_offers'), '$[*]' COLUMNS (Element JSON PATH "
        "'$')) s WHERE o.`StockId`=$1 AND o.`ItemId`=$2 AND v.`ConfigurationKind`='catalog' AND "
        "v.`ReleaseStatus`='published' AND "
        "(JSON_UNQUOTE(JSON_EXTRACT(s.Element,'$.stock_id')))=o.`StockId` AND "
        "(JSON_UNQUOTE(JSON_EXTRACT(s.Element,'$.item_id')))=o.`ItemId`",
        bf::infrastructure::mysql::Parameters{
            tx, definition[0][0].As<std::uint32_t>(), title});
    if (offer.size() != 1U || offer[0][0].As<std::int64_t>() != 1 ||
        !offer[0][1].IsNull() || offer[0][2].As<std::uint32_t>() != 9U ||
        offer[0][3].As<std::uint64_t>() != 0xffffffffULL ||
        offer[0][4].As<std::string>() != "single_instance" ||
        !offer[0][5].As<bool>() || offer[0][6].As<std::int64_t>() != 1 ||
        offer[0][7].As<std::string>() != "null" ||
        offer[0][8].As<std::int64_t>() != 1)
      return Failure(ErrorCode::kInvalidState, "称号原stock永久单件映射未确证");
    const auto inserted = tx.Exec(
        "INSERT INTO `PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`SourceCode`) "
        "VALUES($1,$2,1,FROM_UNIXTIME($3/1000000.0),'native_title_claim') RETURNING "
        "`ItemInstanceId`",
        bf::infrastructure::mysql::Parameters{tx, player.Value(), title, now});
    const auto instance = inserted[0][0].As<std::uint64_t>();
    const auto audit = tx.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,$2,$3,1,'native_title_claim',$4) RETURNING `InventoryTransactionId`",
        bf::infrastructure::mysql::Parameters{
            tx, player.Value(), title, instance, operation.ToUuidString()});
    static_cast<void>(tx.Exec(
        "INSERT INTO "
        "`NativeTitleClaims`(`ClaimId`,`PlayerId`,`TitleId`,`ItemInstanceId`,`InventoryTransactionId`)"
        " VALUES($1,$2,$3,$4,$5)",
        bf::infrastructure::mysql::Parameters{
            tx, operation.ToUuidString(), player.Value(), title, instance,
            audit[0][0].As<std::uint64_t>()}));
    for (const auto& row : conditions)
      static_cast<void>(tx.Exec(
          "INSERT INTO `NativeTitleMedalDebits`(`ClaimId`,`PlayerId`,`MedalKey`,`Amount`) "
          "VALUES($1,$2,$3,$4)",
          bf::infrastructure::mysql::Parameters{
              tx, operation.ToUuidString(), player.Value(),
              row[0].As<std::uint32_t>(), row[1].As<std::uint32_t>()}));
    const auto claim_valid =
        tx.Exec(
              "SELECT EXISTS(SELECT 1 FROM NativeTitleClaims c JOIN PlayerItems i ON "
              "i.ItemInstanceId=c.ItemInstanceId AND i.PlayerId=c.PlayerId AND i.ItemId=c.TitleId "
              "JOIN InventoryTransactions a ON a.InventoryTransactionId=c.InventoryTransactionId "
              "AND a.ItemInstanceId=c.ItemInstanceId AND a.PlayerId=c.PlayerId AND a.ItemId=c.TitleId "
              "AND a.BusinessId=c.ClaimId JOIN NativeTitleDefinitions d ON d.TitleId=c.TitleId "
              "WHERE c.ClaimId=$1 AND i.Quantity=1 AND i.ExpiresAt IS NULL AND i.SourceCode='native_title_claim' "
              "AND a.QuantityDelta=1 AND a.ReasonCode='native_title_claim' AND d.ClaimEnabled "
              "AND EXISTS(SELECT 1 FROM NativeTitleRequirements r WHERE r.TitleId=c.TitleId) "
              "AND NOT EXISTS(SELECT 1 FROM NativeTitleRequirements r LEFT JOIN NativeTitleMedalDebits b "
              "ON b.ClaimId=c.ClaimId AND b.MedalKey=r.MedalKey "
              "WHERE r.TitleId=c.TitleId AND (b.Amount IS NULL OR b.Amount<>r.RequiredCount)))",
              Parameters{tx, operation.ToUuidString()})
            .OneField()
            .As<bool>();
    if (!claim_valid)
      return Failure(ErrorCode::kConflict, "称号兑换流水校验失败");
    tx.Commit();
    return Reply(title, 0, true);
  } catch (const std::exception& e) {
    spdlog::warn("native title claim unconfirmed: {}", e.what());
    return Failure(ErrorCode::kStorageUnavailable, "称号兑换事务未确认");
  }
}
}  // namespace bf::infrastructure::mysql
