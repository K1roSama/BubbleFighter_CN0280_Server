#include "sessions/clan_ticket_broker.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace bf::runtime {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

bool ConstantTimeEqual(std::span<const std::uint8_t> left,
                       const ClanTicketSecret& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  std::uint8_t difference = 0U;
  for (std::size_t index = 0U; index < right.size(); ++index) {
    difference =
        static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
  }
  return difference == 0U;
}

}  // namespace

ClanTicketBroker::ClanTicketBroker(
    const application::SystemClock& clock,
    const application::CryptographicRandomSource& random_source,
    const SessionRegistry& sessions, std::size_t capacity)
    : clock_(clock),
      random_source_(random_source),
      sessions_(sessions),
      kCapacity(capacity) {
  if (kCapacity == 0U) {
    throw std::invalid_argument{"ClanTicketBroker capacity must be positive"};
  }
}

application::ServiceResult<IssuedClanTicket> ClanTicketBroker::Issue(
    domain::PlayerId player_id, const std::string& nickname,
    SessionHandle gateway_session) {
  if (!player_id.IsValid() || nickname.empty() || !gateway_session.IsValid() ||
      !sessions_.IsCurrentPlayerSession(player_id, gateway_session)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能为当前已认证主会话签发战队凭证");
  }
  const auto first_result = random_source_.GenerateLoginTicketSecret();
  if (const auto* error =
          std::get_if<application::ServiceError>(&first_result)) {
    return *error;
  }
  const auto second_result = random_source_.GenerateLoginTicketSecret();
  if (const auto* error =
          std::get_if<application::ServiceError>(&second_result)) {
    return *error;
  }
  ClanTicketSecret secret{};
  const auto& first = std::get<application::LoginTicketSecret>(first_result);
  const auto& second = std::get<application::LoginTicketSecret>(second_result);
  std::ranges::copy(first, secret.begin());
  std::ranges::copy(second,
                    secret.begin() + static_cast<std::ptrdiff_t>(first.size()));

  const auto now = clock_.Now();
  {
    std::scoped_lock lock{mutex_};
    PruneExpiredLocked(now);
    if (!pending_.contains(player_id) && pending_.size() >= kCapacity) {
      return MakeError(application::ErrorCode::kOverloaded, "战队凭证队列已满",
                       true);
    }
    pending_.insert_or_assign(
        player_id, PendingTicket{.nickname = nickname,
                                 .gateway_session = gateway_session,
                                 .secret = secret,
                                 .expires_at = now + std::chrono::seconds{60}});
  }
  return IssuedClanTicket{
      .player_id = player_id, .nickname = nickname, .secret = secret};
}

application::ServiceResult<ClanSessionIdentity> ClanTicketBroker::Consume(
    domain::PlayerId player_id, const std::string& nickname,
    std::span<const std::uint8_t> secret) {
  const auto now = clock_.Now();
  PendingTicket ticket;
  {
    std::scoped_lock lock{mutex_};
    PruneExpiredLocked(now);
    const auto found = pending_.find(player_id);
    if (found == pending_.end() || found->second.nickname != nickname ||
        !ConstantTimeEqual(secret, found->second.secret)) {
      return MakeError(application::ErrorCode::kUnauthenticated,
                       "战队登录凭证已过期");
    }
    ticket = found->second;
    pending_.erase(found);
  }
  if (!sessions_.IsCurrentPlayerSession(player_id, ticket.gateway_session)) {
    return MakeError(application::ErrorCode::kUnauthenticated,
                     "战队登录所属大厅会话已断开");
  }
  return ClanSessionIdentity{.player_id = player_id,
                             .nickname = std::move(ticket.nickname),
                             .gateway_session = ticket.gateway_session,
                             .secret = ticket.secret};
}

bool ClanTicketBroker::IsGatewaySessionCurrent(
    const ClanSessionIdentity& identity) const {
  return sessions_.IsCurrentPlayerSession(identity.player_id,
                                          identity.gateway_session);
}

void ClanTicketBroker::PruneExpiredLocked(
    std::chrono::system_clock::time_point now) {
  std::erase_if(pending_, [now](const auto& entry) {
    return entry.second.expires_at <= now;
  });
}

}  // namespace bf::runtime
