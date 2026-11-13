#include "network/room_ds_service.h"

#include <algorithm>
#include <stdexcept>

#include "protocol/codec/packet_io.h"

namespace bf::infrastructure::network {
using Json = nlohmann::json;
using protocol::internal::RoomEnvelope;
using protocol::internal::RoomMessageKind;
namespace asio = boost::asio;

namespace {
Json ResultValue(
    application::ServiceResult<runtime::RoomCoordinatorResult> result) {
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return WriteRoomError(*error);
  return WriteRoomResult(
      std::get<runtime::RoomCoordinatorResult>(std::move(result)));
}

template <typename T>
Json BoolValue(application::ServiceResult<T> result) {
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return WriteRoomError(*error);
  return {{"ok", true}, {"changed", std::get<T>(result)}};
}
}  // namespace

RoomDsService::RoomDsService(asio::io_context& io, RoomDsOptions options)
    : io_(io),
      options_(std::move(options)),
      acceptor_(io),
      timer_(io),
      lease_deadline_(std::chrono::steady_clock::now() +
                      options_.bind_timeout) {
  if (options_.bind_timeout <= std::chrono::milliseconds::zero() ||
      options_.lease_duration < std::chrono::seconds{2} ||
      options_.lease_duration > std::chrono::seconds{10})
    throw std::invalid_argument{"invalid room process deadline"};
  ConfigureRoomTls(tls_, options_.tls);
  const asio::ip::tcp::endpoint endpoint{
      asio::ip::make_address(options_.bind_address), options_.port};
  acceptor_.open(endpoint.protocol());
  acceptor_.bind(endpoint);
  acceptor_.listen(1);
}

void RoomDsService::Start() {
  acceptor_.async_accept([this](auto error, auto socket) {
    if (error || stopped_) return;
    channel_ = std::make_shared<RoomTlsChannel>(std::move(socket), tls_,
                                                options_.tls.peer_name);
    channel_->Start(
        [this](RoomEnvelope message) { Receive(std::move(message)); }, {},
        [this] { Stop(); });
    // A room process has exactly one authenticated gateway for its whole
    // lifetime.
    boost::system::error_code ignored;
    acceptor_.close(ignored);
  });
  Schedule();
}

void RoomDsService::Stop() {
  if (stopped_) return;
  stopped_ = true;
  timer_.cancel();
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  if (channel_) channel_->Close();
  io_.stop();
}

std::uint16_t RoomDsService::Port() const {
  return acceptor_.local_endpoint().port();
}

void RoomDsService::Receive(RoomEnvelope message) {
  if (std::chrono::steady_clock::now() >= lease_deadline_) {
    Stop();
    return;
  }
  try {
    const auto payload = ParseRoomPayload(message.payload);
    auto value = Execute(message, payload);
    Respond(std::move(message), std::move(value), RoomMessageKind::kReply);
  } catch (const std::exception&) {
    Respond(std::move(message),
            {{"ok", false}, {"detail", "invalid internal room request"}},
            RoomMessageKind::kError);
  }
  Schedule();
}

Json RoomDsService::Execute(const RoomEnvelope& message, const Json& payload) {
  const auto now = std::chrono::steady_clock::now();
  if (!message.request_id)
    throw std::invalid_argument{"request id is required"};
  const auto lease_budget = [&] {
    const auto milliseconds =
        payload.value("lease_ms", options_.lease_duration.count());
    if (milliseconds <= 0 || milliseconds > options_.lease_duration.count())
      throw std::invalid_argument{"invalid confirmed lease budget"};
    return std::chrono::milliseconds{milliseconds};
  };
  if (!room_) {
    if (message.kind != RoomMessageKind::kBind || !message.identity.IsValid() ||
        !message.sequence || message.room_generation != 0U ||
        payload.at("op") != "bind")
      throw std::invalid_argument{"room must be bound once before use"};
    const auto capacity = payload.value("capacity", 12U);
    if (capacity > 16U) throw std::invalid_argument{"invalid room capacity"};
    room_ = std::make_unique<runtime::DedicatedRoom>(
        message.identity, options_.timing,
        static_cast<std::uint16_t>(capacity));
    control_sequence_ = message.sequence;
    lease_deadline_ = now + lease_budget();
    return {{"ok", true}, {"bound", true}};
  }
  if (message.identity != room_->Identity() ||
      (message.kind != RoomMessageKind::kHeartbeat && !room_->Accepts(message)))
    throw std::invalid_argument{"stale room identity or generation"};
  if (message.kind == RoomMessageKind::kHeartbeat) {
    if (message.sequence <= control_sequence_)
      throw std::invalid_argument{"stale control sequence"};
    control_sequence_ = message.sequence;
    lease_deadline_ = now + lease_budget();
    const auto timing = timer_lateness_.Snapshot();
    return {{"ok", true},
            {"lease_ms", options_.lease_duration.count()},
            {"timer_samples", timing.samples},
            {"timer_p99_us", timing.p99_us}};
  }
  if (message.kind != RoomMessageKind::kCommand)
    throw std::invalid_argument{"invalid room command kind"};
  const auto operation = payload.at("op").get<std::string>();
  if (operation == "packet") {
    if (message.match_id != (installed_match_ ? installed_match_->Bytes()
                                              : domain::MatchId::ByteArray{}))
      throw std::invalid_argument{"stale match identity"};
    if (pending_start_ || business_paused_)
      return {{"ok", false},
              {"detail", "room persistence pending"},
              {"retryable", true}};
    const auto raw_opcode = payload.at("opcode").get<unsigned>();
    if (raw_opcode > 65535U)
      throw std::invalid_argument{"invalid client opcode"};
    protocol::Packet packet{.opcode = static_cast<std::uint16_t>(raw_opcode),
                            .body = {}};
    const auto& body = payload.at("body");
    if (!body.is_array() || body.size() > 65535U)
      throw std::invalid_argument{"invalid client payload size"};
    for (const auto& byte : body) {
      const auto number = byte.get<unsigned>();
      if (number > 255U)
        throw std::invalid_argument{"invalid client payload byte"};
      packet.body.push_back(static_cast<std::uint8_t>(number));
    }
    if (packet.opcode == 513U) {
      const auto request =
          protocol::DecodeRoomConfigurationRequest(packet.body);
      if (request.mode_id != 1U || request.submode_id == 4U)
        return {{"ok", false},
                {"detail",
                 "mode has not passed dedicated-room migration acceptance"}};
    }
    runtime::RoomActorContext actor{.player_id = message.player_id};
    if (payload.contains("player"))
      actor.player_snapshot = ReadRoomPlayer(payload.at("player"));
    actor.peer_address = payload.value("peer_address", std::string{});
    auto handled = room_->Command(message, std::move(actor), packet, now);
    if (auto* result = std::get_if<runtime::RoomCoordinatorResult>(&handled);
        result && result->event == runtime::RoomEvent::kMatchStarted) {
      pending_start_ = std::move(*result);
      return StartRequest();
    }
    if (const auto* result =
            std::get_if<runtime::RoomCoordinatorResult>(&handled);
        result && result->event == runtime::RoomEvent::kReturnedToWaiting)
      installed_match_.reset();
    return ResultValue(std::move(handled));
  }
  // Control mutations have a separate sequence from every player's input
  // stream.
  if (message.sequence <= control_sequence_)
    throw std::invalid_argument{"duplicate internal control"};
  control_sequence_ = message.sequence;
  auto& coordinator = room_->Coordinator();
  const auto id = room_->Identity().room_id;
  if (operation == "install") {
    if (!pending_start_ || installed_match_)
      throw std::invalid_argument{"no pending start"};
    auto installation = ReadRoomInstallation(payload.at("installation"));
    if (installation.match_id->Bytes() != message.match_id)
      throw std::invalid_argument{"install receipt match mismatch"};
    auto installed = coordinator.InstallCombat(id, installation);
    if (const auto* error = std::get_if<application::ServiceError>(&installed))
      return WriteRoomError(*error);
    if (auto runtime = coordinator.FindRoom(id))
      runtime->BeginAuthorizedMatchClock(now);
    installed_match_ = installation.match_id;
    auto out = WriteRoomResult(*pending_start_);
    pending_start_.reset();
    auto initial = ResultValue(coordinator.InitialMatchEffects(id, now));
    out["initial"] = std::move(initial);
    return out;
  }
  if (operation == "abort_start") {
    if (!pending_start_) throw std::invalid_argument{"no pending start"};
    auto aborted = coordinator.AbortFailedMatchStart(id, "Start failed");
    if (std::holds_alternative<runtime::RoomCoordinatorResult>(aborted))
      pending_start_.reset();
    return ResultValue(std::move(aborted));
  }
  if (operation == "settlement_staged") {
    const auto match = domain::MatchId::FromBytes(message.match_id);
    if (!match || installed_match_ != match)
      throw std::invalid_argument{"settlement match mismatch"};
    auto acknowledged = coordinator.AcknowledgeSettlementStaged(id, *match);
    if (std::holds_alternative<bool>(acknowledged)) terminal_pending_ = false;
    return BoolValue(std::move(acknowledged));
  }
  if (operation == "settlement_result") {
    auto settlement = ReadRoomSettlement(payload.at("settlement"));
    if (settlement.match_id.Bytes() != message.match_id ||
        installed_match_ != settlement.match_id)
      throw std::invalid_argument{"settlement result match mismatch"};
    return ResultValue(coordinator.ApplySettlementResult(settlement, now));
  }
  if (operation == "equipment")
    return ResultValue(
        coordinator.EquipmentChanged(ReadRoomPlayer(payload.at("player"))));
  if (operation == "assets")
    return ResultValue(
        coordinator.AssetsCommitted(ReadRoomPlayer(payload.at("player"))));
  if (operation == "level_icon")
    return ResultValue(
        coordinator.LevelIconChanged(ReadRoomPlayer(payload.at("player"))));
  if (operation == "couple")
    return ResultValue(
        coordinator.CoupleChanged(ReadRoomPlayer(payload.at("player"))));
  if (operation == "business_begin" || operation == "respawn_begin") {
    if (business_paused_ || pending_start_ || terminal_pending_)
      return {{"ok", false},
              {"detail", "room persistence pending"},
              {"retryable", true}};
    const auto runtime = coordinator.FindRoom(message.player_id);
    if (!runtime) throw std::invalid_argument{"business player not in room"};
    if (operation == "respawn_begin") {
      pending_respawn_ = coordinator.PrepareInstantRespawn(
          message.player_id, payload.at("object").get<std::uint32_t>());
      business_paused_ = pending_respawn_.has_value();
      if (!pending_respawn_) return {{"ok", true}, {"allowed", false}};
      return {{"ok", true},
              {"allowed", true},
              {"item_id", pending_respawn_->item_id.Value()},
              {"match_id", pending_respawn_->match_id->Bytes()},
              {"life_generation", pending_respawn_->life_generation}};
    }
    const bool allowed = runtime->Phase() == runtime::RoomPhase::kWaiting ||
                         (payload.value("preset", false) &&
                          coordinator.CanChangeCombatPreset(message.player_id));
    business_paused_ = allowed;
    return {{"ok", true},
            {"allowed", allowed},
            {"combat_mode", runtime->Phase() == runtime::RoomPhase::kPlaying
                                ? runtime->ModeId()
                                : 0U}};
  }
  if (operation == "business_end") {
    Json result = WriteRoomResult(runtime::RoomCoordinatorResult{});
    if (payload.contains("player")) {
      if (!business_paused_)
        throw std::invalid_argument{"equipment update without reservation"};
      result = ResultValue(payload.value("level_icon", false)
                               ? coordinator.LevelIconChanged(
                                     ReadRoomPlayer(payload.at("player")))
                               : coordinator.EquipmentChanged(
                                     ReadRoomPlayer(payload.at("player"))));
    }
    business_paused_ = false;
    return result;
  }
  if (operation == "respawn_end") {
    const bool authorized =
        pending_respawn_ &&
        (!payload.value("authorized", false) ||
         coordinator.AuthorizeInstantRespawn(*pending_respawn_));
    pending_respawn_.reset();
    business_paused_ = false;
    return {{"ok", authorized}};
  }
  if (operation == "finish") {
    if (!installed_match_ || message.match_id != installed_match_->Bytes())
      throw std::invalid_argument{"finish match mismatch"};
    return ResultValue(coordinator.FinishMatch(
        id, application::MatchCompletionKind::kCompleted, now));
  }
  if (operation == "inspect") {
    Json members = Json::array();
    for (const auto& member : room_->Members())
      members.push_back(
          {{"player_id", member.player_id.Value()},
           {"seat", member.seat},
           {"team", member.team},
           {"ready", member.ready},
           {"object_id",
            member.object_id ? Json(*member.object_id) : Json(nullptr)}});
    return {{"ok", true},
            {"members", members},
            {"closed", room_->Closed()},
            {"pending_start", pending_start_.has_value()},
            {"queued_bytes", channel_->QueuedBytes()}};
  }
  throw std::invalid_argument{"unsupported internal room operation"};
}

Json RoomDsService::StartRequest() {
  Json participants = Json::array();
  for (const auto& member : room_->Members()) {
    if (!member.IsObserver())
      participants.push_back(
          {{"player_id", member.player_id.Value()},
           {"seat_index", member.seat},
           {"team_code", member.team},
           {"actor_object_id",
            member.object_id ? Json(*member.object_id) : Json(nullptr)}});
  }
  auto activation = *pending_start_;
  std::erase_if(activation.deliveries,
                [](const auto& row) { return row.message.opcode == 704U; });
  activation.session_updates.clear();
  std::erase_if(pending_start_->deliveries,
                [](const auto& row) { return row.message.opcode != 704U; });
  const auto runtime = room_->Coordinator().FindRoom(room_->Identity().room_id);
  Json viewers = Json::array();
  for (const auto& member : room_->Members()) {
    const auto profile = room_->Coordinator().PlayerSnapshot(member.player_id);
    viewers.push_back({{"player_id", member.player_id.Value()},
                       {"seat", member.seat},
                       {"nickname", profile->nickname}});
  }
  return {{"ok", true},
          {"start_required", true},
          {"participants", std::move(participants)},
          {"mode_id", runtime->ModeId()},
          {"restricted_items", runtime->RestrictedItems()},
          {"viewers", std::move(viewers)},
          {"activation", WriteRoomResult(activation)}};
}

bool RoomDsService::CanExit() const {
  if (!room_ || !room_->Closed() || pending_start_) return false;
  // A closed room can still own the only terminal snapshot until the journal
  // receipt.
  return std::holds_alternative<application::ServiceError>(
      room_->Coordinator().PendingSettlement(room_->Identity().room_id));
}

void RoomDsService::Respond(RoomEnvelope envelope, Json value,
                            RoomMessageKind kind) {
  envelope.kind = kind;
  if (room_) {
    envelope.identity = room_->Identity();
    envelope.room_generation = room_->Generation();
    value["closed"] = room_->Closed();
    envelope.match_id = installed_match_ ? installed_match_->Bytes()
                                         : domain::MatchId::ByteArray{};
    const auto event = value.value("event", runtime::RoomEvent::kNoChange);
    if (event != runtime::RoomEvent::kNoChange &&
        event != runtime::RoomEvent::kCombatTickDue &&
        event != runtime::RoomEvent::kModeObjectiveDue &&
        event != runtime::RoomEvent::kLoadProgress) {
      if (const auto scene =
              room_->Coordinator().Scene(room_->Identity().room_id)) {
        protocol::PacketWriter out;
        protocol::EncodeSceneInfo(out, *scene);
        value["scene"] = out.Release();
      }
    }
    if (value.contains("settlement") && !value.at("settlement").is_null()) {
      terminal_pending_ = true;
      value["settlement"]["room_owner"] = {
          {"instance_id", room_->Identity().instance_id},
          {"owner_epoch", room_->Identity().owner_epoch}};
    }
  }
  const auto bytes = value.dump();
  envelope.payload.assign(bytes.begin(), bytes.end());
  if (!channel_->Send(std::move(envelope))) Stop();
}

void RoomDsService::Schedule() {
  if (stopped_) return;
  timer_.cancel();
  auto deadline = lease_deadline_;
  if (room_ && !pending_start_ && !business_paused_)
    for (const auto& token : room_->Timers()) {
      if (terminal_pending_ &&
          (token.kind == runtime::RoomTimerKind::kResultsDisplay ||
           token.kind == runtime::RoomTimerKind::kResultsReturn))
        continue;
      deadline = std::min(deadline, token.deadline);
    }
  if (CanExit())
    deadline = std::min(deadline, std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds{100});
  timer_.expires_at(deadline);
  timer_.async_wait([this](auto error) {
    if (error || stopped_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= lease_deadline_ ||
        (CanExit() && channel_->QueuedBytes() == 0U)) {
      Stop();
      return;
    }
    if (room_ && !pending_start_ && !business_paused_) {
      for (const auto& token : room_->Timers()) {
        if (token.deadline > now) continue;
        if (terminal_pending_ &&
            (token.kind == runtime::RoomTimerKind::kResultsDisplay ||
             token.kind == runtime::RoomTimerKind::kResultsReturn))
          continue;
        timer_lateness_.Observe(now - token.deadline);
        auto applied = room_->ApplyTimer(token, now);
        auto* result = std::get_if<runtime::RoomCoordinatorResult>(&applied);
        if (result && result->event == runtime::RoomEvent::kMatchStarted) {
          pending_start_ = std::move(*result);
          RoomEnvelope event;
          Respond(std::move(event), StartRequest(), RoomMessageKind::kEvent);
          break;
        }
        if (result && result->event == runtime::RoomEvent::kReturnedToWaiting)
          installed_match_.reset();
        RoomEnvelope event;
        Respond(std::move(event), ResultValue(std::move(applied)),
                RoomMessageKind::kEvent);
      }
    }
    Schedule();
  });
}
}  // namespace bf::infrastructure::network
