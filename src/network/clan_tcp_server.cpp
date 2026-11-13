#include "network/clan_tcp_server.h"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "protocol/clans/clan_messages.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "sessions/social_command_flow.h"

namespace bf::infrastructure::network {
namespace {

using boost::asio::ip::tcp;
using ClanPackets = std::vector<protocol::ClanPacket>;

/** @brief 显式消费仅通过 error_code 报错的 Asio 操作返回值。 */
template <typename Result>
void ObserveIoResult(Result&& /*result*/) noexcept {}

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

protocol::ClanPacket Packet(std::uint16_t opcode,
                            std::vector<std::uint8_t> body = {}) {
  return {.opcode = opcode, .body = std::move(body)};
}

std::vector<std::uint8_t> I32Body(std::int32_t value) {
  protocol::ClanPacketWriter writer;
  writer.WriteI32(value);
  return writer.Release();
}

std::vector<std::uint8_t> U32Body(std::uint32_t value) {
  protocol::ClanPacketWriter writer;
  writer.WriteU32(value);
  return writer.Release();
}

std::uint32_t LegacyId(std::uint64_t value) {
  if (value == 0U || value > std::numeric_limits<std::uint32_t>::max()) {
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "clan identifier exceeds uint32"};
  }
  return static_cast<std::uint32_t>(value);
}

bool ConstantTimeEqual(std::span<const std::uint8_t> left,
                       const runtime::ClanTicketSecret& right) noexcept {
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

class ClanConnectionSink {
 public:
  virtual ~ClanConnectionSink() = default;
  virtual void Emit(protocol::ClanPacket value) = 0;
  virtual void RequestStop() noexcept = 0;
};

class ClanHub final {
 public:
  void Bind(domain::PlayerId player_id,
            const std::shared_ptr<ClanConnectionSink>& connection) {
    std::shared_ptr<ClanConnectionSink> previous;
    {
      std::scoped_lock lock{mutex_};
      const auto found = connections_.find(player_id);
      if (found != connections_.end()) {
        previous = found->second.lock();
      }
      connections_[player_id] = connection;
    }
    if (previous && previous.get() != connection.get()) {
      previous->RequestStop();
    }
  }

  void Unbind(domain::PlayerId player_id, const ClanConnectionSink* expected) {
    std::scoped_lock lock{mutex_};
    const auto found = connections_.find(player_id);
    if (found != connections_.end()) {
      const auto active = found->second.lock();
      if (!active || active.get() == expected) {
        connections_.erase(found);
      }
    }
    scenes_.erase(player_id);
  }

  [[nodiscard]] std::shared_ptr<ClanConnectionSink> Connection(
      domain::PlayerId player_id) const {
    std::scoped_lock lock{mutex_};
    const auto found = connections_.find(player_id);
    return found == connections_.end() ? nullptr : found->second.lock();
  }

  void Emit(domain::PlayerId player_id, protocol::ClanPacket value) const {
    if (auto target = Connection(player_id)) {
      target->Emit(std::move(value));
    }
  }

  void SetScene(domain::PlayerId player_id, std::uint32_t scene) {
    std::scoped_lock lock{mutex_};
    scenes_[player_id] = scene;
  }

  [[nodiscard]] std::uint32_t Scene(domain::PlayerId player_id,
                                    bool gateway_online) const {
    if (!gateway_online) {
      return 0U;
    }
    std::scoped_lock lock{mutex_};
    const auto found = scenes_.find(player_id);
    return found == scenes_.end() ? 3U : found->second;
  }

 private:
  mutable std::mutex mutex_;
  std::map<domain::PlayerId, std::weak_ptr<ClanConnectionSink>> connections_;
  std::map<domain::PlayerId, std::uint32_t> scenes_;
};

class ClanSession final : public ClanConnectionSink,
                          public std::enable_shared_from_this<ClanSession> {
 public:
  using ClosedCallback = std::function<void(std::uint64_t)>;

  ClanSession(std::uint64_t connection_id, tcp::socket socket,
              std::size_t maximum_send_queue_bytes,
              application::BackgroundTaskExecutor& database_tasks,
              application::ClanExecutor& clans,
              application::PlayerMenuSnapshotLoader& menu_snapshots,
              runtime::ClanTicketBroker& tickets,
              runtime::SessionRegistry& gateway_sessions,
              const application::SystemClock& clock,
              application::EventLogger& logger, ClanHub& hub,
              ClosedCallback closed_callback)
      : connection_id_(connection_id),
        socket_(std::move(socket)),
        timer_(socket_.get_executor()),
        kMaximumSendQueueBytes(maximum_send_queue_bytes),
        database_tasks_(database_tasks),
        clans_(clans),
        menu_snapshots_(menu_snapshots),
        tickets_(tickets),
        gateway_sessions_(gateway_sessions),
        clock_(clock),
        logger_(logger),
        hub_(hub),
        closed_callback_(std::move(closed_callback)) {}

  void Start() {
    boost::asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
      self->EnqueueOnStrand(Packet(1002U));
      self->StartReadHeader();
    });
  }

  void Emit(protocol::ClanPacket value) override {
    std::size_t wire_size = protocol::kClanFrameHeaderBytes + value.body.size();
    if (value.body.size() > protocol::kMaximumClanBodyBytes ||
        wire_size > kMaximumSendQueueBytes) {
      RequestStop();
      return;
    }
    auto current = externally_pending_bytes_.load(std::memory_order_relaxed);
    while (true) {
      if (current > kMaximumSendQueueBytes - wire_size) {
        RequestStop();
        return;
      }
      if (externally_pending_bytes_.compare_exchange_weak(
              current, current + wire_size, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        break;
      }
    }
    try {
      boost::asio::post(socket_.get_executor(),
                        [self = shared_from_this(), value = std::move(value),
                         wire_size]() mutable {
                          self->externally_pending_bytes_.fetch_sub(
                              wire_size, std::memory_order_acq_rel);
                          self->EnqueueOnStrand(value);
                        });
    } catch (...) {
      externally_pending_bytes_.fetch_sub(wire_size, std::memory_order_acq_rel);
      RequestStop();
    }
  }

  void RequestStop() noexcept override {
    if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      boost::asio::post(socket_.get_executor(),
                        [self = shared_from_this()] { self->Finalize(); });
    } catch (...) {
      logger_.Log(application::LogLevel::kWarning,
                  "Nx clan session stop request could not be posted",
                  std::to_string(connection_id_));
    }
  }

 private:
  using BusinessResult = application::ServiceResult<ClanPackets>;

