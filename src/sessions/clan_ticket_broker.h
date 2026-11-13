#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_TICKET_BROKER_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_TICKET_BROKER_H_

/**
 * @file clan_ticket_broker.h
 * @brief 定义主网关与 Nx 战队子连接之间的短效、单次会话票据。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"
#include "features/authentication/credential_crypto.h"
#include "sessions/session_registry.h"

namespace bf::runtime {

/** @brief Nx C2S1005 携带的 256 位不可预测票据原文。 */
using ClanTicketSecret = std::array<std::uint8_t, 32U>;

/** @brief 主网关签发后供 S2C23 编码的玩家身份与票据。 */
struct IssuedClanTicket final {
  domain::PlayerId player_id;  ///< 票据绑定的权威玩家。
  std::string nickname;        ///< 签发时的权威昵称。
  ClanTicketSecret secret{};   ///< 仅返回客户端，不得写入日志。
};

/** @brief Nx 成功消费票据后保存到子连接的主会话绑定。 */
struct ClanSessionIdentity final {
  domain::PlayerId player_id;     ///< 客户端不能再更改的玩家。
  std::string nickname;           ///< 签发时昵称，用于重复 C2S1005 校验。
  SessionHandle gateway_session;  ///< 票据绑定的主网关会话句柄与代次。
  ClanTicketSecret secret{};      ///< 仅子连接内存持有，断开即释放。
};

/**
 * @brief 线程安全签发并单次消费战队票据。
 * @note 票据固定 60 秒有效，同玩家重新签发会使旧票据失效；每次操作还需再验证
 *       gateway_session，因此主连接断开后 Nx 连接不能独立存活。
 */
class ClanTicketBroker final {
 public:
  /** @brief 注入时钟、CSPRNG、主会话注册表与最大待消费票据数。 */
  ClanTicketBroker(const application::SystemClock& clock,
                   const application::CryptographicRandomSource& random_source,
                   const SessionRegistry& sessions, std::size_t capacity);

  /** @brief 为当前在线主会话签发新票据，并原子替换该玩家未消费旧票据。 */
  [[nodiscard]] application::ServiceResult<IssuedClanTicket> Issue(
      domain::PlayerId player_id, const std::string& nickname,
      SessionHandle gateway_session);

  /** @brief 校验玩家、昵称、密文、过期时间和主会话，成功时单次消费。 */
  [[nodiscard]] application::ServiceResult<ClanSessionIdentity> Consume(
      domain::PlayerId player_id, const std::string& nickname,
      std::span<const std::uint8_t> secret);

  /** @brief 判断已认证 Nx 连接所绑定的主网关会话是否仍在线。 */
  [[nodiscard]] bool IsGatewaySessionCurrent(
      const ClanSessionIdentity& identity) const;

 private:
  /** @brief 尚未消费的票据及其主会话代次和绝对过期时刻。 */
  struct PendingTicket final {
    std::string nickname;
    SessionHandle gateway_session;
    ClanTicketSecret secret{};
    std::chrono::system_clock::time_point expires_at{};
  };

  void PruneExpiredLocked(std::chrono::system_clock::time_point now);

  const application::SystemClock& clock_;
  const application::CryptographicRandomSource& random_source_;
  const SessionRegistry& sessions_;
  const std::size_t kCapacity;
  std::mutex mutex_;
  std::map<domain::PlayerId, PendingTicket> pending_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_CLAN_TICKET_BROKER_H_
