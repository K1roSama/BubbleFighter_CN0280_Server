#include "persistence/mysql/launcher_authentication_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

application::ServiceError StorageError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

std::int64_t UnixMicroseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             value.time_since_epoch())
      .count();
}

template <std::size_t Size>
bf::infrastructure::mysql::BytesView AsBytesView(
    const std::array<std::uint8_t, Size>& value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

template <std::size_t Size, typename Field>
std::optional<std::array<std::uint8_t, Size>> FixedBytes(const Field& field) {
  const auto bytes = field.As<bf::infrastructure::mysql::Bytes>();
  if (bytes.size() != Size) {
    return std::nullopt;
  }
  std::array<std::uint8_t, Size> result{};
  std::transform(
      bytes.begin(), bytes.end(), result.begin(),
      [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
  return result;
}

std::optional<application::AccountStatus> AccountStatus(
    std::string_view value) noexcept {
  if (value == "active") {
    return application::AccountStatus::kActive;
  }
  if (value == "disabled") {
    return application::AccountStatus::kDisabled;
  }
  if (value == "locked") {
    return application::AccountStatus::kLocked;
  }
  return std::nullopt;
}

class MySqlRegisterAccountTransaction final
    : public application::RegisterAccountTransaction {
 public:
  explicit MySqlRegisterAccountTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::RegisteredAccount> CreateAccount(
      const application::NewAccountSeed& seed) override {
    if (completed_ || created_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "账号注册事务已使用");
    }
    try {
      const auto progression_rules = transaction_.Exec(
          "SELECT `ConfigurationVersionId` FROM `ConfigurationVersions` WHERE "
          "`ConfigurationKind`='progression_rules' AND `ReleaseStatus`='published' ORDER BY "
          "`ConfigurationVersionId` DESC LIMIT 1");
      if (progression_rules.size() != 1U) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "尚未发布新账号可使用的成长规则");
      }

      const auto accounts = transaction_.Exec(
          "INSERT INTO `Accounts`(`LoginName`,`LoginKey`,`AccountStatus`) VALUES($1,$2,'active') "
          "RETURNING `AccountId`,`LoginName`",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                seed.account_name.DisplayName(),
                                                seed.account_name.Key()});
      if (accounts.size() != 1U) {
        return MakeError(application::ErrorCode::kInternal,
                         "创建账号没有返回唯一主键");
      }
      const auto account_id = accounts[0][0].As<std::uint64_t>();
      transaction_.Exec(
          "INSERT INTO `AccountCredentials`(`AccountId`,`Algorithm`,`Salt`,`Digest`,`Iterations`) "
          "VALUES($1,$2,$3,$4,$5)",
          bf::infrastructure::mysql::Parameters{
              transaction_, account_id, application::kPbkdf2Sha256Algorithm,
              AsBytesView(seed.salt), AsBytesView(seed.digest),
              seed.iterations});
      const auto profiles = transaction_.Exec(
          "INSERT INTO "
          "`PlayerProfiles`(`AccountId`,`Nickname`,`NicknameKey`,`IsCharacterCreated`,`InitialFighterId`)"
          " VALUES($1,$2,NULL,false,NULL) RETURNING `PlayerId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, account_id, seed.account_name.DisplayName()});
      if (profiles.size() != 1U) {
        return MakeError(application::ErrorCode::kInternal,
                         "创建玩家资料没有返回唯一主键");
      }
      const auto player_id = profiles[0][0].As<std::uint64_t>();
      transaction_.Exec(
          "INSERT INTO `PlayerProgressions`(`PlayerId`,`Experience`,`Rating`,`RulesVersionId`) "
          "VALUES($1,0,1000,$2)",
          bf::infrastructure::mysql::Parameters{
              transaction_, player_id,
              progression_rules[0][0].As<std::int64_t>()});

      const std::array wallets{
          std::pair<std::string_view, std::int64_t>{"lucci",
                                                    seed.initial_lucci},
          std::pair<std::string_view, std::int64_t>{"bubble_cash",
                                                    seed.initial_bubble_cash},
          std::pair<std::string_view, std::int64_t>{
              "treasure_bubble", seed.initial_treasure_bubble},
      };
      for (const auto& [currency, balance] : wallets) {
        transaction_.Exec(
            "INSERT INTO `PlayerWallets`(`PlayerId`,`CurrencyCode`,`Balance`) VALUES($1,$2,$3)",
            bf::infrastructure::mysql::Parameters{transaction_, player_id,
                                                  currency, balance});
        transaction_.Exec(
            "INSERT INTO "
            "`WalletTransactions`(`PlayerId`,`CurrencyCode`,`AmountDelta`,`BalanceBefore`,`BalanceAfter`,`ReasonCode`,`BusinessId`)"
            " "
            "VALUES($1,$2,$3,0,$3,'account_registration',BIN_TO_UUID(UNHEX(MD5(CONCAT('account-registration:',$4,':',$5)))))",
            bf::infrastructure::mysql::Parameters{
                transaction_, player_id, currency, balance,
                std::to_string(player_id), currency});
      }
      created_ = true;
      return application::RegisteredAccount{
          .account_id = domain::AccountId{account_id},
          .player_id = domain::PlayerId{player_id},
          .account_name = accounts[0][1].As<std::string>()};
    } catch (const bf::infrastructure::mysql::UniqueViolation&) {
      return MakeError(application::ErrorCode::kConflict, "账号唯一键已存在");
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "创建账号时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("创建账号", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "创建账号时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_ || !created_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "账号注册事务不能提交");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交账号注册时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      completed_ = true;
      return StorageError("提交账号注册", error);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交账号注册时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool created_{false};
  bool completed_{false};
};

class MySqlIssueLoginTicketTransaction final
    : public application::IssueLoginTicketTransaction {
 public:
  explicit MySqlIssueLoginTicketTransaction(MySqlConnectionPool::Lease lease,
                                            std::uint32_t region_id)
      : lease_(std::move(lease)),
        transaction_(lease_.Connection()),
        region_id_(region_id) {}

  std::optional<application::ServiceError> ReplaceTickets(
      domain::AccountId account_id,
      const application::Sha256Digest& ticket_digest,
      std::chrono::system_clock::time_point now,
      std::chrono::system_clock::time_point expires_at,
      std::string_view machine_code) override {
    machine_code = {};
    if (completed_ || replaced_ || !account_id.IsValid() || expires_at <= now) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "登录票据签发参数或事务状态无效");
    }
    try {
      transaction_.Exec(
          "SELECT LockKey FROM TransactionLocks WHERE LockKey='moderation' FOR SHARE");
      const auto account = transaction_.Exec(
          "SELECT `AccountStatus` FROM `Accounts` WHERE `AccountId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                account_id.Value()});
      if (account.size() != 1U ||
          account[0][0].As<std::string_view>() != "active") {
        return MakeError(application::ErrorCode::kUnauthenticated,
                         "账号或密码错误");
      }
      const auto now_us = UnixMicroseconds(now);
      if (transaction_
              .Exec("SELECT `AccountBanActive`($1,NULLIF($2,''))",
                    bf::infrastructure::mysql::Parameters{
                        transaction_, account_id.Value(), machine_code})[0][0]
              .As<bool>())
        return MakeError(application::ErrorCode::kPermissionDenied,
                         "账号已被封禁，请联系管理员");
      transaction_.Exec(
          "DELETE FROM `LoginTickets` WHERE `AccountId`=$1 OR "
          "`ExpiresAt`<=FROM_UNIXTIME($2/1000000.0)",
          bf::infrastructure::mysql::Parameters{transaction_,
                                                account_id.Value(), now_us});
      transaction_.Exec(
          "INSERT INTO "
          "`LoginTickets`(`TicketDigest`,`AccountId`,`ExpiresAt`,`CreatedAt`,`RegionId`,`MachineCode`)"
          " VALUES($1,$2,FROM_UNIXTIME($3/1000000.0),FROM_UNIXTIME($4/1000000.0),$5,NULLIF($6,''))",
          bf::infrastructure::mysql::Parameters{
              transaction_, AsBytesView(ticket_digest), account_id.Value(),
              UnixMicroseconds(expires_at), now_us, region_id_, machine_code});
      replaced_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "签发登录票据时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("签发登录票据", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "签发登录票据时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_ || !replaced_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "登录票据签发事务不能提交");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交登录票据签发时连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      completed_ = true;
      return StorageError("提交登录票据签发", error);
    } catch (const std::exception&) {
      completed_ = true;
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交登录票据签发时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool replaced_{false};
  std::uint32_t region_id_;
  bool completed_{false};
};

}  // namespace