  void StartReadHeader() {
    if (finalized_ || handling_packet_) {
      return;
    }
    ArmTimeout(identity_.has_value() ? std::chrono::seconds{180}
                                     : std::chrono::seconds{10});
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_header_),
        [self = shared_from_this()](const boost::system::error_code& error,
                                    std::size_t transferred) {
          self->HandleHeader(error, transferred);
        });
  }

  void HandleHeader(const boost::system::error_code& error,
                    std::size_t transferred) {
    if (finalized_ || error || transferred != read_header_.size()) {
      Finalize();
      return;
    }
    const auto size = static_cast<std::uint16_t>(read_header_[0]) |
                      (static_cast<std::uint16_t>(read_header_[1]) << 8U);
    pending_opcode_ = static_cast<std::uint16_t>(read_header_[2]) |
                      (static_cast<std::uint16_t>(read_header_[3]) << 8U);
    if (size > protocol::kMaximumClanBodyBytes) {
      logger_.Log(application::LogLevel::kWarning,
                  "Nx clan frame exceeds length limit",
                  std::to_string(connection_id_));
      Finalize();
      return;
    }
    read_body_.assign(size, 0U);
    if (read_body_.empty()) {
      ObserveIoResult(timer_.cancel());
      HandleCompleteFrame();
      return;
    }
    boost::asio::async_read(
        socket_, boost::asio::buffer(read_body_),
        [self = shared_from_this()](const boost::system::error_code& body_error,
                                    std::size_t body_transferred) {
          if (self->finalized_ || body_error ||
              body_transferred != self->read_body_.size()) {
            self->Finalize();
            return;
          }
          ObserveIoResult(self->timer_.cancel());
          self->HandleCompleteFrame();
        });
  }

  void HandleCompleteFrame() {
    if (finalized_) {
      return;
    }
    try {
      protocol::CryptClanPayload(pending_opcode_, read_body_);
      protocol::ClanPacket incoming{.opcode = pending_opcode_,
                                    .body = std::move(read_body_)};
      if (incoming.opcode == 1003U) {
        protocol::ClanPacketReader reader{incoming.body};
        reader.RequireEnd();
        if (!identity_.has_value()) {
          EnqueueOnStrand(Packet(1002U));
        } else if (!tickets_.IsGatewaySessionCurrent(*identity_)) {
          Finalize();
          return;
        }
        StartReadHeader();
        return;
      }
      if (incoming.opcode == 1005U) {
        HandleLogin(std::move(incoming));
        return;
      }
      if (!identity_.has_value() ||
          !tickets_.IsGatewaySessionCurrent(*identity_)) {
        logger_.Log(application::LogLevel::kWarning,
                    "Nx clan command has no live gateway binding",
                    std::to_string(connection_id_));
        Finalize();
        return;
      }
      if ((incoming.opcode == 1066U || incoming.opcode == 1042U) &&
          !ConsumeChatQuota()) {
        HandleBusinessError(incoming,
                            MakeError(application::ErrorCode::kOverloaded,
                                      "发送过快，请稍后再试"));
        StartReadHeader();
        return;
      }
      handling_packet_ = true;
      auto self = shared_from_this();
      const auto identity = *identity_;
      auto request =
          std::make_shared<protocol::ClanPacket>(std::move(incoming));
      // 回调主体已经捕获全部异常；LLVM 19 仍会把 std::function
      // 销毁路径误报为逃逸。 NOLINTNEXTLINE(bugprone-exception-escape)
      const auto post_error = database_tasks_.Post([self, identity, request] {
        try {
          auto result = self->HandleBusiness(*request, identity);
          boost::asio::post(
              self->socket_.get_executor(),
              [self, request, result = std::move(result)]() mutable {
                self->CompleteBusiness(*request, std::move(result));
              });
        } catch (const protocol::ProtocolError&) {
          self->logger_.Log(application::LogLevel::kWarning,
                            "Nx clan handler rejected malformed payload");
          self->RequestStop();
        } catch (const std::exception&) {
          self->logger_.Log(application::LogLevel::kError,
                            "Nx clan handler failed by exception");
          self->RequestStop();
        } catch (...) {
          self->RequestStop();
        }
      });
      if (post_error.has_value()) {
        handling_packet_ = false;
        HandleBusinessError(*request, *post_error);
        StartReadHeader();
      }
    } catch (const protocol::ProtocolError&) {
      logger_.Log(application::LogLevel::kWarning,
                  "Nx clan frame payload is malformed",
                  std::to_string(connection_id_));
      Finalize();
    }
  }

  void HandleLogin(protocol::ClanPacket incoming) {
    try {
      protocol::ClanPacketReader reader{incoming.body};
      const auto raw_player_id = reader.ReadU32();
      const auto nickname = reader.ReadText(32U);
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU32());
      const auto secret_size = reader.ReadU32();
      if (secret_size > 128U) {
        throw protocol::ProtocolError{protocol::ProtocolErrorCode::kLengthLimit,
                                      "clan ticket exceeds limit"};
      }
      const auto secret = reader.ReadBytes(secret_size);
      reader.RequireEnd();
      const domain::PlayerId player_id{raw_player_id};
      if (identity_.has_value()) {
        if (identity_->player_id != player_id ||
            identity_->nickname != nickname ||
            !ConstantTimeEqual(secret, identity_->secret) ||
            !tickets_.IsGatewaySessionCurrent(*identity_)) {
          Finalize();
          return;
        }
      } else {
        auto consumed = tickets_.Consume(player_id, nickname, secret);
        if (const auto* error =
                std::get_if<application::ServiceError>(&consumed)) {
          logger_.Log(application::LogLevel::kWarning, error->safe_message,
                      std::to_string(connection_id_));
          Finalize();
          return;
        }
        identity_ = std::get<runtime::ClanSessionIdentity>(std::move(consumed));
        hub_.Bind(identity_->player_id, shared_from_this());
      }
      EnqueueOnStrand(Packet(1006U, I32Body(1)));
      StartReadHeader();
    } catch (const protocol::ProtocolError&) {
      Finalize();
    }
  }

  BusinessResult HandleBusiness(const protocol::ClanPacket& incoming,
                                const runtime::ClanSessionIdentity& identity) {
    const auto player_id = identity.player_id;
    protocol::ClanPacketReader reader{incoming.body};
    switch (incoming.opcode) {
      case 1073U: {
        reader.RequireEnd();
        auto loaded = clans_.Activity(player_id);
        if (const auto* error = std::get_if<application::ServiceError>(&loaded))
          return *error;
        const auto& value = std::get<application::ClanActivityView>(loaded);
        ClanPackets responses{
            Packet(1074U, protocol::EncodeClanActivity(value))};
        if (last_activity_level_ &&
            last_activity_level_->first == value.clan_id &&
            value.level > last_activity_level_->second && value.level <= 255U) {
          protocol::ClanPacketWriter level;
          level.WriteU32(value.level);
          responses.push_back(Packet(1075U, level.Release()));
        }
        last_activity_level_ = std::pair{value.clan_id, value.level};
        return responses;
      }
      case 1084U:
      case 1086U:
      case 1088U:
      case 1090U: {
        application::ClanRankingQuery query;
        if (incoming.opcode == 1088U || incoming.opcode == 1090U)
          query.clan_id = reader.ReadU32();
        if (incoming.opcode == 1084U || incoming.opcode == 1088U)
          query.month = reader.ReadMonth();
        reader.RequireEnd();
        auto loaded = clans_.Rankings(player_id, query);
        if (const auto* error = std::get_if<application::ServiceError>(&loaded))
          return *error;
        return ClanPackets{Packet(
            static_cast<std::uint16_t>(incoming.opcode + 1U),
            protocol::EncodeClanRankings(
                std::get<std::vector<application::ClanRankingEntry>>(loaded),
                query.clan_id.has_value()))};
      }
      case 1082U: {
        const auto clan = reader.ReadU32();
        reader.RequireEnd();
        auto loaded = clans_.RecentMatches(player_id, clan);
        if (const auto* error = std::get_if<application::ServiceError>(&loaded))
          return *error;
        return ClanPackets{Packet(
            1083U,
            protocol::EncodeClanRecentMatches(
                std::get<std::vector<application::ClanRecentMatch>>(loaded)))};
      }
      case 1076U:
      case 1078U: {
        const auto clan = reader.ReadU32();
        const auto month = reader.ReadMonth();
        reader.RequireEnd();
        const bool opponents = incoming.opcode == 1078U;
        auto loaded = clans_.Records(
            player_id,
            {.clan_id = clan, .month = month, .opponents = opponents});
        if (const auto* error = std::get_if<application::ServiceError>(&loaded))
          return *error;
        return ClanPackets{Packet(
            static_cast<std::uint16_t>(incoming.opcode + 1U),
            protocol::EncodeClanRecords(
                std::get<std::vector<application::ClanRecordEntry>>(loaded),
                opponents))};
      }
      case 1094U: {
        const auto clan = reader.ReadU32();
        reader.RequireEnd();
        auto loaded = clans_.TodayResults(player_id, clan);
        if (const auto* error = std::get_if<application::ServiceError>(&loaded))
          return *error;
        const auto& counts = std::get<application::ClanTodayResults>(loaded);
        protocol::ClanPacketWriter response;
        response.WriteU32(counts.wins);
        response.WriteU32(counts.draws);
        response.WriteU32(counts.losses);
        return ClanPackets{Packet(1095U, response.Release())};
      }
      case 1102U:
        return HandleRecruitment(player_id, reader);
      case 1042U:
        return HandleWhisper(player_id, reader);
      case 1015U:
        return HandleHome(player_id, reader);
      case 1017U:
        reader.RequireEnd();
        return ProfilePackets(player_id);
      case 1019U:
        return HandleSearch(reader);
      case 1108U:
        if (reader.ReadI32() != -20) {
          return MakeError(application::ErrorCode::kInvalidInput,
                           "训练所战队退出状态不正确");
        }
        reader.RequireEnd();
        return HandleLeave(player_id, true, identity.nickname);
      case 1036U:
        reader.RequireEnd();
        return HandleLeave(player_id, false, identity.nickname);
      case 1106U:
        return HandleJoinTraining(player_id, reader);
      case 1011U:
        reader.RequireEnd();
        return RosterPackets(player_id, false);
      case 1013U:
        static_cast<void>(reader.ReadU32());
        reader.RequireEnd();
        return RosterPackets(player_id, true);
      case 1027U:
        return HandleCreate(player_id, reader);
      case 1045U:
        return HandleScene(player_id, reader);
      case 1066U:
        return HandleChat(player_id, reader);
      case 1032U:
      case 1033U:
        return HandleApply(player_id, incoming.opcode, reader,
                           identity.nickname);
      case 1063U:
        return HandleCancelApplication(player_id, reader);
      case 1069U:
        return HandleApplicants(player_id, reader);
      case 1039U:
        return HandleMemberChange(player_id, reader);
      case 1048U:
        return HandleEditText(player_id, false, reader);
      case 1051U:
        return HandleEditText(player_id, true, reader);
      case 1096U:
        return HandleMarkRefresh(player_id, reader);
      case 1057U:
        return HandleTransferRequest(player_id, reader, identity.nickname);
      case 1060U:
        return HandleTransferResponse(player_id, reader, identity.nickname);
      case 1054U:
        return HandleTransferFinish(player_id, reader, identity.nickname);
      case 1029U:
        return HandleDisband(player_id, reader);
      case 1021U:
        return HandlePosts(player_id, reader);
      case 1023U:
        return HandleAddPost(player_id, reader);
      case 1025U:
        return HandleDeletePost(player_id, reader);
      default:
        logger_.Log(application::LogLevel::kInfo,
                    "Nx clan opcode is not implemented: " +
                        std::to_string(incoming.opcode),
                    std::to_string(connection_id_));
        // 旧客户端不同版本可能附带尚未恢复的扩展字段。未知操作只记录且忽略，
        // 不按已知消息的“必须读完”规则主动断开兼容客户端。
        return ClanPackets{};
    }
  }

  BusinessResult HandleWhisper(domain::PlayerId player_id,
                               protocol::ClanPacketReader& reader) {
    // 721050 writes kind, target, text; 724550/724920 read canonical identity
    // strings.
    const auto kind = reader.ReadU32();
    const auto target = reader.ReadText(32U);
    const auto body = reader.ReadText(256U);
    reader.RequireEnd();
    auto result = clans_.Whisper(
        player_id, target, body, kind, [this](domain::PlayerId recipient) {
          return gateway_sessions_.Lookup(recipient).online &&
                 hub_.Connection(recipient) != nullptr;
        });
    protocol::ClanPacketWriter reply;
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      const auto status =
          error->code == application::ErrorCode::kInvalidInput ? 3U
          : (error->code == application::ErrorCode::kInvalidState ||
             error->code == application::ErrorCode::kPermissionDenied)
              ? 2U
              : 4U;
      reply.WriteU32(status);
      reply.WriteU32(kind);
      reply.WriteText(target);
      reply.WriteText(target);
      reply.WriteText(body);
      return ClanPackets{Packet(1043U, reply.Release())};
    }
    const auto& sent = std::get<application::ClanWhisperResult>(result);
    protocol::ClanPacketWriter delivery;
    delivery.WriteU32(kind);
    // 724920把第一个身份字符串交给AF5B40保存/r目标；两字段均使用已认证玩家的当前昵称。
    delivery.WriteText(sent.sender_nickname);
    delivery.WriteText(sent.sender_nickname);
    delivery.WriteText(body);
    hub_.Emit(sent.recipient, Packet(1044U, delivery.Release()));
    reply.WriteU32(1U);
    reply.WriteU32(kind);
    reply.WriteText(sent.recipient_nickname);
    reply.WriteText(sent.recipient_nickname);
    reply.WriteText(body);
    return ClanPackets{Packet(1043U, reply.Release())};
  }

  BusinessResult HandleHome(domain::PlayerId player_id,
                            protocol::ClanPacketReader& reader) {
    reader.RequireEnd();
    auto starter = clans_.EnsureStarterCard(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&starter)) {
      return *error;
    }
    if (std::get<bool>(starter)) {
      auto snapshot = menu_snapshots_.Load(player_id);
      if (const auto* error =
              std::get_if<application::ServiceError>(&snapshot)) {
        return *error;
      }
      DispatchGateway(player_id,
                      {.opcode = 802U,
                       .body = protocol::EncodeInventorySnapshotResponse(
                           std::get<application::PlayerMenuSnapshot>(
                               std::move(snapshot)))});
    }
    auto home = clans_.Home(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&home)) {
      return *error;
    }
    const auto& state = std::get<application::ClanHomeState>(home);
    ClanPackets replies{Packet(1016U, protocol::EncodeClanHome(state))};
    if (state.clan.has_value()) {
      auto profile = ProfilePackets(player_id);
      if (const auto* error =
              std::get_if<application::ServiceError>(&profile)) {
        return *error;
      }
      auto profile_values = std::get<ClanPackets>(std::move(profile));
      replies.insert(replies.end(),
                     std::make_move_iterator(profile_values.begin()),
                     std::make_move_iterator(profile_values.end()));
    }
    return replies;
  }

  BusinessResult ProfilePackets(domain::PlayerId player_id) {
    auto profile = clans_.Profile(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&profile)) {
      return *error;
    }
    auto clan =
        std::get<std::optional<application::ClanSummary>>(std::move(profile));
    std::vector<application::ClanMemberView> roster;
    if (clan.has_value()) {
      auto roster_result = clans_.Roster(player_id);
      if (const auto* error =
              std::get_if<application::ServiceError>(&roster_result)) {
        return *error;
      }
      roster = std::get<std::vector<application::ClanMemberView>>(
          std::move(roster_result));
    }
    return ClanPackets{
        Packet(1018U, protocol::EncodeClanProfile(clan, roster))};
  }

  BusinessResult HandleSearch(protocol::ClanPacketReader& reader) {
    static_cast<void>(reader.ReadU32());
    const auto page = reader.ReadU32();
    auto query = reader.ReadText(32U);
    reader.RequireEnd();
    auto result = clans_.Search(page, query);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto search_page = std::get<application::ClanSearchPage>(std::move(result));
    return ClanPackets{Packet(
        1020U, protocol::EncodeClanSearch(
                   search_page.entries, search_page.total_count, page, query))};
  }

  BusinessResult HandleLeave(domain::PlayerId player_id, bool training_only,
                             std::string_view actor_nickname) {
    auto old_roster = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&old_roster)) {
      if (error->code != application::ErrorCode::kInvalidState) {
        return *error;
      }
    }
    auto result = clans_.Leave(player_id, training_only);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto left =
        std::get<std::optional<application::ClanSummary>>(std::move(result));
    if (!left.has_value()) {
      return ClanPackets{};
    }
    protocol::ClanPacketWriter departure;
    departure.WriteText(left->name);
    departure.WriteU32(LegacyId(player_id.Value()));
    departure.WriteText(actor_nickname);
    departure.WriteU32(0U);
    departure.WriteI32(-20);
    auto departure_body = departure.Release();
    if (const auto* roster =
            std::get_if<std::vector<application::ClanMemberView>>(
                &old_roster)) {
      Broadcast(*roster, 1037U, departure_body, player_id);
    }
    auto home = clans_.Home(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&home)) {
      return *error;
    }
    return ClanPackets{
        Packet(1037U, std::move(departure_body)),
        Packet(1016U, protocol::EncodeClanHome(
                          std::get<application::ClanHomeState>(home)))};
  }

  BusinessResult HandleJoinTraining(domain::PlayerId player_id,
                                    protocol::ClanPacketReader& reader) {
    reader.RequireEnd();
    auto result = clans_.JoinTraining(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& clan = std::get<application::ClanSummary>(result);
    protocol::ClanPacketWriter writer;
    writer.WriteU32(LegacyId(clan.clan_id));
    writer.WriteU32(application::kClanMemberRole);
    writer.WriteText(clan.name);
    return ClanPackets{Packet(1107U, writer.Release())};
  }

  BusinessResult RosterPackets(domain::PlayerId player_id, bool extended) {
    auto result = clans_.Roster(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto roster =
        std::get<std::vector<application::ClanMemberView>>(std::move(result));
    return ClanPackets{
        Packet(extended ? 1014U : 1012U,
               protocol::EncodeClanRoster(WireRoster(roster), extended))};
  }

  BusinessResult HandleCreate(domain::PlayerId player_id,
                              protocol::ClanPacketReader& reader) {
    auto name = reader.ReadText(12U);
    static_cast<void>(reader.ReadU32());
    auto introduction = reader.ReadText(100U);
    const auto card = reader.ReadU32();
    const auto capacity = reader.ReadU32();
    reader.RequireEnd();
    auto result = clans_.Create(player_id, name, introduction, card, capacity);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    protocol::ClanPacketWriter writer;
    writer.WriteI32(1);
    writer.WriteU32(card);
    return ClanPackets{Packet(1028U, writer.Release())};
  }

  BusinessResult HandleScene(domain::PlayerId player_id,
                             protocol::ClanPacketReader& reader) {
    static_cast<void>(reader.ReadU32());
    const auto scene = reader.ReadU32();
    reader.RequireEnd();
    hub_.SetScene(player_id, scene);
    auto result = clans_.Roster(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& roster =
        std::get<std::vector<application::ClanMemberView>>(result);
    BroadcastRosters(roster);
    return ClanPackets{};
  }

  BusinessResult HandleChat(domain::PlayerId player_id,
                            protocol::ClanPacketReader& reader) {
    const auto channel = reader.ReadU32();
    static_cast<void>(reader.ReadText(32U));
    auto body = reader.ReadText(256U);
    reader.RequireEnd();
    auto result = clans_.SendChat(player_id, body);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& chat = std::get<application::ClanChatResult>(result);
    protocol::ClanPacketWriter writer;
    writer.WriteU32(channel);
    writer.WriteText(chat.sender_nickname);
    writer.WriteText(chat.body);
    auto event = writer.Release();
    for (const auto recipient : chat.recipients) {
      if (recipient != player_id) {
        hub_.Emit(recipient, Packet(1068U, event));
      }
    }
    return ClanPackets{Packet(1067U, U32Body(1U)),
                       Packet(1068U, std::move(event))};
  }

  BusinessResult HandleApply(domain::PlayerId player_id, std::uint16_t opcode,
                             protocol::ClanPacketReader& reader,
                             std::string_view actor_nickname) {
    const auto clan_id = reader.ReadU32();
    auto clan_name = reader.ReadText(32U);
    if (opcode == 1033U) {
      const auto claimed = reader.ReadText(32U);
      if (claimed != actor_nickname) {
        return MakeError(application::ErrorCode::kPermissionDenied,
                         "申请身份不正确");
      }
    }
    reader.RequireEnd();
    auto before = clans_.Home(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&before)) {
      return *error;
    }
    auto result = clans_.Apply(player_id, clan_id, clan_name);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& old_home = std::get<application::ClanHomeState>(before);
    const auto& new_home = std::get<application::ClanHomeState>(result);
    ClanPackets replies;
    if (old_home.clan.has_value() &&
        old_home.clan->clan_id == application::kTrainingClanId) {
      protocol::ClanPacketWriter departure;
      departure.WriteText(old_home.clan->name);
      departure.WriteU32(LegacyId(player_id.Value()));
      departure.WriteText(actor_nickname);
      departure.WriteU32(0U);
      departure.WriteI32(-20);
      replies.push_back(Packet(1037U, departure.Release()));
    }
    replies.push_back(Packet(1016U, protocol::EncodeClanHome(new_home)));
    protocol::ClanPacketWriter applied;
    applied.WriteU32(1U);
    applied.WriteU32(
        new_home.role_code.value_or(application::kClanApplicantRole));
    applied.WriteU32(clan_id);
    applied.WriteText(clan_name);
    replies.push_back(Packet(1034U, applied.Release()));
    if (new_home.clan.has_value()) {
      auto roster_result = clans_.Roster(player_id);
      if (const auto* roster =
              std::get_if<std::vector<application::ClanMemberView>>(
                  &roster_result)) {
        protocol::ClanPacketWriter event;
        event.WriteText(new_home.clan->name);
        event.WriteU32(LegacyId(player_id.Value()));
        event.WriteText(actor_nickname);
        event.WriteU32(application::kClanMemberRole);
        event.WriteU32(1U);
        Broadcast(*roster, 1041U, event.Release(), player_id);
        const auto wire = WireRoster(*roster);
        const auto regular = protocol::EncodeClanRoster(wire, false);
        const auto extended = protocol::EncodeClanRoster(wire, true);
        Broadcast(*roster, 1012U, regular, player_id);
        Broadcast(*roster, 1014U, extended, player_id);
        // 本人先处理 1016/1034 入队身份，再处理成员表，避免 UI 丢弃提前到达的
        // roster。
        replies.push_back(Packet(1012U, regular));
        replies.push_back(Packet(1014U, extended));
        const auto profile =
            protocol::EncodeClanProfile(new_home.clan, *roster);
        Broadcast(*roster, 1018U, profile, player_id);
        replies.push_back(Packet(1018U, profile));
      }
    }
    return replies;
  }

  BusinessResult HandleCancelApplication(domain::PlayerId player_id,
                                         protocol::ClanPacketReader& reader) {
    const auto clan_id = reader.ReadU32();
    reader.RequireEnd();
    auto result = clans_.CancelApplication(player_id, clan_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    protocol::ClanPacketWriter cancelled;
    cancelled.WriteU32(1U);
    cancelled.WriteU32(clan_id);
    return ClanPackets{
        Packet(1064U, cancelled.Release()),
        Packet(1016U, protocol::EncodeClanHome(
                          std::get<application::ClanHomeState>(result)))};
  }

  BusinessResult HandleApplicants(domain::PlayerId player_id,
                                  protocol::ClanPacketReader& reader) {
    reader.RequireEnd();
    auto result = clans_.Applicants(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    return ClanPackets{Packet(
        1070U,
        protocol::EncodeClanApplicants(
            std::get<std::vector<application::ClanApplicantView>>(result)))};
  }

  BusinessResult HandleMemberChange(domain::PlayerId player_id,
                                    protocol::ClanPacketReader& reader) {
    const domain::PlayerId target{reader.ReadU32()};
    auto nickname = reader.ReadText(32U);
    const auto old_role = reader.ReadI32();
    const auto new_role = reader.ReadI32();
    reader.RequireEnd();
    auto result =
        clans_.ChangeMember(player_id, target, nickname, old_role, new_role);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& changed = std::get<application::ClanMemberChangeResult>(result);
    protocol::ClanPacketWriter event_writer;
    event_writer.WriteText(changed.clan.name);
    event_writer.WriteU32(LegacyId(changed.target_player_id.Value()));
    event_writer.WriteText(changed.target_nickname);
    event_writer.WriteI32(changed.new_role);
    event_writer.WriteU32(changed.joined_from_application ? 1U : 0U);
    auto event = event_writer.Release();

    auto roster_result = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&roster_result)) {
      return *error;
    }
    const auto& roster =
        std::get<std::vector<application::ClanMemberView>>(roster_result);
    Broadcast(roster, 1041U, event, player_id);
    // 加入、晋升和降级后目标仍在 roster 中，前一行广播已经覆盖目标；踢出或
    // 拒绝申请时目标不在新 roster
    // 中，才需要单独投递，避免在线成员收到两次事件。
    if (changed.new_role < 0) {
      hub_.Emit(target, Packet(1041U, event));
    }
    auto target_home = clans_.Home(target);
    if (const auto* error =
            std::get_if<application::ServiceError>(&target_home)) {
      return *error;
    }
    hub_.Emit(
        target,
        Packet(1016U, protocol::EncodeClanHome(
                          std::get<application::ClanHomeState>(target_home))));
    BroadcastRosters(roster);

    protocol::ClanPacketWriter response;
    response.WriteU32(1U);
    response.WriteText(changed.target_nickname);
    response.WriteI32(changed.new_role);
    response.WriteU32(changed.joined_from_application ? 1U : 0U);
    auto profile = ProfilePackets(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&profile)) {
      return *error;
    }
    ClanPackets replies{Packet(1040U, response.Release())};
    auto values = std::get<ClanPackets>(std::move(profile));
    replies.insert(replies.end(), std::make_move_iterator(values.begin()),
                   std::make_move_iterator(values.end()));
    return replies;
  }

  BusinessResult HandleMarkRefresh(domain::PlayerId player_id,
                                   protocol::ClanPacketReader& reader) {
    const auto clan_id = reader.ReadU32();
    const auto mark = reader.ReadU32();
    reader.RequireEnd();
    auto profile_result = clans_.Profile(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&profile_result)) {
      return *error;
    }
    const auto& clan =
        std::get<std::optional<application::ClanSummary>>(profile_result);
    // 原版 990
    // 成功后发送的刷新信号；徽章必须已由网关事务提交，不能信任客户端写库。
    if (!clan || clan->clan_id != clan_id || clan->mark_code != mark) {
      return ClanPackets{};
    }
    auto roster_result = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&roster_result)) {
      return *error;
    }
    const auto& roster =
        std::get<std::vector<application::ClanMemberView>>(roster_result);
    const auto body = protocol::EncodeClanProfile(clan, roster);
    Broadcast(roster, 1018U, body, player_id);
    return ClanPackets{Packet(1018U, body)};
  }

  BusinessResult HandleRecruitment(domain::PlayerId player_id,
                                   protocol::ClanPacketReader& reader) {
    const auto clan_id = reader.ReadU32();
    const auto mode = reader.ReadU32();
    reader.RequireEnd();
    auto result = clans_.SetRecruitment(player_id, clan_id, mode);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& clan = std::get<application::ClanSummary>(result);
    protocol::ClanPacketWriter response;
    response.WriteU32(LegacyId(clan.clan_id));
    response.WriteU32(1U);
    response.WriteU32(clan.recruitment_mode);
    const auto body = response.Release();
    auto roster_result = clans_.Roster(player_id);
    if (const auto* roster =
            std::get_if<std::vector<application::ClanMemberView>>(
                &roster_result)) {
      // 使用完整资料更新其他成员；1103 只回给发起操作的客户端。
      Broadcast(*roster, 1018U, protocol::EncodeClanProfile(clan, *roster),
                player_id);
    }
    return ClanPackets{Packet(1103U, body)};
  }

  BusinessResult HandleEditText(domain::PlayerId player_id, bool introduction,
                                protocol::ClanPacketReader& reader) {
    auto value = reader.ReadText(introduction ? 100U : 256U);
    reader.RequireEnd();
    auto result = clans_.EditText(player_id, introduction, value);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto roster_result = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&roster_result)) {
      return *error;
    }
    protocol::ClanPacketWriter event;
    event.WriteText(value);
    Broadcast(std::get<std::vector<application::ClanMemberView>>(roster_result),
              introduction ? 1053U : 1050U, event.Release(), player_id);
    if (introduction) {
      return ClanPackets{Packet(1052U, U32Body(1U))};
    }
    protocol::ClanPacketWriter response;
    response.WriteU32(1U);
    response.WriteText(value);
    return ClanPackets{Packet(1049U, response.Release())};
  }

  BusinessResult HandleTransferRequest(domain::PlayerId player_id,
                                       protocol::ClanPacketReader& reader,
                                       std::string_view actor_nickname) {
    const domain::PlayerId target{reader.ReadU32()};
    reader.RequireEnd();
    if (!hub_.Connection(target)) {
      return MakeError(application::ErrorCode::kConflict,
                       "请选择在线的本战队成员");
    }
    auto result = clans_.RequestTransfer(player_id, target);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    protocol::ClanPacketWriter event;
    event.WriteText(actor_nickname);
    event.WriteU32(LegacyId(player_id.Value()));
    hub_.Emit(target, Packet(1059U, event.Release()));
    protocol::ClanPacketWriter response;
    response.WriteU8(1U);
    response.WriteU32(1U);
    return ClanPackets{Packet(1058U, response.Release())};
  }

  BusinessResult HandleTransferResponse(domain::PlayerId player_id,
                                        protocol::ClanPacketReader& reader,
                                        std::string_view actor_nickname) {
    const auto accepted_raw = reader.ReadU8();
    const domain::PlayerId source{reader.ReadU32()};
    reader.RequireEnd();
    if (accepted_raw > 1U) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "会长转让回复不正确");
    }
    auto result = clans_.RespondTransfer(player_id, source, accepted_raw == 1U);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    protocol::ClanPacketWriter event;
    event.WriteU8(accepted_raw);
    event.WriteU32(LegacyId(player_id.Value()));
    event.WriteText(actor_nickname);
    hub_.Emit(source, Packet(1062U, event.Release()));
    protocol::ClanPacketWriter response;
    response.WriteU8(1U);
    return ClanPackets{Packet(1061U, response.Release())};
  }

  BusinessResult HandleTransferFinish(domain::PlayerId player_id,
                                      protocol::ClanPacketReader& reader,
                                      std::string_view actor_nickname) {
    const domain::PlayerId target{reader.ReadU32()};
    auto nickname = reader.ReadText(32U);
    reader.RequireEnd();
    auto result = clans_.FinishTransfer(player_id, target, nickname);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    const auto& changed = std::get<application::ClanMemberChangeResult>(result);
    protocol::ClanPacketWriter event;
    event.WriteU32(1U);
    event.WriteU32(LegacyId(player_id.Value()));
    event.WriteText(actor_nickname);
    event.WriteU32(LegacyId(target.Value()));
    event.WriteText(changed.target_nickname);
    auto body = event.Release();
    auto roster_result = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&roster_result)) {
      return *error;
    }
    const auto& roster =
        std::get<std::vector<application::ClanMemberView>>(roster_result);
    Broadcast(roster, 1056U, body, player_id);
    BroadcastRosters(roster);
    return ClanPackets{Packet(1056U, std::move(body))};
  }

  BusinessResult HandleDisband(domain::PlayerId player_id,
                               protocol::ClanPacketReader& reader) {
    const auto kind = reader.ReadU32();
    reader.RequireEnd();
    if (kind != 1U && kind != 2U) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "战队解散参数不正确");
    }
    auto result = clans_.ScheduleDisband(player_id, kind == 1U);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto roster_result = clans_.Roster(player_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&roster_result)) {
      return *error;
    }
    protocol::ClanPacketWriter body_writer;
    body_writer.WriteU32(1U);
    body_writer.WriteU32(kind);
    auto body = body_writer.Release();
    Broadcast(std::get<std::vector<application::ClanMemberView>>(roster_result),
              1031U, body, player_id);
    return ClanPackets{Packet(1030U, body), Packet(1031U, std::move(body))};
  }

  BusinessResult HandlePosts(domain::PlayerId player_id,
                             protocol::ClanPacketReader& reader) {
    const auto kind = reader.ReadU32();
    const auto cursor = reader.ReadU32();
    reader.RequireEnd();
    auto result = clans_.Posts(player_id, kind, cursor);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    return ClanPackets{Packet(
        1022U, protocol::EncodeClanPosts(
                   std::get<std::vector<application::ClanPostView>>(result)))};
  }

  BusinessResult HandleAddPost(domain::PlayerId player_id,
                               protocol::ClanPacketReader& reader) {
    static_cast<void>(reader.ReadU32());
    auto body = reader.ReadText(256U);
    reader.RequireEnd();
    auto result = clans_.AddPost(player_id, body);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto posts = clans_.Posts(player_id, 1U, 0U);
    if (const auto* error = std::get_if<application::ServiceError>(&posts)) {
      return *error;
    }
    return ClanPackets{
        Packet(1024U, U32Body(1U)),
        Packet(1022U,
               protocol::EncodeClanPosts(
                   std::get<std::vector<application::ClanPostView>>(posts)))};
  }

  BusinessResult HandleDeletePost(domain::PlayerId player_id,
                                  protocol::ClanPacketReader& reader) {
    const domain::PlayerId claimed{reader.ReadU32()};
    const auto post_id = reader.ReadU32();
    reader.RequireEnd();
    auto result = clans_.DeletePost(player_id, claimed, post_id);
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return *error;
    }
    auto posts = clans_.Posts(player_id, 1U, 0U);
    if (const auto* error = std::get_if<application::ServiceError>(&posts)) {
      return *error;
    }
    return ClanPackets{
        Packet(1026U),
        Packet(1022U,
               protocol::EncodeClanPosts(
                   std::get<std::vector<application::ClanPostView>>(posts)))};
  }

  void CompleteBusiness(const protocol::ClanPacket& incoming,
                        BusinessResult result) {
    if (finalized_) {
      return;
    }
    handling_packet_ = false;
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      HandleBusinessError(incoming, *error);
    } else {
      for (const auto& response : std::get<ClanPackets>(result)) {
        EnqueueOnStrand(response);
      }
    }
    StartReadHeader();
  }

  void HandleBusinessError(const protocol::ClanPacket& incoming,
                           const application::ServiceError& error) {
    logger_.Log(
        application::LogLevel::kWarning,
        "Nx clan action rejected opcode=" + std::to_string(incoming.opcode) +
            " code=" + std::string{application::ErrorCodeName(error.code)},
        std::to_string(connection_id_));
    for (const auto& response : ErrorPackets(incoming)) {
      EnqueueOnStrand(response);
    }
    if (identity_.has_value()) {
      DispatchGateway(identity_->player_id,
                      {.opcode = 109U,
                       .body = protocol::EncodeNoticeResponse(
                           protocol::Utf8ToUtf16(error.safe_message))});
    }
  }

  ClanPackets ErrorPackets(const protocol::ClanPacket& incoming) const {
    protocol::ClanPacketWriter writer;
    switch (incoming.opcode) {
      case 1102U: {
        protocol::ClanPacketReader reader{incoming.body};
        writer.WriteU32(reader.ReadU32());
        writer.WriteU32(0U);  // 72C320 以非零判断成功，不能返回 -1。
        writer.WriteU32(reader.ReadU32());
        reader.RequireEnd();
        return {Packet(1103U, writer.Release())};
      }
      case 1042U: {
        protocol::ClanPacketReader reader{incoming.body};
        const auto kind = reader.ReadU32();
        const auto target = reader.ReadText(32U);
        const auto body = reader.ReadText(256U);
        reader.RequireEnd();
        writer.WriteU32(4U);
        writer.WriteU32(kind);
        writer.WriteText(target);
        writer.WriteText(target);
        writer.WriteText(body);
        return {Packet(1043U, writer.Release())};
      }
      case 1027U:
        writer.WriteI32(-100);
        writer.WriteU32(0U);
        return {Packet(1028U, writer.Release())};
      case 1106U:
        return {Packet(1107U, I32Body(-1))};
      case 1032U:
      case 1033U:
        writer.WriteI32(-1);
        writer.WriteU32(0U);
        writer.WriteU32(0U);
        writer.WriteText("");
        return {Packet(1034U, writer.Release())};
      case 1039U:
        writer.WriteI32(-1);
        writer.WriteText("");
        writer.WriteU32(0U);
        writer.WriteU32(0U);
        return {Packet(1040U, writer.Release())};
      case 1048U:
        writer.WriteI32(-1);
        writer.WriteText("");
        return {Packet(1049U, writer.Release())};
      case 1051U:
        return {Packet(1052U, I32Body(-1))};
      case 1057U:
        writer.WriteU8(0U);
        writer.WriteU32(2U);
        return {Packet(1058U, writer.Release())};
      case 1060U:
        writer.WriteU8(0U);
        return {Packet(1061U, writer.Release())};
      case 1063U:
        writer.WriteI32(-1);
        writer.WriteU32(0U);
        return {Packet(1064U, writer.Release())};
      case 1066U:
        return {Packet(1067U, U32Body(3U))};
      case 1023U:
        return {Packet(1024U, I32Body(-1))};
      case 1029U: {
        writer.WriteI32(-1);
        std::uint32_t kind = 0U;
        if (incoming.body.size() >= sizeof(std::uint32_t)) {
          protocol::PacketReader raw{incoming.body};
          kind = raw.ReadU32();
        }
        writer.WriteU32(kind);
        return {Packet(1030U, writer.Release())};
      }
      default:
        return {};
    }
  }

  std::vector<protocol::ClanRosterWireEntry> WireRoster(
      std::span<const application::ClanMemberView> roster) const {
    std::vector<protocol::ClanRosterWireEntry> result;
    result.reserve(roster.size());
    for (const auto& member : roster) {
      const auto online = gateway_sessions_.Lookup(member.player_id).online;
      result.push_back(
          {.member = member, .scene = hub_.Scene(member.player_id, online)});
    }
    return result;
  }

  void Broadcast(std::span<const application::ClanMemberView> roster,
                 std::uint16_t opcode, const std::vector<std::uint8_t>& body,
                 std::optional<domain::PlayerId> exclude = std::nullopt) const {
    for (const auto& member : roster) {
      if (!exclude.has_value() || member.player_id != *exclude) {
        hub_.Emit(member.player_id, Packet(opcode, body));
      }
    }
  }

  void BroadcastRosters(
      std::span<const application::ClanMemberView> roster) const {
    const auto wire = WireRoster(roster);
    const auto regular = protocol::EncodeClanRoster(wire, false);
    const auto extended = protocol::EncodeClanRoster(wire, true);
    for (const auto& member : roster) {
      hub_.Emit(member.player_id, Packet(1012U, regular));
      hub_.Emit(member.player_id, Packet(1014U, extended));
    }
  }

  void DispatchGateway(domain::PlayerId player_id,
                       runtime::OutboundMessage message) const {
    runtime::SocialCommandResult command;
    command.deliveries.push_back(
        {.recipient = player_id, .message = std::move(message)});
    const auto result = gateway_sessions_.Dispatch(std::move(command));
    static_cast<void>(result);
  }

  bool ConsumeChatQuota() {
    const auto now = std::chrono::steady_clock::now();
    while (!chat_times_.empty() &&
           chat_times_.front() <= now - std::chrono::seconds{10}) {
      chat_times_.pop_front();
    }
    if (chat_times_.size() >= 8U) {
      return false;
    }
    chat_times_.push_back(now);
    return true;
  }

  void EnqueueOnStrand(const protocol::ClanPacket& value) {
    if (finalized_) {
      return;
    }
    try {
      auto wire = protocol::EncodeClanFrame(value);
      if (wire.size() > kMaximumSendQueueBytes ||
          queued_bytes_ > kMaximumSendQueueBytes - wire.size()) {
        logger_.Log(application::LogLevel::kWarning,
                    "Nx clan send queue limit reached",
                    std::to_string(connection_id_));
        Finalize();
        return;
      }
      queued_bytes_ += wire.size();
      send_queue_.push_back(std::move(wire));
      StartWrite();
    } catch (const protocol::ProtocolError&) {
      Finalize();
    }
  }

  void StartWrite() {
    if (finalized_ || write_in_progress_ || send_queue_.empty()) {
      return;
    }
    write_in_progress_ = true;
    boost::asio::async_write(
        socket_, boost::asio::buffer(send_queue_.front()),
        [self = shared_from_this()](const boost::system::error_code& error,
                                    std::size_t transferred) {
          self->HandleWrite(error, transferred);
        });
  }

  void HandleWrite(const boost::system::error_code& error,
                   std::size_t transferred) {
    if (finalized_) {
      return;
    }
    if (error || send_queue_.empty() ||
        transferred != send_queue_.front().size()) {
      Finalize();
      return;
    }
    queued_bytes_ -= send_queue_.front().size();
    send_queue_.pop_front();
    write_in_progress_ = false;
    StartWrite();
  }

  template <typename Rep, typename Period>
  void ArmTimeout(std::chrono::duration<Rep, Period> timeout) {
    timer_.expires_after(timeout);
    timer_.async_wait(
        [self = shared_from_this()](const boost::system::error_code& error) {
          if (!error) {
            self->Finalize();
          }
        });
  }

  void Finalize() noexcept {
    if (finalized_) {
      return;
    }
    finalized_ = true;
    stop_requested_.store(true, std::memory_order_release);
    try {
      boost::system::error_code ignored;
      ObserveIoResult(timer_.cancel());
      if (identity_.has_value()) {
        hub_.Unbind(identity_->player_id, this);
      }
      ObserveIoResult(socket_.cancel(ignored));
      ObserveIoResult(socket_.shutdown(tcp::socket::shutdown_both, ignored));
      ObserveIoResult(socket_.close(ignored));
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "Nx clan session cleanup failed",
                  std::to_string(connection_id_));
    }
    try {
      closed_callback_(connection_id_);
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "Nx clan close callback failed",
                  std::to_string(connection_id_));
    }
  }

  std::uint64_t connection_id_{0U};
  tcp::socket socket_;
  boost::asio::steady_timer timer_;
  const std::size_t kMaximumSendQueueBytes;
  application::BackgroundTaskExecutor& database_tasks_;
  application::ClanExecutor& clans_;
  application::PlayerMenuSnapshotLoader& menu_snapshots_;
  runtime::ClanTicketBroker& tickets_;
  runtime::SessionRegistry& gateway_sessions_;
  const application::SystemClock& clock_;
  application::EventLogger& logger_;
  ClanHub& hub_;
  ClosedCallback closed_callback_;
  std::optional<runtime::ClanSessionIdentity> identity_;
  std::optional<std::pair<std::uint64_t, std::uint32_t>> last_activity_level_;
  std::array<std::uint8_t, protocol::kClanFrameHeaderBytes> read_header_{};
  std::vector<std::uint8_t> read_body_;
  std::uint16_t pending_opcode_{0U};
  std::deque<std::vector<std::uint8_t>> send_queue_;
  std::deque<std::chrono::steady_clock::time_point> chat_times_;
  std::size_t queued_bytes_{0U};
  std::atomic<std::size_t> externally_pending_bytes_{0U};
  std::atomic<bool> stop_requested_{false};
  bool write_in_progress_{false};
  bool handling_packet_{false};
  bool finalized_{false};
};

}  // namespace

