#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_EQUIPMENT_RELAY_BROKER_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_EQUIPMENT_RELAY_BROKER_H_

/**
 * @file equipment_relay_broker.h
 * @brief 本局装备序列、短效 Relay 票据和同连接初始化确认。
 */

#include <deque>
#include <map>
#include <mutex>

#include "application/event_logger.h"
#include "application/system_clock.h"
#include "features/authentication/credential_crypto.h"
#include "features/battle/match_preparation.h"
#include "protocol/rooms/equipment_relay_messages.h"
#include "sessions/session_registry.h"

namespace bf::runtime {
/** @brief 接收全员序列的已认证成员；观察者也必须看到相同序列。 */
struct EquipmentRelayViewer final {
  domain::PlayerId player;
  std::uint8_t seat{0U};
  std::string nickname;
};

/** @brief 网关 S2C601 所需的拥有型玩家与凭据，不记录凭据。 */
struct EquipmentRelayInvitation final {
  domain::PlayerId player;
  protocol::EquipmentRelayKey key{};
};
/** @brief 全员序列初始化状态，失败必须阻止开局。 */
enum class EquipmentRelayReadiness { kPending, kReady, kFailed };

/** @brief 原生客户端状态报告的审计计数；不作为权威 HP、击爆数或结算奖励。 */
struct EquipmentHealthAudit final {
  std::array<std::uint64_t, 17U> reasons{};
  std::uint64_t duplicates{0U};
};

/** @brief 五类原生角色报告的有界审计计数，索引为辅助 opcode。 */
struct EquipmentReportAudit final {
  std::array<std::uint64_t, 28U> received{};
  std::uint64_t duplicates{0U};
  std::uint64_t unsupported{
      0U};  ///< 登录后收到、尚未恢复格式的报告；忽略但不断开。
};

/**
 * @brief 只从开局的有效装备快照生成序列；不接受客户端上报的属性或序列。
 * @note 第一次 C2S1 发全员 S2C5/6，随后原生 S2C13 与 S2C3。同连接 C2S6
 *       是顺序接收确认；重复登录不会再次下发或重置客户端游标。
 */
class EquipmentRelayBroker final {
 public:
  EquipmentRelayBroker(const application::SystemClock& clock,
                       const application::CryptographicRandomSource& random,
                       const SessionRegistry& sessions, std::size_t capacity,
                       application::EventLogger* logger = nullptr);
  [[nodiscard]] application::ServiceResult<
      std::vector<EquipmentRelayInvitation>>
  Begin(const application::PreparedMatch& match, std::uint32_t channel,
        std::span<const EquipmentRelayViewer> viewers);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>> Handle(
      SessionHandle connection, const protocol::Packet& packet);
  void Disconnect(SessionHandle connection) noexcept;
  void Revoke(domain::PlayerId player,
              std::string_view reason = "room_phase_changed");
  void RevokeRoom(domain::RoomId room,
                  std::string_view reason = "room_revoked");
  [[nodiscard]] EquipmentRelayReadiness Readiness(domain::RoomId room) const;
  /** @brief
   * 只读初始化状态快照；包含身份、阶段及失败原因，不包含密钥、昵称或报文内容。
   */
  [[nodiscard]] std::string Diagnostic(domain::RoomId room) const;
  [[nodiscard]] std::optional<EquipmentHealthAudit> HealthAudit(
      domain::PlayerId player) const;
  /** @brief 只读审计；不将客户端报告写入权威血量、移动或结算。 */
  [[nodiscard]] std::optional<EquipmentReportAudit> ReportAudit(
      domain::PlayerId player) const;

 private:
  /** @brief 一个签发周期的主会话与辅助连接绑定。 */
  struct Client final {
    domain::RoomId room;
    std::uint32_t mode{0U};
    EquipmentRelayViewer viewer;
    SessionHandle gateway;
    protocol::EquipmentRelayKey key{};
    std::chrono::system_clock::time_point expires{};
    std::optional<SessionHandle> connection;
    std::optional<protocol::EquipmentRelayLogin> login;
    bool ready{false};
    bool failed{false};
    bool participant{false};
    EquipmentHealthAudit health;
    std::deque<protocol::EquipmentHealthEvent> recent_health;
    EquipmentReportAudit reports;
    std::deque<protocol::EquipmentActorReport> recent_reports;
    std::chrono::system_clock::time_point report_window{};
    std::uint32_t reports_in_window{0U};
    std::vector<std::uint16_t>
        unsupported_logged;  ///< 本连接已记录过日志的未知报告 opcode。
  };

  /** @brief 全员共享的不可变序列批次及必须确认的接收名单。 */
  struct Room final {
    std::vector<OutboundMessage> sequences;
    std::vector<domain::PlayerId> viewers;
  };

  /** @brief 已认证旧连接的有界收尾记录；不保留凭据，也不能授权新局。 */
  struct RetiredClient final {
    domain::PlayerId player;
    domain::RoomId room;
    std::uint32_t mode{0U};
    std::uint8_t seat{0U};
    std::string reason;
  };

  void Retire(domain::PlayerId player, const Client& client,
              std::string_view reason);
  [[nodiscard]] std::string ClientDiagnostic(domain::PlayerId player,
                                             const Client& client) const;
  void Log(application::LogLevel level,
           const std::string& message) const noexcept;
  const application::SystemClock& clock_;
  const application::CryptographicRandomSource& random_;
  const SessionRegistry& sessions_;
  const std::size_t kCapacity;
  application::EventLogger* logger_;
  mutable std::mutex mutex_;
  std::map<domain::PlayerId, Client> clients_;
  std::map<domain::RoomId, Room> rooms_;
  std::map<SessionHandle, RetiredClient> retired_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_EQUIPMENT_RELAY_BROKER_H_
