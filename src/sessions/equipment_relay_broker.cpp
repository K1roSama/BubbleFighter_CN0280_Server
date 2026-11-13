#include "sessions/equipment_relay_broker.h"

#include <algorithm>
#include <random>
#include <set>
#include <stdexcept>

#include "features/inventory/clothing_attributes.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
application::ServiceError Rejected(
    std::string_view reason, std::string context = {},
    application::ErrorCode code = application::ErrorCode::kUnauthenticated) {
  return {.code = code,
          .safe_message = "equipment relay rejected: reason=" +
                          std::string{reason} + context,
          .correlation_id = {},
          .is_retryable = false};
}

bool SameKey(const protocol::EquipmentRelayKey& a,
             const protocol::EquipmentRelayKey& b) {
  unsigned difference = 0U;
  for (std::size_t i = 0U; i < a.size(); ++i)
    difference |= static_cast<unsigned>(a[i] ^ b[i]);
  return difference == 0U;
}
}  // namespace

EquipmentRelayBroker::EquipmentRelayBroker(
    const application::SystemClock& clock,
    const application::CryptographicRandomSource& random,
    const SessionRegistry& sessions, std::size_t capacity,
    application::EventLogger* logger)
    : clock_(clock),
      random_(random),
      sessions_(sessions),
      kCapacity(capacity),
      logger_(logger) {
  if (capacity == 0U)
    throw std::invalid_argument{"equipment relay capacity is zero"};
}

