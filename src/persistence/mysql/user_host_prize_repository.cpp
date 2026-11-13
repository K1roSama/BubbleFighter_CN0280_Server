#include "persistence/mysql/user_host_prize_repository.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "features/battle/match_reward.h"

namespace bf::infrastructure::mysql {
namespace {

void LockProfile(bf::infrastructure::mysql::TransactionBase& transaction,
                 domain::PlayerId player_id) {
  const auto row = transaction.Exec(
      "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  if (row.size() != 1U) throw std::runtime_error{"主办比赛资产所有者不存在"};
}

void RefundTicket(bf::infrastructure::mysql::TransactionBase& transaction,
                  std::string_view match_uuid, domain::PlayerId player_id) {
  const auto limit = transaction.Exec(
      "SELECT `MaxQuantity` FROM `ItemTemplates` WHERE `ItemId`=101164 AND `IsGrantable` AND "
      "`DurationPolicy`='permanent' AND `StackPolicy`='merge_quantity'");
  if (limit.size() != 1U) return;  // 发布配置缺失时债权仍保留，不伪造道具。
  const auto existing = transaction.Exec(
      "SELECT `ItemInstanceId`,`Quantity`,`ExpiresAt` FROM `PlayerItems` WHERE `PlayerId`=$1 AND "
      "`ItemId`=101164 FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  std::int64_t instance = 0;
  if (existing.empty()) {
    const auto inserted = transaction.Exec(
        "INSERT INTO `PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`SourceCode`) SELECT"
        " $2,101164,1,`TicketAcquiredAt`,'user_host_refund' FROM `UserHostPrizes` WHERE "
        "`MatchId`=$1 RETURNING `ItemInstanceId`",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                              player_id.Value()});
    instance = inserted.OneRow()[0].As<std::int64_t>();
  } else {
    if (!existing[0][2].IsNull() ||
        existing[0][1].As<std::int64_t>() >= limit[0][0].As<std::int64_t>())
      return;
    instance = existing[0][0].As<std::int64_t>();
    transaction.Exec(
        "UPDATE `PlayerItems` SET `Quantity`=`Quantity`+1 WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, instance});
  }
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
      " "
      "VALUES($2,101164,$3,1,'user_host_refund',BIN_TO_UUID(UNHEX(MD5(CONCAT($1,':user-host-refund-ticket')))))",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                            player_id.Value(), instance});
  transaction.Exec(
      "UPDATE `UserHostPrizeClaims` SET `TicketCredited`=true WHERE `MatchId`=$1 AND "
      "`PlayerId`=$2",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                            player_id.Value()});
}

}  // namespace

