#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_AUTHENTICATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_AUTHENTICATION_H_

/**
 * @file launcher_authentication.h
 * @brief 定义启动器注册、密码登录与五分钟票据签发用例。
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "application/error.h"
#include "application/identifier.h"
#include "features/authentication/account_credentials.h"
#include "features/authentication/authentication.h"
#include "features/authentication/credential_crypto.h"

namespace bf::application {

/** @brief 新注册账号写入数据库的 PBKDF2 凭据和初始玩家状态。 */
struct NewAccountSeed final {
  domain::AccountName account_name;     ///< 已校验的展示名与 ASCII 小写唯一键。
  CredentialSalt salt{};                ///< 16 字节操作系统随机盐。
  PasswordDigest digest{};              ///< 600000 次 PBKDF2-HMAC-SHA256 结果。
  std::uint32_t iterations{0U};         ///< 与摘要匹配的派生迭代次数。
  std::int64_t initial_lucci{100'000};  ///< 兼容旧注册流程的初始金币。
  std::int64_t initial_bubble_cash{10'000};  ///< 兼容旧注册流程的初始点券。
  std::int64_t initial_treasure_bubble{
      100};  ///< build 752 扩展规则发布的抽取资源初值。
};

/** @brief 成功注册并创建一对一玩家资料后返回的强类型标识。 */
struct RegisteredAccount final {
  domain::AccountId account_id;  ///< 新账号标识。
  domain::PlayerId player_id;    ///< 同事务创建的未建角玩家资料标识。
  std::string account_name;      ///< 数据库实际保存的展示账号名。
};

/**
 * @brief 注册账号的数据库事务端口。
 * @note create_account
 * 必须在同一事务创建账号、凭据、未建角玩家资料、初始进度以及三种钱包；
 *       任一唯一冲突或写入失败均不得留下部分账号。
 */
class RegisterAccountTransaction {
 public:
  virtual ~RegisterAccountTransaction() = default;

  /** @brief 写入完整新账号聚合；账号 key 已存在时返回冲突。 */
  [[nodiscard]] virtual ServiceResult<RegisteredAccount> CreateAccount(
      const NewAccountSeed& seed) = 0;

  /** @brief 提交完整注册；提交失败时调用方不能假定账号不存在。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 注册事务工厂，由单个数据库工作线程和独占连接实现。 */
class RegisterAccountUnitOfWork {
 public:
  virtual ~RegisterAccountUnitOfWork() = default;

  /** @brief 开始短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<
      std::unique_ptr<RegisterAccountTransaction>>
  Begin() = 0;
};

/** @brief 启动器注册命令；密码只允许在同步调用期间借用，禁止记录。 */
struct RegisterAccountCommand final {
  std::string account_name;  ///< 用户输入的账号展示名。
  std::string password;      ///< 用户输入的 UTF-8 密码正文。
};

/**
 * @brief 校验并注册账号，同时建立进入首次选角流程所需的初始数据。
 * @note 随机盐和 PBKDF2
 * 在事务开始前完成；调用方必须在有界认证工作池执行本同步用例。
 */
class RegisterAccountService final {
 public:
  /** @brief 注入密码学随机源、PBKDF2 和注册事务端口。 */
  RegisterAccountService(const CryptographicRandomSource& random,
                         const Pbkdf2PasswordHasher& password_hasher,
                         RegisterAccountUnitOfWork& unit_of_work) noexcept;

  /** @brief 完整注册；成功前所有持久化写入必须由一次 commit 原子发布。 */
  [[nodiscard]] ServiceResult<RegisteredAccount> RegisterAccount(
      const RegisterAccountCommand& command) const;

 private:
  const CryptographicRandomSource& random_;
  const Pbkdf2PasswordHasher& password_hasher_;
  RegisterAccountUnitOfWork& unit_of_work_;
};

/** @brief 数据库凭据读取端口返回的账号状态。 */
enum class AccountStatus : std::uint8_t {
  kActive,
  kDisabled,
  kLocked,
};

/** @brief 密码校验前从数据库取出的只读账号凭据快照。 */
struct StoredAccountCredential final {
  domain::AccountId account_id;  ///< 凭据所属账号。
  std::string account_name;      ///< 成功响应使用的数据库展示名。
  std::string login_key;         ///< 查询命中的规范化账号键。
  AccountStatus status{AccountStatus::kDisabled};  ///< 登录时再次执行状态校验。
  std::string algorithm;                           ///< 必须为 `pbkdf2_sha256`。
  CredentialSalt salt{};         ///< 数据库中的固定 16 字节盐。
  PasswordDigest digest{};       ///< 数据库中的固定 32 字节摘要。
  std::uint32_t iterations{0U};  ///< 有界历史迭代次数。
};

/** @brief 在不持有写事务时读取登录凭据的数据库端口。 */
class AccountCredentialReader {
 public:
  virtual ~AccountCredentialReader() = default;

  /** @brief 按 ASCII 小写键读取一行；账号不存在返回空值，存储故障返回错误。 */
  [[nodiscard]] virtual ServiceResult<std::optional<StoredAccountCredential>>
  FindByLoginKey(std::string_view login_key) = 0;
};

/** @brief 登录签发事务端口；只接收票据摘要，不接收原始票据。 */
class IssueLoginTicketTransaction {
 public:
  virtual ~IssueLoginTicketTransaction() = default;

  /**
   * @brief 锁账号并在仍为 active 时撤销旧票据、写入新摘要。
   * @note expires_at 必须晚于
   * now；同账号并发登录最终只保留最后成功提交的一张票据。
   */
  [[nodiscard]] virtual std::optional<ServiceError> ReplaceTickets(
      domain::AccountId account_id, const Sha256Digest& ticket_digest,
      std::chrono::system_clock::time_point now,
      std::chrono::system_clock::time_point expires_at,
      std::string_view machine_code = {}) = 0;

  /** @brief 提交票据替换；提交结果未知时只能按摘要或账号重新查询。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 登录票据签发事务工厂，由数据库工作线程实现。 */
class IssueLoginTicketUnitOfWork {
 public:
  virtual ~IssueLoginTicketUnitOfWork() = default;

  /** @brief 开始短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<
      std::unique_ptr<IssueLoginTicketTransaction>>
  Begin() = 0;
};

/** @brief 启动器密码登录命令；失败信息不得区分账号不存在和密码错误。 */
struct LoginWithPasswordCommand final {
  std::string account_name;  ///< 用户输入账号名，按兼容规则生成 lookup key。
  std::string password;      ///< 用户输入密码，只用于本次 PBKDF2 校验。
  std::string machine_code;  ///< Legacy launcher field; accepted and ignored.
};

/** @brief 密码登录成功后返回给启动器的敏感票据及到期时间。 */
struct LauncherLoginResult final {
  domain::AccountId account_id;  ///< 票据绑定账号。
  std::string account_name;      ///< 数据库保存的账号展示名。
  std::string ticket;            ///< 必须只返回客户端，不写日志或数据库。
  std::chrono::system_clock::time_point expires_at;  ///< 绝对 UTC 到期时间。
};

/** Trusted server-side result of password verification; never populated from a
 * client account id. */
struct PasswordAuthenticatedAccount final {
  domain::AccountId account_id;
  std::string account_name;
  std::string machine_code;
};

/**
 * @brief 校验密码并签发五分钟、单账号替换、单次消费的游戏登录票据。
 * @note 查询不持有写事务；PBKDF2、随机生成和 SHA-256 均在签发事务开始前完成。
 * @note 未知账号仍使用固定假凭据执行一次 PBKDF2，降低账号枚举的时序差异。
 */
class LoginWithPasswordService final {
 public:
  /** @brief 注入所有密码学、时钟、读取和签发端口；生命周期必须覆盖本服务。 */
  LoginWithPasswordService(const Pbkdf2PasswordHasher& password_hasher,
                           const CryptographicRandomSource& random,
                           const Sha256Hasher& ticket_hasher,
                           const SystemClock& clock,
                           AccountCredentialReader& credential_reader,
                           IssueLoginTicketUnitOfWork& unit_of_work) noexcept;

  /** @brief 登录并签发票据；未知账号、错误密码和非 active 状态统一返回未认证。
   */
  [[nodiscard]] ServiceResult<LauncherLoginResult> Login(
      const LoginWithPasswordCommand& command) const;
  [[nodiscard]] ServiceResult<PasswordAuthenticatedAccount> VerifyPassword(
      const LoginWithPasswordCommand& command) const;
  [[nodiscard]] ServiceResult<LauncherLoginResult> IssueTicket(
      const PasswordAuthenticatedAccount& authenticated) const;

 private:
  const Pbkdf2PasswordHasher& password_hasher_;
  const CryptographicRandomSource& random_;
  const Sha256Hasher& ticket_hasher_;
  const SystemClock& clock_;
  AccountCredentialReader& credential_reader_;
  IssueLoginTicketUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_LAUNCHER_AUTHENTICATION_H_