application::ServiceResult<std::vector<EquipmentRelayInvitation>>
EquipmentRelayBroker::Begin(const application::PreparedMatch& match,
                            std::uint32_t channel,
                            std::span<const EquipmentRelayViewer> viewers) {
  try {
    if (!match.room_id.IsValid() || match.participants.empty() ||
        viewers.empty() || viewers.size() > 36U ||
        match.participants.size() > viewers.size())
      return Rejected("invalid_roster");
    std::map<domain::PlayerId, Client> pending;
    std::set<std::uint8_t> seats;
    std::set<protocol::EquipmentRelayKey> keys;
    std::vector<EquipmentRelayInvitation> invitations;
    const auto expires = clock_.Now() + std::chrono::seconds{30};
    for (const auto& viewer : viewers) {
      const auto gateway = sessions_.CurrentPlayerSession(viewer.player);
      if (!gateway || viewer.seat >= 36U || viewer.nickname.empty() ||
          pending.contains(viewer.player) || !seats.insert(viewer.seat).second)
        return Rejected(
            "invalid_viewer",
            " room_id=" + std::to_string(match.room_id.Value()) +
                " player_id=" + std::to_string(viewer.player.Value()));
      auto first = random_.GenerateLoginTicketSecret();
      auto second = random_.GenerateLoginTicketSecret();
      if (const auto* error = std::get_if<application::ServiceError>(&first))
        return *error;
      if (const auto* error = std::get_if<application::ServiceError>(&second))
        return *error;
      protocol::EquipmentRelayKey key{};
      const auto a = std::get<application::LoginTicketSecret>(first);
      const auto b = std::get<application::LoginTicketSecret>(second);
      std::copy(a.begin(), a.end(), key.begin());
      std::copy_n(b.begin(), 4U, key.begin() + 16);
      if (!keys.insert(key).second) return Rejected("duplicate_generated_key");
      pending.emplace(viewer.player, Client{.room = match.room_id,
                                            .mode = match.mode_id,
                                            .viewer = viewer,
                                            .gateway = *gateway,
                                            .key = key,
                                            .expires = expires});
      invitations.push_back({viewer.player, key});
    }
    // Independent random seed; ticket bytes are never used as gameplay
    // randomness.
    const auto entropy = random_.GenerateLoginTicketSecret();
    if (const auto* error = std::get_if<application::ServiceError>(&entropy))
      return *error;
    const auto bytes = std::get<application::LoginTicketSecret>(entropy);
    std::seed_seq seed(bytes.begin(), bytes.end());
    std::mt19937 random{seed};
    const auto fraction = [&random] {
      return static_cast<float>(random() >> 8U) / 16777216.0F;
    };
    Room room;
    for (const auto& viewer : viewers) room.viewers.push_back(viewer.player);
    std::set<std::uint32_t> objects;
    for (const auto& participant : match.participants) {
      const auto viewer = pending.find(participant.identity.player_id);
      const auto object = participant.identity.actor_object_id;
      if (viewer == pending.end() ||
          viewer->second.viewer.seat != participant.identity.seat_index ||
          object != 1000000U + 100000U * participant.identity.seat_index ||
          !objects.insert(object).second)
        return Rejected("invalid_participant");
      viewer->second.participant = true;
      const auto attributes = domain::NativeClothingAttributes(
          300000U + participant.fighter, participant.equipment_items, channel,
          participant.equipment_attributes);
      protocol::EquipmentDefenseSequence sequence;
      for (std::size_t i = 0U; i < protocol::kEquipmentSequenceLength; ++i) {
        sequence.defense[i] =
            domain::NativeDefensePercent(attributes, fraction());
        if (domain::NativeCriticalCancel(attributes, fraction()))
          sequence.critical_cancel[i / 32U] |= std::uint32_t{1U} << (i % 32U);
      }
      room.sequences.push_back(
          {5U, protocol::EncodeEquipmentDefenseSequence(object, sequence)});
      room.sequences.push_back(
          {6U,
           protocol::EncodeEquipmentCriticalCancelSequence(object, sequence)});
    }
    std::scoped_lock lock{mutex_};
    if (clients_.size() + pending.size() >
        kCapacity + static_cast<std::size_t>(
                        std::ranges::count_if(pending, [this](const auto& row) {
                          return clients_.contains(row.first);
                        })))
      return Rejected("capacity_exceeded");
    for (const auto& [player, client] : clients_) {
      static_cast<void>(player);
      if (keys.contains(client.key)) return Rejected("generated_key_collision");
    }
    // Replace the entire old room atomically; late sockets keep no authority
    // over the new epoch.
    std::erase_if(clients_, [&](const auto& row) {
      if (row.second.room != match.room_id && !pending.contains(row.first))
        return false;
      Retire(row.first, row.second, "new_match");
      return true;
    });
    for (auto& [player, client] : pending)
      clients_.insert_or_assign(player, std::move(client));
    rooms_.insert_or_assign(match.room_id, std::move(room));
    std::erase_if(rooms_, [&](const auto& row) {
      return std::ranges::none_of(clients_, [&](const auto& client) {
        return client.second.room == row.first;
      });
    });
    Log(application::LogLevel::kInfo,
        "equipment bootstrap issued: room_id=" +
            std::to_string(match.room_id.Value()) +
            " mode_id=" + std::to_string(match.mode_id) +
            " viewers=" + std::to_string(viewers.size()));
    return invitations;
  } catch (const std::exception&) {
    return Rejected("bootstrap_exception");
  }
}

