#include "features/authentication/authentication.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace bf::application {
namespace {

constexpr std::string_view kLauncherTicketPrefix = "launcher-v2:";
constexpr std::size_t kLauncherTicketRandomHexLength = 32U;

ServiceError InvalidTicket() {
  return ServiceError{.code = ErrorCode::kUnauthenticated,
                      .safe_message = "登录凭证已失效，请在启动器重新登录"};
}

ServiceError TransactionContractError(std::string_view detail) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::string{"票据事务返回了无效状态："} +
                                      std::string{detail}};
}

bool IsValidTicketShape(std::string_view ticket) noexcept {
  if (!ticket.starts_with(kLauncherTicketPrefix) ||
      ticket.size() !=
          kLauncherTicketPrefix.size() + kLauncherTicketRandomHexLength) {
    return false;
  }
  const auto random_hex = ticket.substr(kLauncherTicketPrefix.size());
  return std::ranges::all_of(random_hex, [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

bool ValidSnapshot(const AuthenticatedPlayerSnapshot& snapshot) noexcept {
  if (!snapshot.account_id.IsValid() || !snapshot.player_id.IsValid() ||
      snapshot.account_name.empty() || snapshot.nickname.empty()) {
    return false;
  }
  return snapshot.is_character_created == snapshot.initial_fighter.has_value();
}

}  // namespace

LoginTicketService::LoginTicketService(
    const Sha256Hasher& hasher, const SystemClock& clock,
    LoginTicketUnitOfWork& unit_of_work) noexcept
    : hasher_(hasher), clock_(clock), unit_of_work_(unit_of_work) {}

ServiceResult<AuthenticatedPlayerSnapshot> LoginTicketService::Consume(
    const ConsumeLoginTicketCommand& command) const {
  if (!IsValidTicketShape(command.ticket)) {
    return InvalidTicket();
  }
  const auto bytes =
      std::span{reinterpret_cast<const std::uint8_t*>(command.ticket.data()),
                command.ticket.size()};
  auto digest_result = hasher_.Hash(bytes);
  if (const auto* error = std::get_if<ServiceError>(&digest_result)) {
    return *error;
  }
  const auto digest = std::get<Sha256Digest>(digest_result);

  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<LoginTicketTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return TransactionContractError("事务工厂返回空对象");
  }

  auto consume_result = transaction->ConsumeTicket(digest, clock_.Now());
  if (const auto* error = std::get_if<ServiceError>(&consume_result)) {
    if (error->code == ErrorCode::kUnauthenticated) {
      return InvalidTicket();
    }
    return *error;
  }
  auto snapshot =
      std::get<AuthenticatedPlayerSnapshot>(std::move(consume_result));
  if (!ValidSnapshot(snapshot)) {
    return TransactionContractError("账号与玩家快照字段不一致");
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return snapshot;
}

}  // namespace bf::application
