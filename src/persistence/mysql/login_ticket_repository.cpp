#include "persistence/mysql/login_ticket_repository.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "features/battle/match_reward.h"
#include "features/characters/character.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = retryable};
}

std::int64_t UnixMicroseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             value.time_since_epoch())
      .count();
}

bf::infrastructure::mysql::BytesView DigestView(
    const application::Sha256Digest& digest) {
  return {reinterpret_cast<const std::byte*>(digest.data()), digest.size()};
}

class MySqlLoginTicketTransaction final
    : public application::LoginTicketTransaction {
 public:
  explicit MySqlLoginTicketTransaction(MySqlConnectionPool::Lease lease,
                                       std::uint32_t region_id)
      : lease_(std::move(lease)),
        transaction_(lease_.Connection()),
        region_id_(region_id) {}

  application::ServiceResult<application::AuthenticatedPlayerSnapshot>
  ConsumeTicket(const application::Sha256Digest& ticket_digest,
                std::chrono::system_clock::time_point now) override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "登录票据事务已结束");
    }
    try {
      transaction_.Exec(
          "SELECT LockKey FROM TransactionLocks WHERE LockKey='moderation' FOR SHARE");
      const auto params = Parameters{transaction_, DigestView(ticket_digest),
                                     UnixMicroseconds(now), region_id_};
      const auto rows = transaction_.Exec(
          "SELECT account.AccountId, profile.PlayerId, account.LoginName, profile.Nickname, "
          "profile.IsCharacterCreated, profile.InitialFighterId, NULL "
          "FROM LoginTickets AS ticket JOIN Accounts AS account ON ticket.AccountId=account.AccountId "
          "JOIN PlayerProfiles AS profile ON profile.AccountId=account.AccountId "
          "WHERE ticket.TicketDigest=$1 AND ticket.ConsumedAt IS NULL AND ticket.RegionId=$3 "
          "AND ticket.ExpiresAt>FROM_UNIXTIME($2/1000000.0) AND account.AccountStatus='active' "
          "AND NOT AccountBanActive(account.AccountId,NULL) FOR UPDATE",
          params);
      if (!rows.empty() &&
          transaction_
                  .Exec(
                      "UPDATE LoginTickets SET ConsumedAt=FROM_UNIXTIME($2/1000000.0) "
                      "WHERE TicketDigest=$1 AND ConsumedAt IS NULL",
                      params)
                  .AffectedRows() != 1) {
        return MakeError(application::ErrorCode::kInternal,
                         "登录票据消费状态发生变化");
      }
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kUnauthenticated,
                         "登录票据无效、已过期或已使用");
      }
      if (rows.size() != 1U) {
        return MakeError(application::ErrorCode::kInternal,
                         "登录票据联表返回非唯一资料");
      }
      const auto account_value = rows[0][0].As<std::int64_t>();
      const auto player_value = rows[0][1].As<std::int64_t>();
      if (account_value <= 0 || player_value <= 0) {
        return MakeError(application::ErrorCode::kInternal,
                         "登录票据关联了无效标识");
      }
      const auto character_created = rows[0][4].As<bool>();
      std::optional<domain::InitialFighter> fighter;
      if (character_created) {
        if (rows[0][5].IsNull()) {
          return MakeError(application::ErrorCode::kInternal,
                           "已建角资料缺少初始角色");
        }
        fighter =
            domain::InitialFighterFromProtocol(rows[0][5].As<std::uint32_t>());
        if (!fighter.has_value()) {
          return MakeError(application::ErrorCode::kInternal,
                           "已建角资料包含未发布初始角色");
        }
      } else if (!rows[0][5].IsNull()) {
        return MakeError(application::ErrorCode::kInternal,
                         "未建角资料错误保存了初始角色");
      }
      return application::AuthenticatedPlayerSnapshot{
          .account_id =
              domain::AccountId{static_cast<std::uint64_t>(account_value)},
          .player_id =
              domain::PlayerId{static_cast<std::uint64_t>(player_value)},
          .account_name = rows[0][2].As<std::string>(),
          .nickname = rows[0][3].As<std::string>(),
          .is_character_created = character_created,
          .initial_fighter = fighter,
          .machine_code = rows[0][6].IsNull() ? std::string{}
                                              : rows[0][6].As<std::string>(),
      };
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "消费登录票据时 MySQL 连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      spdlog::error("登录票据消费 SQL 失败，sqlstate={}，detail={}",
                    error.Sqlstate(), error.what());
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "MySQL 拒绝登录票据消费", false);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取登录票据资料时发生未分类错误", false);
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "登录票据事务已结束");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交登录票据消费时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "MySQL 拒绝提交登录票据消费", false);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交登录票据事务时发生未分类错误", false);
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  std::uint32_t region_id_;
  bool completed_{false};
};

}  // namespace

void MySqlDailyLoginRewardGranter::Log(bool success,
                                       const std::string& message) const {
  if (logger_ != nullptr) {
    logger_->Log(
        success ? application::LogLevel::kInfo : application::LogLevel::kError,
        message);
  } else {
    spdlog::log(success ? spdlog::level::info : spdlog::level::err, "{}",
                message);
  }
}