application::ServiceResult<std::vector<OutboundMessage>>
EquipmentRelayBroker::Handle(SessionHandle connection,
                             const protocol::Packet& packet) {
  const auto context = " relay_session=" + std::to_string(connection.id) + ":" +
                       std::to_string(connection.generation) +
                       " opcode=" + std::to_string(packet.opcode) +
                       " body_bytes=" + std::to_string(packet.body.size());
  if (!connection.IsValid()) return Rejected("invalid_connection", context);
  std::scoped_lock lock{mutex_};
  if (const auto old = retired_.find(connection); old != retired_.end()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidState,
        .safe_message =
            "equipment relay retired: reason=" + old->second.reason + context +
            " player_id=" + std::to_string(old->second.player.Value()) +
            " room_id=" + std::to_string(old->second.room.Value()) +
            " mode_id=" + std::to_string(old->second.mode) +
            " seat=" + std::to_string(old->second.seat),
        .correlation_id = {},
        .is_retryable = false};
  }
  std::optional<protocol::EquipmentRelayLogin> login;
  std::optional<protocol::EquipmentHealthEvent> health;
  std::optional<protocol::EquipmentActorReport> report;
  bool unsupported = false;
  auto found = std::ranges::find_if(clients_, [&](const auto& row) {
    return row.second.connection == connection;
  });
  const auto reject = [&](std::string_view reason,
                          application::ErrorCode code =
                              application::ErrorCode::kUnauthenticated) {
    return Rejected(
        reason,
        context + (found == clients_.end()
                       ? " stage=unbound"
                       : ClientDiagnostic(found->first, found->second)),
        code);
  };
  try {
    if (packet.opcode == 1U)
      login = protocol::DecodeEquipmentRelayLogin(packet.body);
    else if (packet.opcode == 16U)
      health = protocol::DecodeEquipmentHealthEvent(packet.body);
    else if (packet.opcode == 11U || packet.opcode == 13U ||
             packet.opcode == 14U || packet.opcode == 15U ||
             packet.opcode == 22U || packet.opcode == 24U ||
             packet.opcode == 25U || packet.opcode == 26U ||
             packet.opcode == 27U)
      report = protocol::DecodeEquipmentActorReport(packet.opcode, packet.body);
    else if (packet.opcode < 11U || packet.opcode == 21U) {
      // Session/fence control (1-10) and sequence-owned 21 keep their exact
      // formats.
      if (packet.opcode != 6U || !packet.body.empty())
        return reject("unsupported_packet");
    } else {
      unsupported =
          true;  // Report types not recovered yet: ignored after login (below).
    }
  } catch (const protocol::ProtocolError&) {
    return reject("malformed_payload", application::ErrorCode::kInvalidInput);
  }
  if (found == clients_.end() && login) {
    found = std::ranges::find_if(clients_, [&](const auto& row) {
      return SameKey(row.second.key, login->key);
    });
  }
  if (found == clients_.end())
    return reject(login ? "unknown_ticket" : "unbound_connection");
  auto& client = found->second;
  if (client.failed) return reject("connection_failed");
  if (!sessions_.IsCurrentPlayerSession(found->first, client.gateway))
    return reject("gateway_changed");
  if (!client.ready && clock_.Now() >= client.expires)
    return reject("ticket_expired");
  if (client.connection && client.connection != connection)
    return reject("connection_replay");
  if (login) {
    if (!SameKey(client.key, login->key)) return reject("ticket_mismatch");
    if (login->seat != client.viewer.seat) return reject("seat_mismatch");
    // B5A0D0 uses the login-time profile, while 105/503 can refresh the
    // displayed room nickname independently. Authenticate with the immutable
    // profile ID, ticket, current gateway and seat; never adopt a
    // client-supplied nickname.
    if (login->profile_id != found->first.Value())
      return reject("profile_id_mismatch");
    if (client.login) {
      if (*client.login != *login) return reject("duplicate_login_changed");
      return std::vector<OutboundMessage>{};  // TCP already owns the original
                                              // initialization batch.
    }
    const auto room = rooms_.find(client.room);
    if (room == rooms_.end()) return reject("room_missing");
    auto messages = room->second.sequences;
    messages.push_back(
        {13U,
         {}});  // B5A60D: native successful relay notification -> main C2S601.
    messages.push_back(
        {3U, {}});  // B5A674/B5B600: ordered, same-connection C2S6 fence.
    client.connection = connection;
    client.login = std::move(login);
    if (client.login->nickname != client.viewer.nickname)
      Log(application::LogLevel::kInfo,
          "equipment relay nickname differs: identity=verified_profile" +
              context + ClientDiagnostic(found->first, client));
    Log(application::LogLevel::kInfo,
        "equipment relay authenticated:" + context +
            ClientDiagnostic(found->first, client));
    return messages;
  }
  if (!client.login) return reject("login_required");
  if (unsupported) {
    // An authenticated client's native report type this server has not
    // recovered. Closing here ended the relay for the rest of the match
    // (2026-09-29: C2S24 on every weapon change disconnected 63 of 83 players),
    // and the native client keeps relying on this connection for its defense
    // and critical sequences. Count and ignore it under the shared report
    // quota; log each opcode once.
    const auto now = clock_.Now();
    if (now < client.report_window ||
        now - client.report_window >= std::chrono::seconds{1}) {
      client.report_window = now;
      client.reports_in_window = 0U;
    }
    if (++client.reports_in_window > 256U)
      return reject("report_rate_exceeded");
    ++client.reports.unsupported;
    if (std::ranges::find(client.unsupported_logged, packet.opcode) ==
            client.unsupported_logged.end() &&
        client.unsupported_logged.size() < 16U) {
      client.unsupported_logged.push_back(packet.opcode);
      Log(application::LogLevel::kWarning,
          "equipment relay ignored unsupported report, keeping connection:" +
              context + ClientDiagnostic(found->first, client));
    }
    return std::vector<OutboundMessage>{};
  }
  if (health || report) {
    // Native callbacks may report local state between receiving sequences and
    // sending C2S6. Such reports never substitute for the ordered receive
    // fence.
    if (!client.participant)
      return reject(health ? "observer_health" : "observer_report");
    const auto actor = health ? health->actor : report->actor;
    if (actor != 1000000U + 100000U * client.viewer.seat)
      return Rejected(
          health ? "health_actor_mismatch" : "report_actor_mismatch",
          context + ClientDiagnostic(found->first, client) +
              " actor=" + std::to_string(actor) + " expected_actor=" +
              std::to_string(1000000U + 100000U * client.viewer.seat));
    if (report && report->opcode == 27U && report->seat != client.viewer.seat)
      return reject("report_seat_mismatch");
    const auto now = clock_.Now();
    if (now < client.report_window ||
        now - client.report_window >= std::chrono::seconds{1}) {
      client.report_window = now;
      client.reports_in_window = 0U;
    }
    if (++client.reports_in_window > 256U)
      return reject("report_rate_exceeded");
    if (report) {
      // Tick-less C2S22/26 can repeat as distinct transitions. Do not
      // deduplicate those by payload alone. Timestamped reports mask the native
      // padding byte.
      const bool timed = report->opcode == 11U || report->opcode == 13U ||
                         report->opcode == 14U || report->opcode == 15U;
      if (timed && std::ranges::find(client.recent_reports, *report) !=
                       client.recent_reports.end()) {
        ++client.reports.duplicates;
      } else {
        ++client.reports.received[report->opcode];
        if (timed) {
          client.recent_reports.push_back(*report);
          if (client.recent_reports.size() > 256U)
            client.recent_reports.pop_front();
        }
      }
      // These are reports, not requests for an echo or a new defense sequence.
      return std::vector<OutboundMessage>{};
    }
    if (std::ranges::find(client.recent_health, *health) !=
        client.recent_health.end()) {
      ++client.health.duplicates;
    } else {
      ++client.health.reasons[health->reason];
      client.recent_health.push_back(*health);
      if (client.recent_health.size() > 256U) client.recent_health.pop_front();
    }
    // 7EA5C0 sends a report after the local state change. The native dispatcher
    // has no S2C16 acknowledgement. Record it without resetting sequences,
    // replaying a heal, or fabricating a gameplay response.
    return std::vector<OutboundMessage>{};
  }
  if (!client.ready) {
    client.ready = true;
    Log(application::LogLevel::kInfo,
        "equipment relay ready:" + context +
            ClientDiagnostic(found->first, client));
  }
  return std::vector<OutboundMessage>{};  // Native heartbeat acknowledgement
                                          // has no response.
}