MySqlRegisterAccountUnitOfWork::MySqlRegisterAccountUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<
    std::unique_ptr<application::RegisterAccountTransaction>>
MySqlRegisterAccountUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::RegisterAccountTransaction>{
        std::make_unique<MySqlRegisterAccountTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始账号注册事务", true);
  }
}

MySqlAccountCredentialReader::MySqlAccountCredentialReader(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::optional<application::StoredAccountCredential>>
MySqlAccountCredentialReader::FindByLoginKey(std::string_view login_key) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto rows = transaction.Exec(
        "SELECT "
        "account.`AccountId`,account.`LoginName`,account.`LoginKey`,account.`AccountStatus`,credential.`Algorithm`,credential.`Salt`,credential.`Digest`,credential.`Iterations`"
        " FROM `Accounts` AS account JOIN `AccountCredentials` AS credential USING(`AccountId`) "
        "WHERE account.`LoginKey`=$1",
        bf::infrastructure::mysql::Parameters{transaction, login_key});
    if (rows.empty()) {
      transaction.Commit();
      return std::optional<application::StoredAccountCredential>{};
    }
    if (rows.size() != 1U) {
      return MakeError(application::ErrorCode::kInternal,
                       "账号凭据查询返回非唯一记录");
    }
    const auto status = AccountStatus(rows[0][3].As<std::string_view>());
    const auto salt =
        FixedBytes<application::CredentialSalt{}.size()>(rows[0][5]);
    const auto digest =
        FixedBytes<application::PasswordDigest{}.size()>(rows[0][6]);
    if (!status.has_value() || !salt.has_value() || !digest.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "账号凭据字段不满足数据库契约");
    }
    application::StoredAccountCredential result{
        .account_id = domain::AccountId{rows[0][0].As<std::uint64_t>()},
        .account_name = rows[0][1].As<std::string>(),
        .login_key = rows[0][2].As<std::string>(),
        .status = *status,
        .algorithm = rows[0][4].As<std::string>(),
        .salt = *salt,
        .digest = *digest,
        .iterations = rows[0][7].As<std::uint32_t>(),
    };
    transaction.Commit();
    return std::optional<application::StoredAccountCredential>{
        std::move(result)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取账号凭据时连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return StorageError("读取账号凭据", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取账号凭据时发生未分类错误");
  }
}

MySqlIssueLoginTicketUnitOfWork::MySqlIssueLoginTicketUnitOfWork(
    MySqlConnectionPool& pool, std::uint32_t region_id) noexcept
    : region_id_(region_id), pool_(pool) {}

application::ServiceResult<
    std::unique_ptr<application::IssueLoginTicketTransaction>>
MySqlIssueLoginTicketUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::IssueLoginTicketTransaction>{
        std::make_unique<MySqlIssueLoginTicketTransaction>(std::move(lease),
                                                           region_id_)};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始登录票据签发事务", true);
  }
}

}  // namespace bf::infrastructure::mysql