std::optional<application::ServiceError> FundUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::MatchStartCommand& command) {
  if (!command.user_host_prize) return std::nullopt;
  const auto& terms = *command.user_host_prize;
  const auto match_uuid = command.match_id.ToUuidString();
  LockProfile(transaction, terms.sponsor_player_id);
  const auto wallet = transaction.Exec(
      "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`='lucci' FOR "
      "UPDATE",
      bf::infrastructure::mysql::Parameters{transaction,
                                            terms.sponsor_player_id.Value()});
  if (wallet.size() != 1U || wallet[0][0].As<std::int64_t>() < terms.amount)
    return application::ServiceError{
        .code = application::ErrorCode::kInsufficientBalance,
        .safe_message = "主办者奖金余额不足"};
  const auto ticket = transaction.Exec(
      "SELECT i.`ItemInstanceId`,i.`Quantity` FROM `PlayerItems` i JOIN `ItemTemplates` t "
      "USING(`ItemId`) WHERE i.`PlayerId`=$1 AND i.`ItemId`=101164 AND i.`ExpiresAt` IS NULL AND "
      "t.`IsGrantable` AND t.`DurationPolicy`='permanent' AND t.`StackPolicy`='merge_quantity' "
      "FOR UPDATE OF i",
      bf::infrastructure::mysql::Parameters{transaction,
                                            terms.sponsor_player_id.Value()});
  if (ticket.size() != 1U)
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidState,
        .safe_message = "主办者没有可用赏金卡"};
  const auto instance = ticket[0][0].As<std::int64_t>();
  transaction.Exec(
      "INSERT INTO "
      "`UserHostPrizes`(`MatchId`,`SponsorPlayerId`,`Amount`,`FeeAmount`,`PolicyVersion`,`TicketAcquiredAt`)"
      " SELECT $1,$2,$3,$3 DIV 10,$4,`AcquiredAt` FROM `PlayerItems` WHERE `ItemInstanceId`=$5",
      bf::infrastructure::mysql::Parameters{
          transaction, match_uuid, terms.sponsor_player_id.Value(),
          terms.amount, terms.policy_version, instance});
  const auto before = wallet[0][0].As<std::int64_t>();
  transaction.Exec(
      "UPDATE `PlayerWallets` SET `Balance`=`Balance`-$2,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
      "`PlayerId`=$1 AND `CurrencyCode`='lucci'",
      bf::infrastructure::mysql::Parameters{
          transaction, terms.sponsor_player_id.Value(), terms.amount});
  transaction.Exec(
      "INSERT INTO "
      "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
      " "
      "VALUES($2,'lucci',-$3,$4,$4-$3,'user_host_funding',BIN_TO_UUID(UNHEX(MD5(CONCAT($1,':user-host-funding')))))",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                            terms.sponsor_player_id.Value(),
                                            terms.amount, before});
  transaction.Exec(
      "INSERT INTO "
      "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
      " "
      "VALUES($2,101164,$3,-1,'user_host_funding',BIN_TO_UUID(UNHEX(MD5(CONCAT($1,':user-host-funding')))))",
      bf::infrastructure::mysql::Parameters{
          transaction, match_uuid, terms.sponsor_player_id.Value(), instance});
  if (ticket[0][1].As<std::int64_t>() == 1) {
    transaction.Exec(
        "DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, instance});
    transaction.Exec(
        "DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, instance});
  } else {
    transaction.Exec(
        "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, instance});
  }
  return std::nullopt;
}

std::optional<domain::PlayerId> UserHostSponsor(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid) {
  const auto row = transaction.Exec(
      "SELECT `SponsorPlayerId` FROM `UserHostPrizes` WHERE `MatchId`=$1",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid});
  if (row.empty()) return std::nullopt;
  return domain::PlayerId{row[0][0].As<std::uint64_t>()};
}

std::optional<application::UserHostPrizeReceipt> ReadUserHostPrizeReceipt(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid) {
  // 重放没有资产写锁，故所有债权与钱包必须在一条语句的MVCC快照中读取。
  const auto rows = transaction.Exec(
      "WITH prize AS (SELECT * FROM `UserHostPrizes` WHERE `MatchId`=$1), people AS (SELECT "
      "`PlayerId` FROM `MatchParticipants` WHERE `MatchId`=$1 UNION SELECT `SponsorPlayerId` FROM"
      " prize UNION SELECT `PlayerId` FROM `UserHostPrizeClaims` WHERE `MatchId`=$1) SELECT "
      "p.`SponsorPlayerId`,p.`Amount`,p.`FeeAmount`,p.`PolicyVersion`,p.`PrizeStatus`,u.`PlayerId`,l.`Balance`,b.`Balance`,t.`Balance`,c.`PlayerId`,c.`ClaimKind`,c.`Amount`,c.`CreditedAmount`,c.`TicketDue`,c.`TicketCredited`,(SELECT"
      " coalesce(sum(w.`AmountDelta`),0) FROM `UserHostPrizePayments` x JOIN `WalletTransactions`"
      " w USING(`WalletTransactionId`) WHERE x.`MatchId`=p.`MatchId` AND "
      "x.`PlayerId`=u.`PlayerId` AND w.`PlayerId`=x.`PlayerId` AND w.`CurrencyCode`='lucci' AND "
      "w.`AmountDelta`>0 AND w.`ReasonCode`=CONCAT('user_host_',c.`ClaimKind`)),EXISTS(SELECT 1 FROM "
      "`InventoryTransactions` i WHERE i.`PlayerId`=u.`PlayerId` AND i.`ItemId`=101164 AND "
      "i.`QuantityDelta`=1 AND i.`ReasonCode`='user_host_refund' AND "
      "i.`BusinessId`=BIN_TO_UUID(UNHEX(MD5(CONCAT(p.`MatchId`,':user-host-refund-ticket'))))) "
      "FROM prize p CROSS JOIN people u LEFT JOIN `PlayerWallets` l ON l.`PlayerId`=u.`PlayerId` "
      "AND l.`CurrencyCode`='lucci' LEFT JOIN `PlayerWallets` b ON b.`PlayerId`=u.`PlayerId` AND "
      "b.`CurrencyCode`='bubble_cash' LEFT JOIN `PlayerWallets` t ON t.`PlayerId`=u.`PlayerId` "
      "AND t.`CurrencyCode`='treasure_bubble' LEFT JOIN `UserHostPrizeClaims` c ON "
      "c.`MatchId`=p.`MatchId` AND c.`PlayerId`=u.`PlayerId` ORDER BY u.`PlayerId`",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid});
  if (rows.empty()) return std::nullopt;
  const auto status = rows[0][4].As<std::string>();
  if (status != "distributed" && status != "refunded")
    throw std::runtime_error{"主办奖金尚未关闭，不能生成完成回执"};
  application::UserHostPrizeReceipt result{
      .sponsor_player_id = domain::PlayerId{rows[0][0].As<std::uint64_t>()},
      .amount = rows[0][1].As<std::int64_t>(),
      .fee_amount = rows[0][2].As<std::int64_t>(),
      .policy_version = rows[0][3].As<std::uint32_t>(),
      .refunded = status == "refunded"};
  for (const auto& row : rows) {
    const auto player = domain::PlayerId{row[5].As<std::uint64_t>()};
    result.wallets.push_back(
        {.player_id = player,
         .wallets = {.lucci = row[6].As<std::int64_t>(),
                     .bubble_cash = row[7].As<std::int64_t>(),
                     .treasure_bubble = row[8].As<std::int64_t>()}});
    if (row[9].IsNull()) continue;
    if (row[10].As<std::string>() != (result.refunded ? "refund" : "payout") ||
        row[12].As<std::int64_t>() != row[15].As<std::int64_t>() ||
        row[14].As<bool>() != row[16].As<bool>())
      throw std::runtime_error{"主办奖金债权状态与真实金币或库存流水不一致"};
    result.claims.push_back({.player_id = player,
                             .amount = row[11].As<std::int64_t>(),
                             .credited_amount = row[12].As<std::int64_t>(),
                             .ticket_due = row[13].As<bool>(),
                             .ticket_credited = row[14].As<bool>()});
  }
  return result;
}