void EquipmentRelayBroker::Disconnect(SessionHandle connection) noexcept {
  std::scoped_lock lock{mutex_};
  retired_.erase(connection);
  for (auto& [player, client] : clients_) {
    static_cast<void>(player);
    if (client.connection == connection) {
      client.failed = true;
    }
  }
}

void EquipmentRelayBroker::Revoke(domain::PlayerId player,
                                  std::string_view reason) {
  std::scoped_lock lock{mutex_};
  if (const auto found = clients_.find(player); found != clients_.end())
    Retire(player, found->second, reason);
  clients_.erase(player);
  std::erase_if(rooms_, [&](const auto& row) {
    return std::ranges::none_of(clients_, [&](const auto& client) {
      return client.second.room == row.first;
    });
  });
}

void EquipmentRelayBroker::RevokeRoom(domain::RoomId room,
                                      std::string_view reason) {
  std::scoped_lock lock{mutex_};
  std::erase_if(clients_, [&](const auto& row) {
    if (row.second.room != room) return false;
    Retire(row.first, row.second, reason);
    return true;
  });
  rooms_.erase(room);
}

void EquipmentRelayBroker::Retire(domain::PlayerId player, const Client& client,
                                  std::string_view reason) {
  if (client.connection && !client.failed) {
    retired_.insert_or_assign(
        *client.connection,
        RetiredClient{player, client.room, client.mode, client.viewer.seat,
                      std::string{reason}});
    // At most one record per live auxiliary slot. No ticket survives
    // revocation.
    while (retired_.size() > kCapacity) retired_.erase(retired_.begin());
  }
}

