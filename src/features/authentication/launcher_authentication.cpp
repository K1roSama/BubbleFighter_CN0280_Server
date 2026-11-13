#include "features/authentication/launcher_authentication.h"

#include <array>
#include <chrono>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "features/characters/name_policy.h"

namespace bf::application {
namespace {

constexpr std::string_view kLauncherTicketPrefix = "launcher-v2:";
constexpr auto kLoginTicketLifetime = std::chrono::minutes{5};

ServiceError InvalidAccountName(domain::AccountNameValidationError error) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::string{
                          domain::AccountNameValidationErrorMessage(error)}};
}

ServiceError InvalidPassword(domain::PasswordValidationError error) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::string{
                          domain::PasswordValidationErrorMessage(error)}};
}

ServiceError InvalidLogin() {
  return ServiceError{.code = ErrorCode::kUnauthenticated,
                      .safe_message = "账号或密码错误"};
}

ServiceError RegistrationConflict() {
  return ServiceError{.code = ErrorCode::kConflict,
                      .safe_message = "该账号已存在，请更换账号或直接登录"};
}

ServiceError TransactionContractError(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInternal,
      .safe_message =
          std::string{"启动器认证事务返回了无效状态："} + std::string{detail}};
}

bool ValidRegisteredAccount(const RegisteredAccount& result,
                            const domain::AccountName& requested) noexcept {
  return result.account_id.IsValid() && result.player_id.IsValid() &&
         result.account_name == requested.DisplayName();
}

bool ValidStoredCredential(const StoredAccountCredential& credential,
                           std::string_view requested_key) noexcept {
  return credential.account_id.IsValid() && !credential.account_name.empty() &&
         credential.login_key == requested_key &&
         credential.algorithm == kPbkdf2Sha256Algorithm &&
         credential.iterations >= kMinimumAcceptedPbkdf2Iterations &&
         credential.iterations <= kMaximumAcceptedPbkdf2Iterations;
}

std::string EncodeLowerHex(const LoginTicketSecret& secret) {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5',
                                         '6', '7', '8', '9', 'a', 'b',
                                         'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(secret.size() * 2U);
  for (const auto value : secret) {
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0FU]);
  }
  return result;
}

struct PreparedTicket final {
  std::string text;
  Sha256Digest digest{};
  std::chrono::system_clock::time_point issued_at;
  std::chrono::system_clock::time_point expires_at;
};

ServiceResult<PreparedTicket> PrepareTicket(
    const CryptographicRandomSource& random, const Sha256Hasher& hasher,
    const SystemClock& clock) {
  auto secret_result = random.GenerateLoginTicketSecret();
  if (const auto* error = std::get_if<ServiceError>(&secret_result)) {
    return *error;
  }
  const auto secret = std::get<LoginTicketSecret>(secret_result);

  PreparedTicket ticket;
  ticket.text = std::string{kLauncherTicketPrefix} + EncodeLowerHex(secret);
  const auto bytes =
      std::span{reinterpret_cast<const std::uint8_t*>(ticket.text.data()),
                ticket.text.size()};
  auto digest_result = hasher.Hash(bytes);
  if (const auto* error = std::get_if<ServiceError>(&digest_result)) {
    return *error;
  }
  ticket.digest = std::get<Sha256Digest>(digest_result);
  ticket.issued_at = clock.Now();

  const auto lifetime =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          kLoginTicketLifetime);
  if (ticket.issued_at.time_since_epoch() >
      std::chrono::system_clock::time_point::max().time_since_epoch() -
          lifetime) {
    return TransactionContractError("系统时间无法表示票据到期时刻");
  }
  ticket.expires_at = ticket.issued_at + lifetime;
  return ticket;
}

}  // namespace

RegisterAccountService::RegisterAccountService(
    const CryptographicRandomSource& random,
    const Pbkdf2PasswordHasher& password_hasher,
    RegisterAccountUnitOfWork& unit_of_work) noexcept
    : random_(random),
      password_hasher_(password_hasher),
      unit_of_work_(unit_of_work) {}