void ClaimUserHostPrizes(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id) {
  // 查询用于避开普通菜单上的写锁；新增债权可在下一次刷新领取。
  const auto pending = transaction.Exec(
      "SELECT 1 FROM `UserHostPrizeClaims` WHERE `PlayerId`=$1 AND (`CreditedAmount`<`Amount` OR "
      "(`TicketDue` AND NOT `TicketCredited`)) LIMIT 1",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  if (pending.empty()) return;
  LockProfile(transaction, player_id);
  const auto wallet = transaction.Exec(
      "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`='lucci' FOR "
      "UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  auto balance = wallet.OneRow()[0].As<std::int64_t>();
  if (balance < 0 || balance > domain::kNativeSignedDwordMaximum)
    throw std::runtime_error{"主办比赛钱包超出原生范围"};
  const auto claims = transaction.Exec(
      "SELECT `MatchId`,`Amount`,`CreditedAmount`,`ClaimKind`,`TicketDue`,`TicketCredited` FROM "
      "`UserHostPrizeClaims` WHERE `PlayerId`=$1 AND (`CreditedAmount`<`Amount` OR (`TicketDue` "
      "AND NOT `TicketCredited`)) ORDER BY `MatchId` FOR UPDATE",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
  for (const auto& claim : claims) {
    const auto match_uuid = claim[0].As<std::string>();
    const auto credited = claim[2].As<std::int64_t>();
    const auto amount = std::min(claim[1].As<std::int64_t>() - credited,
                                 domain::kNativeSignedDwordMaximum - balance);
    if (amount > 0) {
      const auto payment = transaction.Exec(
          "INSERT INTO "
          "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
          " "
          "VALUES($2,'lucci',$3,$4,$4+$3,CONCAT('user_host_',$5),BIN_TO_UUID(UNHEX(MD5(CONCAT($1,':user-host-credit:',$2,':',$6)))))"
          " RETURNING `WalletTransactionId`",
          bf::infrastructure::mysql::Parameters{
              transaction, match_uuid, player_id.Value(), amount, balance,
              claim[3].As<std::string>(), credited});
      transaction.Exec(
          "INSERT INTO `UserHostPrizePayments`(`WalletTransactionId`,`MatchId`,`PlayerId`) "
          "VALUES($1,$2,$3)",
          bf::infrastructure::mysql::Parameters{
              transaction, payment.OneRow()[0].As<std::int64_t>(), match_uuid,
              player_id.Value()});
      transaction.Exec(
          "UPDATE `UserHostPrizeClaims` SET `CreditedAmount`=`CreditedAmount`+$3 WHERE `MatchId`=$1 "
          "AND `PlayerId`=$2",
          bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                                player_id.Value(), amount});
      balance += amount;
    }
    if (claim[4].As<bool>() && !claim[5].As<bool>())
      RefundTicket(transaction, match_uuid, player_id);
  }
  transaction.Exec(
      "UPDATE `PlayerWallets` SET `Balance`=$2,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
      "`PlayerId`=$1 AND `CurrencyCode`='lucci'",
      bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                            balance});
}

void RefundUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid) {
  const auto closed = transaction.Exec(
      "UPDATE `UserHostPrizes` SET `PrizeStatus`='refunded',`ClosedAt`=CURRENT_TIMESTAMP(6) WHERE"
      " `MatchId`=$1 AND `PrizeStatus`='funded' RETURNING `SponsorPlayerId`,`Amount`",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid});
  if (closed.empty()) return;
  const auto sponsor = domain::PlayerId{closed[0][0].As<std::uint64_t>()};
  transaction.Exec(
      "INSERT INTO `UserHostPrizeClaims`(`MatchId`,`PlayerId`,`ClaimKind`,`Amount`,`TicketDue`) "
      "VALUES($1,$2,'refund',$3,true)",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                            sponsor.Value(),
                                            closed[0][1].As<std::int64_t>()});
  ClaimUserHostPrizes(transaction, sponsor);
}