void EquipmentRelayBroker::Log(application::LogLevel level,
                               const std::string& message) const noexcept {
  if (logger_) logger_->Log(level, message);
}

std::string EquipmentRelayBroker::ClientDiagnostic(domain::PlayerId player,
                                                   const Client& client) const {
  const bool current = sessions_.IsCurrentPlayerSession(player, client.gateway);
  const auto stage = client.failed                    ? "disconnected"
                     : !current                       ? "gateway_changed"
                     : client.ready                   ? "ready"
                     : clock_.Now() >= client.expires ? "expired"
                     : client.login                   ? "waiting_fence"
                                                      : "waiting_login";
  return " player_id=" + std::to_string(player.Value()) +
         " room_id=" + std::to_string(client.room.Value()) +
         " mode_id=" + std::to_string(client.mode) +
         " seat=" + std::to_string(client.viewer.seat) + " stage=" + stage +
         " login=" + std::to_string(client.login.has_value()) +
         " ready=" + std::to_string(client.ready) + " bound_relay_session=" +
         (client.connection ? std::to_string(client.connection->id) + ":" +
                                  std::to_string(client.connection->generation)
                            : "none");
}

std::string EquipmentRelayBroker::Diagnostic(domain::RoomId room) const {
  std::scoped_lock lock{mutex_};
  const auto found = rooms_.find(room);
  if (found == rooms_.end()) return " equipment_state=room_missing";
  std::string result;
  for (const auto player : found->second.viewers) {
    const auto client = clients_.find(player);
    result += " [";
    if (client == clients_.end() || client->second.room != room)
      result +=
          "player_id=" + std::to_string(player.Value()) + " stage=revoked";
    else
      result += ClientDiagnostic(player, client->second);
    result += "]";
  }
  return result;
}

EquipmentRelayReadiness EquipmentRelayBroker::Readiness(
    domain::RoomId room) const {
  std::scoped_lock lock{mutex_};
  const auto found = rooms_.find(room);
  if (found == rooms_.end()) return EquipmentRelayReadiness::kFailed;
  bool pending = false;
  for (const auto player : found->second.viewers) {
    const auto client = clients_.find(player);
    if (client == clients_.end() || client->second.room != room ||
        client->second.failed ||
        !sessions_.IsCurrentPlayerSession(player, client->second.gateway) ||
        (!client->second.ready && clock_.Now() >= client->second.expires))
      return EquipmentRelayReadiness::kFailed;
    pending = pending || !client->second.ready;
  }
  return pending ? EquipmentRelayReadiness::kPending
                 : EquipmentRelayReadiness::kReady;
}

std::optional<EquipmentHealthAudit> EquipmentRelayBroker::HealthAudit(
    domain::PlayerId player) const {
  std::scoped_lock lock{mutex_};
  const auto found = clients_.find(player);
  if (found == clients_.end()) return std::nullopt;
  return found->second.health;
}

std::optional<EquipmentReportAudit> EquipmentRelayBroker::ReportAudit(
    domain::PlayerId player) const {
  std::scoped_lock lock{mutex_};
  const auto found = clients_.find(player);
  if (found == clients_.end()) return std::nullopt;
  return found->second.reports;
}
}  // namespace bf::runtime