ServiceResult<RegisteredAccount> RegisterAccountService::RegisterAccount(
    const RegisterAccountCommand& command) const {
  auto account_result = domain::AccountName::Parse(command.account_name);
  if (const auto* error =
          std::get_if<domain::AccountNameValidationError>(&account_result)) {
    return InvalidAccountName(*error);
  }
  if (domain::IsRestrictedName(command.account_name)) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "账号包含禁用词或保留名称，请更换账号"};
  }
  if (const auto error = domain::ValidatePassword(command.password)) {
    return InvalidPassword(*error);
  }
  auto account_name = std::get<domain::AccountName>(std::move(account_result));

  auto salt_result = random_.GenerateCredentialSalt();
  if (const auto* error = std::get_if<ServiceError>(&salt_result)) {
    return *error;
  }
  const auto salt = std::get<CredentialSalt>(salt_result);
  auto digest_result =
      password_hasher_.Derive(command.password, salt, kPbkdf2Sha256Iterations);
  if (const auto* error = std::get_if<ServiceError>(&digest_result)) {
    return *error;
  }

  const NewAccountSeed seed{.account_name = std::move(account_name),
                            .salt = salt,
                            .digest = std::get<PasswordDigest>(digest_result),
                            .iterations = kPbkdf2Sha256Iterations};
  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<RegisterAccountTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("注册事务工厂返回空对象");
  }

  auto create_result = transaction->CreateAccount(seed);
  if (const auto* error = std::get_if<ServiceError>(&create_result)) {
    return error->code == ErrorCode::kConflict ? RegistrationConflict()
                                               : *error;
  }
  auto result = std::get<RegisteredAccount>(std::move(create_result));
  if (!ValidRegisteredAccount(result, seed.account_name)) {
    return TransactionContractError("新账号标识或展示名不一致");
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return result;
}

LoginWithPasswordService::LoginWithPasswordService(
    const Pbkdf2PasswordHasher& password_hasher,
    const CryptographicRandomSource& random, const Sha256Hasher& ticket_hasher,
    const SystemClock& clock, AccountCredentialReader& credential_reader,
    IssueLoginTicketUnitOfWork& unit_of_work) noexcept
    : password_hasher_(password_hasher),
      random_(random),
      ticket_hasher_(ticket_hasher),
      clock_(clock),
      credential_reader_(credential_reader),
      unit_of_work_(unit_of_work) {}

ServiceResult<LauncherLoginResult> LoginWithPasswordService::Login(
    const LoginWithPasswordCommand& command) const {
  auto verified = VerifyPassword(command);
  if (auto* error = std::get_if<ServiceError>(&verified)) return *error;
  return IssueTicket(std::get<PasswordAuthenticatedAccount>(verified));
}

ServiceResult<PasswordAuthenticatedAccount>
LoginWithPasswordService::VerifyPassword(
    const LoginWithPasswordCommand& command) const {
  const auto account_result = domain::AccountName::Parse(command.account_name);
  if (const auto* error =
          std::get_if<domain::AccountNameValidationError>(&account_result)) {
    return InvalidAccountName(*error);
  }
  if (const auto error = domain::ValidatePassword(command.password)) {
    return InvalidPassword(*error);
  }
  const auto& account_name = std::get<domain::AccountName>(account_result);

  auto lookup_result = credential_reader_.FindByLoginKey(account_name.Key());
  if (const auto* error = std::get_if<ServiceError>(&lookup_result)) {
    return *error;
  }
  auto credential = std::get<std::optional<StoredAccountCredential>>(
      std::move(lookup_result));
  if (credential.has_value() &&
      !ValidStoredCredential(*credential, account_name.Key())) {
    return TransactionContractError("数据库凭据字段不满足已发布约束");
  }

  const CredentialSalt fallback_salt{};
  const PasswordDigest fallback_digest{};
  const auto& salt = credential.has_value() ? credential->salt : fallback_salt;
  const auto& digest =
      credential.has_value() ? credential->digest : fallback_digest;
  const auto iterations =
      credential.has_value() ? credential->iterations : kPbkdf2Sha256Iterations;
  auto verify_result =
      password_hasher_.Verify(command.password, salt, iterations, digest);
  if (const auto* error = std::get_if<ServiceError>(&verify_result)) {
    return *error;
  }
  if (!credential.has_value() || !std::get<bool>(verify_result) ||
      credential->status != AccountStatus::kActive) {
    return InvalidLogin();
  }

  return PasswordAuthenticatedAccount{
      credential->account_id, credential->account_name, {}};
}

ServiceResult<LauncherLoginResult> LoginWithPasswordService::IssueTicket(
    const PasswordAuthenticatedAccount& authenticated) const {
  if (!authenticated.account_id.IsValid() || authenticated.account_name.empty())
    return InvalidLogin();
  auto ticket_result = PrepareTicket(random_, ticket_hasher_, clock_);
  if (const auto* error = std::get_if<ServiceError>(&ticket_result)) {
    return *error;
  }
  auto ticket = std::get<PreparedTicket>(std::move(ticket_result));

  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<IssueLoginTicketTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("票据签发事务工厂返回空对象");
  }
  if (auto error =
          transaction->ReplaceTickets(authenticated.account_id, ticket.digest,
                                      ticket.issued_at, ticket.expires_at)) {
    return error->code == ErrorCode::kUnauthenticated ? InvalidLogin() : *error;
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return LauncherLoginResult{.account_id = authenticated.account_id,
                             .account_name = authenticated.account_name,
                             .ticket = std::move(ticket.text),
                             .expires_at = ticket.expires_at};
}

}  // namespace bf::application