std::optional<application::ServiceError> SettleUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::MatchSettlementCommand& command) {
  const auto match_uuid = command.match_id.ToUuidString();
  const auto prize = transaction.Exec(
      "SELECT `Amount`-`FeeAmount` FROM `UserHostPrizes` WHERE `MatchId`=$1 AND "
      "`PrizeStatus`='funded'",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid});
  if (prize.empty()) return std::nullopt;
  std::vector<domain::PlayerId> winners;
  std::set<std::uint8_t> winning_teams;
  if (command.completion_kind == application::MatchCompletionKind::kCompleted) {
    for (const auto& participant : command.participants) {
      if (!participant.departed &&
          participant.outcome == domain::MatchOutcome::kWin) {
        winners.push_back(participant.player_id);
        winning_teams.insert(participant.team_code);
      }
    }
  }
  if (winning_teams.size() > 1U)
    return application::ServiceError{
        .code = application::ErrorCode::kConflict,
        .safe_message = "主办比赛终局包含多个胜队"};
  if (winners.empty()) {
    RefundUserHostPrize(transaction, match_uuid);
    return std::nullopt;
  }
  std::ranges::sort(winners);
  const auto net = prize[0][0].As<std::int64_t>();
  const auto count = static_cast<std::int64_t>(winners.size());
  auto remainder = net % count;
  for (const auto player_id : winners) {
    const auto amount = net / count + (remainder > 0 ? 1 : 0);
    if (remainder > 0) --remainder;
    transaction.Exec(
        "INSERT INTO `UserHostPrizeClaims`(`MatchId`,`PlayerId`,`ClaimKind`,`Amount`) "
        "VALUES($1,$2,'payout',$3)",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                              player_id.Value(), amount});
    ClaimUserHostPrizes(transaction, player_id);
  }
  transaction.Exec(
      "UPDATE `UserHostPrizes` SET `PrizeStatus`='distributed',`ClosedAt`=CURRENT_TIMESTAMP(6) "
      "WHERE `MatchId`=$1 AND `PrizeStatus`='funded'",
      bf::infrastructure::mysql::Parameters{transaction, match_uuid});
  return std::nullopt;
}

}  // namespace bf::infrastructure::mysql