std::optional<application::ServiceError> MySqlDailyLoginRewardGranter::Grant(
    domain::PlayerId player_id, std::chrono::system_clock::time_point now) {
  const auto result = GrantWithResult(player_id, now);
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return *error;
  return std::nullopt;
}

application::ServiceResult<application::DailyLoginRewardResult>
MySqlDailyLoginRewardGranter::GrantWithResult(
    domain::PlayerId player_id, std::chrono::system_clock::time_point now) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    Log(false, "daily_login_reward failed player=" +
                   std::to_string(player_id.Value()) +
                   " reason=connection_unavailable");
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    // Serialize both unique keys (player/day and business UUID) before
    // inserting. ON CONFLICT(player_id,reward_date) alone can race on the other
    // unique index.
    const auto player = transaction.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (player.empty()) throw std::runtime_error("missing_player");
    const auto claim = transaction.Exec(
        "INSERT INTO DailyLoginRewards (PlayerId,RewardDate,BusinessId,ClaimedAt) VALUES($1, "
        "DATE(CONVERT_TZ(FROM_UNIXTIME($2/1000000.0),'+00:00','+08:00')), "
        "BIN_TO_UUID(UNHEX(MD5(CONCAT('daily_login_reward:',$1,':', "
        "DATE(CONVERT_TZ(FROM_UNIXTIME($2/1000000.0),'+00:00','+08:00')))))), "
        "FROM_UNIXTIME($2/1000000.0)) ON CONFLICT(PlayerId,RewardDate) DO NOTHING RETURNING "
        "BusinessId,RewardDate",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                              UnixMicroseconds(now)});
    if (claim.empty()) {
      transaction.Commit();
      return application::DailyLoginRewardResult{};
    }
    const auto wallets = transaction.Exec(
        "SELECT `CurrencyCode`,`Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND "
        "`CurrencyCode` IN ('bubble_cash','lucci') ORDER BY `CurrencyCode` FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player_id.Value()});
    if (wallets.size() != 2U) {
      throw std::runtime_error("missing_wallet");
    }
    constexpr std::int64_t amount = 99'999;
    for (const auto& wallet : wallets) {
      const auto currency = wallet[0].As<std::string>();
      const auto before = wallet[1].As<std::int64_t>();
      if (before < 0 || before > domain::kNativeSignedDwordMaximum - amount) {
        throw std::runtime_error("wallet_limit");
      }
      transaction.Exec(
          "UPDATE `PlayerWallets` SET `Balance`=$3,`UpdatedAt`=FROM_UNIXTIME($4 / 1000000.0) WHERE "
          "`PlayerId`=$1 AND `CurrencyCode`=$2",
          bf::infrastructure::mysql::Parameters{transaction, player_id.Value(),
                                                currency, before + amount,
                                                UnixMicroseconds(now)});
      transaction.Exec(
          "INSERT INTO "
          "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`,`OccurredAt`)"
          " VALUES($1,$2,$3,$4,$5,'daily_login_reward',$6,FROM_UNIXTIME($7 / 1000000.0))",
          bf::infrastructure::mysql::Parameters{
              transaction, player_id.Value(), currency, amount, before,
              before + amount, claim[0][0].As<std::string>(),
              UnixMicroseconds(now)});
    }
    const auto contribution = transaction.Exec(
        "SELECT `ClanId`,`Points` FROM `ClanActivityGrants` WHERE `SourceKind`='login' AND "
        "`SourceId`=$1 AND `PlayerId`=$2",
        bf::infrastructure::mysql::Parameters{
            transaction, claim[0][0].As<std::string>(), player_id.Value()});
    application::DailyLoginRewardResult result;
    if (!contribution.empty()) {
      result.clan_id = contribution[0][0].As<std::uint32_t>();
      result.clan_points = contribution[0][1].As<std::uint32_t>();
    }
    const auto date = claim[0][1].As<std::string>();
    transaction.Commit();
    Log(true, "daily_login_reward granted player=" +
                  std::to_string(player_id.Value()) + " date=" + date +
                  " bubble_cash=99999 lucci=99999");
    return result;
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    Log(false, "daily_login_reward failed player=" +
                   std::to_string(player_id.Value()) +
                   " sqlstate=" + std::string{error.Sqlstate()});
  } catch (const std::exception&) {
    Log(false, "daily_login_reward failed player=" +
                   std::to_string(player_id.Value()) +
                   " reason=wallet_or_storage_unavailable");
  }
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   "每日登录奖励未提交，下次登录重试", true);
}

MySqlLoginTicketUnitOfWork::MySqlLoginTicketUnitOfWork(
    MySqlConnectionPool& pool, std::uint32_t region_id) noexcept
    : region_id_(region_id), pool_(pool) {}

application::ServiceResult<std::unique_ptr<application::LoginTicketTransaction>>
MySqlLoginTicketUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::LoginTicketTransaction>{
        std::make_unique<MySqlLoginTicketTransaction>(std::move(lease),
                                                      region_id_)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始登录票据事务", true);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "开始登录票据事务时发生未分类错误", false);
  }
}

}  // namespace bf::infrastructure::mysql