class ClanTcpServer::Impl final {
 public:
  Impl(ClanTcpServerOptions options,
       application::BackgroundTaskExecutor& database_tasks,
       application::ClanExecutor& clans,
       application::PlayerMenuSnapshotLoader& menu_snapshots,
       runtime::ClanTicketBroker& tickets,
       runtime::SessionRegistry& gateway_sessions,
       const application::SystemClock& clock, application::EventLogger& logger)
      : options_(std::move(options)),
        database_tasks_(database_tasks),
        clans_(clans),
        menu_snapshots_(menu_snapshots),
        tickets_(tickets),
        gateway_sessions_(gateway_sessions),
        clock_(clock),
        logger_(logger),
        acceptor_(io_context_) {}

  std::optional<application::ServiceError> Start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Nx 战队服务器只能启动一次");
    }
    if (options_.bind_address.empty() || options_.io_threads == 0U ||
        options_.maximum_connections == 0U ||
        options_.maximum_send_queue_bytes <
            protocol::kClanFrameHeaderBytes + 4U) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "Nx 战队服务器配置无效");
    }
    try {
      const tcp::endpoint endpoint{
          boost::asio::ip::make_address(options_.bind_address), options_.port};
      acceptor_.open(endpoint.protocol());
      acceptor_.set_option(tcp::acceptor::reuse_address{true});
      acceptor_.bind(endpoint);
      acceptor_.listen(boost::asio::socket_base::max_listen_connections);
      local_port_.store(acceptor_.local_endpoint().port(),
                        std::memory_order_release);
      running_.store(true, std::memory_order_release);
      AcceptNext();
      threads_.reserve(options_.io_threads);
      for (std::size_t index = 0U; index < options_.io_threads; ++index) {
        threads_.emplace_back([this] {
          try {
            io_context_.run();
          } catch (...) {
            logger_.Log(application::LogLevel::kError,
                        "Nx clan I/O worker terminated by exception");
          }
        });
      }
      logger_.Log(application::LogLevel::kDebug,
                  "Clan listener ready: address=" + options_.bind_address +
                      " port=" + std::to_string(LocalPort()));
      return std::nullopt;
    } catch (const boost::system::system_error&) {
      running_.store(false, std::memory_order_release);
      CloseAcceptor();
      io_context_.stop();
      JoinThreads();
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "Nx 战队监听端点打开或绑定失败", true);
    } catch (...) {
      running_.store(false, std::memory_order_release);
      CloseAcceptor();
      io_context_.stop();
      JoinThreads();
      return MakeError(application::ErrorCode::kInternal,
                       "Nx 战队服务器启动失败");
    }
  }

  void Stop(std::chrono::milliseconds grace) noexcept {
    try {
      StopImpl(grace);
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "Nx clan graceful stop failed; forcing shutdown");
      ForceStopAfterException();
    }
  }

  void StopImpl(std::chrono::milliseconds grace) {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
      JoinThreads();
      return;
    }
    try {
      boost::asio::post(io_context_, [this] {
        CloseAcceptor();
        std::vector<std::shared_ptr<ClanSession>> snapshot;
        {
          std::scoped_lock lock{sessions_mutex_};
          snapshot.reserve(sessions_.size());
          for (const auto& [id, session] : sessions_) {
            static_cast<void>(id);
            snapshot.push_back(session);
          }
        }
        for (const auto& session : snapshot) {
          session->RequestStop();
        }
      });
      std::unique_lock lock{sessions_mutex_};
      sessions_closed_.wait_for(lock, grace,
                                [this] { return sessions_.empty(); });
    } catch (...) {
      logger_.Log(application::LogLevel::kWarning,
                  "Nx clan graceful session drain failed");
    }
    io_context_.stop();
    JoinThreads();
    {
      std::scoped_lock lock{sessions_mutex_};
      sessions_.clear();
    }
    local_port_.store(0U, std::memory_order_release);
    logger_.Log(application::LogLevel::kDebug, "Clan listener stopped");
  }

  [[nodiscard]] std::uint16_t LocalPort() const noexcept {
    return local_port_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept {
    std::scoped_lock lock{sessions_mutex_};
    return sessions_.size();
  }

 private:
  void AcceptNext() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    acceptor_.async_accept(
        boost::asio::make_strand(io_context_),
        [this](const boost::system::error_code& error, tcp::socket socket) {
          if (!error && running_.load(std::memory_order_acquire)) {
            AcceptSocket(std::move(socket));
          } else if (error != boost::asio::error::operation_aborted &&
                     running_.load(std::memory_order_acquire)) {
            logger_.Log(application::LogLevel::kWarning,
                        "Nx clan TCP accept failed");
          }
          AcceptNext();
        });
  }

  void AcceptSocket(tcp::socket socket) {
    std::shared_ptr<ClanSession> session;
    {
      std::scoped_lock lock{sessions_mutex_};
      if (sessions_.size() >= options_.maximum_connections) {
        boost::system::error_code ignored;
        ObserveIoResult(socket.close(ignored));
        logger_.Log(application::LogLevel::kWarning,
                    "Nx clan connection limit reached");
        return;
      }
      const auto connection_id = next_connection_id_++;
      session = std::make_shared<ClanSession>(
          connection_id, std::move(socket), options_.maximum_send_queue_bytes,
          database_tasks_, clans_, menu_snapshots_, tickets_, gateway_sessions_,
          clock_, logger_, hub_,
          [this](std::uint64_t closed_id) { SessionClosed(closed_id); });
      sessions_.emplace(connection_id, session);
    }
    session->Start();
  }

  void SessionClosed(std::uint64_t connection_id) {
    {
      std::scoped_lock lock{sessions_mutex_};
      sessions_.erase(connection_id);
    }
    sessions_closed_.notify_all();
  }

  void CloseAcceptor() noexcept {
    boost::system::error_code ignored;
    ObserveIoResult(acceptor_.cancel(ignored));
    ObserveIoResult(acceptor_.close(ignored));
  }

  void JoinThreads() {
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  /** @brief 正常停机异常时收束
   * I/O、线程和会话；再次失败表示进程状态已不可恢复。 */
  void ForceStopAfterException() noexcept {
    try {
      io_context_.stop();
      JoinThreads();
      {
        std::scoped_lock lock{sessions_mutex_};
        sessions_.clear();
      }
      local_port_.store(0U, std::memory_order_release);
    } catch (...) {
      std::terminate();
    }
  }

  ClanTcpServerOptions options_;
  application::BackgroundTaskExecutor& database_tasks_;
  application::ClanExecutor& clans_;
  application::PlayerMenuSnapshotLoader& menu_snapshots_;
  runtime::ClanTicketBroker& tickets_;
  runtime::SessionRegistry& gateway_sessions_;
  const application::SystemClock& clock_;
  application::EventLogger& logger_;
  ClanHub hub_;
  boost::asio::io_context io_context_;
  tcp::acceptor acceptor_;
  mutable std::mutex sessions_mutex_;
  std::condition_variable sessions_closed_;
  std::map<std::uint64_t, std::shared_ptr<ClanSession>> sessions_;
  std::vector<std::thread> threads_;
  std::atomic<bool> started_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint16_t> local_port_{0U};
  std::uint64_t next_connection_id_{1U};
};

ClanTcpServer::ClanTcpServer(
    ClanTcpServerOptions options,
    application::BackgroundTaskExecutor& database_tasks,
    application::ClanExecutor& clans,
    application::PlayerMenuSnapshotLoader& menu_snapshots,
    runtime::ClanTicketBroker& tickets,
    runtime::SessionRegistry& gateway_sessions,
    const application::SystemClock& clock, application::EventLogger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), database_tasks, clans,
                                   menu_snapshots, tickets, gateway_sessions,
                                   clock, logger)) {}

ClanTcpServer::~ClanTcpServer() { impl_->Stop(std::chrono::milliseconds{0}); }

std::optional<application::ServiceError> ClanTcpServer::Start() {
  return impl_->Start();
}

void ClanTcpServer::Stop(std::chrono::milliseconds grace) noexcept {
  impl_->Stop(grace);
}

std::uint16_t ClanTcpServer::LocalPort() const noexcept {
  return impl_->LocalPort();
}

std::size_t ClanTcpServer::ActiveSessionCount() const noexcept {
  return impl_->ActiveSessionCount();
}

}  // namespace bf::infrastructure::network
