#include "features/rooms/room_coordinator.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "application/utf8.h"
#include "features/battle/event_mole_profile.h"
#include "features/practice/horde_tutor_profile.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"

namespace bf::runtime {
namespace {

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

[[nodiscard]] bool IsUnicodeWhitespace(char32_t value) noexcept {
  return value == U' ' || value == 0x85 || value == 0xA0 || value == 0x1680 ||
         (value >= 0x2000 && value <= 0x200A) || value == 0x2028 ||
         value == 0x2029 || value == 0x202F || value == 0x205F ||
         value == 0x3000;
}

[[nodiscard]] bool ValidRoomName(std::string_view name) noexcept {
  bool has_visible_code_point = false;
  for (std::size_t offset = 0U; offset < name.size();) {
    const auto decoded = domain::DecodeUtf8CodePoint(name, offset);
    if (!decoded.has_value() || decoded->value < 0x20) {
      return false;
    }
    has_visible_code_point =
        has_visible_code_point || !IsUnicodeWhitespace(decoded->value);
    offset += decoded->width;
  }
  return has_visible_code_point;
}

[[nodiscard]] RoomCoordinatorResult SingleDelivery(
    domain::PlayerId recipient, std::uint16_t opcode,
    std::vector<std::uint8_t> body) {
  RoomCoordinatorResult result;
  result.deliveries.push_back(
      {.recipient = recipient,
       .message = {.opcode = opcode, .body = std::move(body)}});
  return result;
}

void AppendNotice(RoomCoordinatorResult& result, domain::PlayerId recipient,
                  std::string_view message) {
  result.deliveries.push_back(
      {.recipient = recipient,
       .message = {.opcode = 109U,
                   .body = protocol::EncodeNoticeResponse(
                       protocol::Utf8ToUtf16(message))}});
}

[[nodiscard]] bool SnapshotMatches(const RoomActorContext& actor) noexcept {
  return !actor.player_snapshot.has_value() ||
         actor.player_snapshot->player_id == actor.player_id;
}

}  // namespace

std::optional<application::UserHostPrizeTerms> RoomCoordinator::UserHostPrize(
    domain::RoomId room_id) const {
  const auto entry = entries_.find(room_id);
  return entry == entries_.end() ? std::nullopt
                                 : entry->second.flow->UserHostPrize();
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::AssetsCommitted(application::PlayerMenuSnapshot snapshot) {
  const auto room = registry_.FindByPlayer(snapshot.player_id);
  if (!room) return RoomCoordinatorResult{};
  auto result =
      entries_.at(room->RoomId()).flow->AssetsCommitted(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return *error;
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(result)));
}

RoomCoordinator::RoomCoordinator(RoomMatchTiming timing,
                                 std::optional<domain::RoomId> isolated_room,
                                 std::uint16_t isolated_capacity)
    : timing_(timing),
      isolated_capacity_(isolated_capacity),
      registry_(isolated_room) {}

std::optional<application::UseItemCommand>
RoomCoordinator::PrepareInstantRespawn(domain::PlayerId player,
                                       std::uint32_t object) const {
  const auto room = registry_.FindByPlayer(player);
  return room ? entries_.at(room->RoomId())
                    .flow->PrepareInstantRespawn(player, object)
              : std::nullopt;
}

bool RoomCoordinator::AuthorizeInstantRespawn(
    const application::UseItemCommand& command) {
  const auto room = registry_.FindByPlayer(command.player_id);
  return room &&
         entries_.at(room->RoomId()).flow->AuthorizeInstantRespawn(command);
}

bool RoomCoordinator::CanChangeCombatPreset(
    domain::PlayerId player) const noexcept {
  const auto room = registry_.FindByPlayer(player);
  return room &&
         entries_.at(room->RoomId()).flow->CanChangeCombatPreset(player);
}

RoomCoordinatorResult RoomCoordinator::ClanMarkChanged(domain::PlayerId player,
                                                       std::uint32_t mark) {
  const auto room = registry_.FindByPlayer(player);
  if (!room) {
    return RoomCoordinatorResult{};
  }
  return Decorate(
      room->RoomId(),
      entries_.at(room->RoomId()).flow->ClanMarkChanged(player, mark));
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::EquipmentChanged(application::PlayerMenuSnapshot snapshot) {
  const auto room = registry_.FindByPlayer(snapshot.player_id);
  if (!room) {
    return RoomCoordinatorResult{};
  }
  auto result =
      entries_.at(room->RoomId()).flow->EquipmentChanged(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&result)) {
    return *error;
  }
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(result)));
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::InitialMatchEffects(
    domain::RoomId room_id, std::chrono::steady_clock::time_point now) {
  const auto found = entries_.find(room_id);
  if (found == entries_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "初始目标所属房间不存在");
  }
  auto value = found->second.flow->InitialMatchEffects(now);
  if (const auto* error = std::get_if<application::ServiceError>(&value)) {
    return *error;
  }
  return Decorate(room_id, std::get<RoomCommandResult>(std::move(value)));
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::Handle(
    RoomActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (!actor.player_id.IsValid() || !SnapshotMatches(actor)) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "房间命令玩家标识与资料快照不一致");
  }

  switch (packet.opcode) {
    case kTrainingEntryOpcode:
      return HandleTrainingEntry(std::move(actor), packet, now);
    case 141U:
      return HandleSurvivalQueue(std::move(actor), packet);
    case 401U:
      return HandleList(actor.player_id, packet);
    case 402U:
      return HandleCreate(std::move(actor), packet, isolated_capacity_);
    case 403U:
      return HandleJoin(std::move(actor), packet);
    case 404U:
      return HandleLeave(actor.player_id, packet, now);
    default:
      if ((packet.opcode == 2028U || packet.opcode == 2029U) &&
          actor.player_snapshot) {
        auto updated = UpdatePlayerSnapshot(std::move(*actor.player_snapshot));
        if (const auto* error =
                std::get_if<application::ServiceError>(&updated))
          return *error;
      }
      return HandleRoomCommand(actor.player_id, packet, now, actor.peer_address,
                               actor.ice_lease);
  }
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::HandleChat(
    domain::PlayerId actor, bool team_only, const domain::UserMessage& message,
    std::span<const domain::PlayerId> blocked_peers) {
  const auto room = registry_.FindByPlayer(actor);
  if (!room) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在活动房间中");
  }
  const auto entry = entries_.find(room->RoomId());
  if (entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间 registry 与命令流记录不一致");
  }
  auto handled =
      entry->second.flow->HandleChat(actor, team_only, message, blocked_peers);
  if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
    return *error;
  }
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(handled)));
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::HandleList(
    domain::PlayerId actor, const protocol::Packet& packet) const {
  const auto request = protocol::DecodeRoomListRequest(packet.body);
  const auto start = static_cast<std::size_t>(request.page) * request.page_size;
  // Private training rooms never appear in the public room list.
  std::vector<const protocol::SceneInfo*> visible;
  for (const auto& [id, entry] : entries_) {
    const auto room = registry_.FindById(id);
    if (room && !room->IsTraining()) visible.push_back(&entry.flow->Scene());
  }
  std::vector<protocol::SceneInfo> scenes;
  if (start < visible.size()) {
    const auto remaining = visible.size() - start;
    scenes.reserve(std::min<std::size_t>(request.page_size, remaining));
    for (std::size_t count = 0U; count < remaining && count < request.page_size;
         ++count)
      scenes.push_back(*visible[start + count]);
  }
  for (auto& scene : scenes) {
    if (scene.configuration.auxiliary_label != HideSeekRules::kBusinessKey)
      continue;
    const auto room = registry_.FindById(domain::RoomId{scene.scene_id});
    const bool observing = room && room->Phase() == RoomPhase::kPlaying;
    // Build752 disables double-click for kind4 rows. Present only this
    // business mode as joinable; handle_join still assigns an observer and
    // the authoritative scene/match remain Playing throughout loading.
    if (observing) scene.kind = 2U;
    auto prefix =
        protocol::Utf8ToUtf16(observing ? "[躲猫猫·观战] " : "[躲猫猫] ");
    auto name = protocol::Utf8ToUtf16(scene.name);
    if (name.size() > 64U - prefix.size()) {
      name.resize(64U - prefix.size());
      if (!name.empty() && name.back() >= 0xD800U && name.back() <= 0xDBFFU)
        name.pop_back();
    }
    scene.name = protocol::Utf16ToUtf8(prefix + name);
  }
  const bool has_next =
      start < visible.size() && scenes.size() < visible.size() - start;
  return SingleDelivery(actor, 101U,
                        protocol::EncodeRoomListResponse(request.page, has_next,
                                                         std::span{scenes}));
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::HandleTrainingEntry(
    RoomActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  // Compatibility entry: build 1302 C2S108 collides with the 752 plaza user
  // list, so the bridge sends 65033. The server builds the private one-seat
  // scene.
  protocol::PacketReader{packet.body}.RequireEnd();
  const auto player = actor.player_id;
  protocol::SceneInfo scene;
  scene.name = kTrainingName;
  scene.configuration.mode_id = kTrainingMode;
  scene.configuration.family_key = kTrainingMap;
  scene.configuration.map_key = kTrainingMap;
  scene.configuration.display_name = kTrainingName;
  for (auto& map : scene.configuration.rotating_maps)
    map = {.family_key = std::string{kTrainingMap},
           .map_key = std::string{kTrainingMap}};
  protocol::PacketWriter create;
  protocol::EncodeSceneInfo(create, scene);
  create.WriteUtf16(u"");
  auto created = HandleCreate(std::move(actor),
                              {.opcode = 402U, .body = create.Release()}, 1U);
  if (const auto* error = std::get_if<application::ServiceError>(&created))
    return *error;
  auto result = std::get<RoomCoordinatorResult>(std::move(created));
  for (auto& delivery : result.deliveries)
    if (delivery.recipient == player && delivery.message.opcode == 102U)
      delivery.message.opcode = kTrainingEntryOpcode;
  if (!result.room_id) return result;
  const auto id = *result.room_id;
  protocol::PacketWriter start;
  start.WriteU32(0U);
  start.WriteUtf16(protocol::Utf8ToUtf16(kTrainingMap));
  start.WriteUtf16(protocol::Utf8ToUtf16(kTrainingName));
  start.WriteUtf16(protocol::Utf8ToUtf16(kTrainingMap));
  start.WriteU32(0U);
  for (std::size_t i = 0; i < protocol::kMatchRuleFieldCount; ++i)
    start.WriteU32(0U);
  start.WriteU8(0U);
  auto loading = entries_.at(id).flow->Handle(
      player, {.opcode = 503U, .body = start.Release()}, now);
  if (const auto* error = std::get_if<application::ServiceError>(&loading)) {
    static_cast<void>(registry_.Leave(player));
    entries_.erase(id);
    return *error;
  }
  auto transition =
      Decorate(id, std::get<RoomCommandResult>(std::move(loading)));
  result.deliveries.insert(
      result.deliveries.end(),
      std::make_move_iterator(transition.deliveries.begin()),
      std::make_move_iterator(transition.deliveries.end()));
  result.event = transition.event;
  result.session_updates = std::move(transition.session_updates);
  result.active_timers = std::move(transition.active_timers);
  return result;
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::HandleCreate(
    RoomActorContext actor, const protocol::Packet& packet,
    std::uint16_t capacity) {
  auto request = protocol::DecodeCreateRoomRequest(packet.body);
  const bool hide_seek =
      request.requested_scene.configuration.auxiliary_label ==
      HideSeekRules::kBusinessKey;
  if (hide_seek &&
      (request.requested_scene.configuration.mode_id != 1U ||
       request.requested_scene.configuration.submode_id == 4U ||
       request.requested_scene.configuration.field_12 != 0U ||
       !timing_.hide_seek_scene || !timing_.hide_seek_scene->Valid())) {
    auto result = SingleDelivery(actor.player_id, 102U,
                                 protocol::EncodeCreateRoomResponse(1U));
    AppendNotice(result, actor.player_id,
                 "躲猫猫客户端与地图尚未完成适配，模式未开放");
    return result;
  }
  if (hide_seek) capacity = 8U;
  if (!actor.player_snapshot.has_value()) {
    return MakeError(application::ErrorCode::kInternal,
                     "建房缺少一致性玩家资料快照");
  }
  if (!ValidRoomName(request.requested_scene.name) ||
      request.requested_scene.configuration.mode_id == 0U ||
      request.requested_scene.configuration.mode_id == kHordeTutorMode ||
      (request.requested_scene.configuration.mode_id == kTrainingMode &&
       capacity != 1U)) {
    return SingleDelivery(actor.player_id, 102U,
                          protocol::EncodeCreateRoomResponse(1U));
  }

  if (request.requested_scene.configuration.mode_id == kEventMoleMode)
    capacity = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        capacity, GetEventMoleProfile().maximum_players));
  const bool user_hosted = request.requested_scene.configuration.field_12 != 0U;
  if (user_hosted &&
      (!timing_.user_host_prize.enabled || capacity > 12U ||
       request.requested_scene.configuration.mode_id == 23U ||
       request.requested_scene.configuration.submode_id == 4U ||
       (request.requested_scene.configuration.field_8 & 0x80U) != 0U))
    return SingleDelivery(actor.player_id, 102U,
                          protocol::EncodeCreateRoomResponse(1U));
  RoomCreateSpec spec{.capacity = capacity,
                      .mode_id = request.requested_scene.configuration.mode_id,
                      .password = request.password,
                      .user_hosted = user_hosted,
                      .hide_seek_v1 = hide_seek};
  auto created = registry_.Create(actor.player_id, std::move(spec));
  if (const auto* error = std::get_if<application::ServiceError>(&created)) {
    static_cast<void>(error);
    return SingleDelivery(actor.player_id, 102U,
                          protocol::EncodeCreateRoomResponse(1U));
  }
  auto room = std::get<std::shared_ptr<RoomRuntime>>(std::move(created));

  auto scene = std::move(request.requested_scene);
  auto& configuration = scene.configuration;
  // 原版建房窗口默认带入胡同；普通房间应使用原生 Random 预览及开局抽图分支。
  // 只初始化未选图/客户端默认值，后续 513 手动选图不经过这里。
  if (!hide_seek && configuration.mode_id < 100U &&
      configuration.mode_id != 23U && configuration.mode_id != 16U &&
      configuration.mode_id != 11U &&
      (configuration.family_key.empty() ||
       configuration.family_key == "rel_Village01_Byway")) {
    configuration.family_key = "Random";
    configuration.map_key.clear();
    configuration.display_name = "随机地图";
    configuration.auxiliary_label.clear();
    for (auto& map : configuration.rotating_maps) {
      map = {.family_key = "Random", .map_key = {}};
    }
  }
  if (hide_seek) {
    configuration.family_key = timing_.hide_seek_scene->map_key;
    configuration.map_key = timing_.hide_seek_scene->map_key;
    // Keep the shipped native waiting-room duration option valid. The mode's
    // independent authoritative clock adds its own thirty-second hiding phase.
    configuration.selected_duration_ms = 240'000U;
    configuration.field_24 = 240'000U;
  }
  scene.scene_id = static_cast<std::uint16_t>(room->RoomId().Value());
  scene.kind = 2U;
  scene.occupants = user_hosted ? 0U : 1U;
  scene.observers = user_hosted ? 1U : 0U;
  scene.capacity = room->Capacity();
  scene.locked = request.password.empty() ? 0U : 1U;
  scene.field_40 = actor.player_snapshot->nickname;
  scene.scene_type = 2U;
  scene.field_68 = 0U;
  scene.key = room->RoomId().Value();
  scene.field_80.clear();
  scene.field_104 = 0U;
  scene.field_108 = 0U;
  scene.field_112 = 0U;
  scene.field_116 = 0U;

  std::vector<application::PlayerMenuSnapshot> snapshots;
  snapshots.push_back(std::move(*actor.player_snapshot));
  auto flow_result = RoomCommandFlow::Create(room, std::move(scene),
                                             std::move(snapshots), timing_);
  if (const auto* error =
          std::get_if<application::ServiceError>(&flow_result)) {
    const auto rollback = registry_.Leave(actor.player_id);
    static_cast<void>(rollback);
    return *error;
  }
  auto flow =
      std::get<std::unique_ptr<RoomCommandFlow>>(std::move(flow_result));
  auto snapshot = flow->MakeSceneSessionMessage(actor.player_id);
  if (const auto* error = std::get_if<application::ServiceError>(&snapshot)) {
    const auto rollback = registry_.Leave(actor.player_id);
    static_cast<void>(rollback);
    return *error;
  }

  RoomCoordinatorResult result{
      .event = RoomEvent::kMemberJoined,
      .room_id = room->RoomId(),
      .deliveries = {},
      .session_updates = {{.player_id = actor.player_id,
                           .stage = RoomSessionStage::kWaiting}},
      .active_timers = {}};
  result.deliveries.push_back(
      {.recipient = actor.player_id,
       .message = {.opcode = 102U,
                   .body = protocol::EncodeCreateRoomResponse(0U)}});
  result.deliveries.push_back(
      {.recipient = actor.player_id,
       .message = std::get<OutboundMessage>(std::move(snapshot))});
  if (user_hosted)
    result.deliveries.push_back(
        {actor.player_id,
         {999U, protocol::EncodeUserHostDefaultSetting(
                    timing_.user_host_prize.minimum_amount,
                    timing_.user_host_prize.maximum_amount)}});
  entries_.emplace(room->RoomId(), RoomEntry{.flow = std::move(flow)});
  return result;
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::HandleSurvivalQueue(RoomActorContext actor,
                                     const protocol::Packet& packet) {
  const auto request = protocol::DecodeSurvivalQueueRequest(packet.body);
  if (request.cancel) {
    return SingleDelivery(actor.player_id, 1011U,
                          protocol::EncodeSurvivalQueueResponse(4U));
  }
  if (request.mode != 23U || request.map_key != u"rel_Sudden07_Apartment") {
    return SingleDelivery(actor.player_id, 1011U,
                          protocol::EncodeSurvivalQueueResponse(1U, 1U));
  }
  protocol::SceneInfo scene;
  scene.name = "Local Survival";
  scene.configuration.mode_id = 23U;
  scene.configuration.score_target = 20U;
  scene.configuration.selected_duration_ms = 240'000U;
  scene.configuration.field_24 = 240'000U;
  scene.configuration.field_28 = 240'000U;
  scene.configuration.field_32 = 240'000U;
  scene.configuration.family_key = "rel_Sudden07_Apartment";
  scene.configuration.map_key = "rel_Sudden07_Apartment";
  scene.configuration.display_name = "示范公寓";
  for (auto& map : scene.configuration.rotating_maps) {
    map.family_key = "Random";
  }
  protocol::PacketWriter writer;
  protocol::EncodeSceneInfo(writer, scene);
  writer.WriteUtf16(u"");
  const auto player_id = actor.player_id;
  auto created = HandleCreate(std::move(actor),
                              {.opcode = 402U, .body = writer.Release()}, 16U);
  if (const auto* error = std::get_if<application::ServiceError>(&created)) {
    return *error;
  }
  auto result = std::get<RoomCoordinatorResult>(std::move(created));
  for (auto& delivery : result.deliveries) {
    if (delivery.recipient == player_id && delivery.message.opcode == 102U) {
      delivery.message = {.opcode = 1011U,
                          .body = protocol::EncodeSurvivalQueueResponse(
                              result.room_id.has_value() ? 0U : 1U,
                              result.room_id.has_value() ? 23U : 1U)};
    }
  }
  return result;
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::HandleJoin(
    RoomActorContext actor, const protocol::Packet& packet) {
  auto request = protocol::DecodeJoinRoomRequest(packet.body);
  if (!actor.player_snapshot.has_value()) {
    return MakeError(application::ErrorCode::kInternal,
                     "加入房间缺少一致性玩家资料快照");
  }

  const domain::RoomId room_id{request.scene_id};
  auto entry = entries_.find(room_id);
  if (entry == entries_.end()) {
    auto result = SingleDelivery(actor.player_id, 103U,
                                 protocol::EncodeJoinRoomResponse(1U));
    AppendNotice(result, actor.player_id, "房间已不存在");
    return result;
  }
  const auto destination = registry_.FindById(room_id);
  const bool hide_seek_observer = destination && destination->IsHideSeek() &&
                                  destination->Phase() == RoomPhase::kPlaying;
  if (request.entry_option != 0U &&
      !(request.entry_option == 1U && hide_seek_observer)) {
    auto result = SingleDelivery(actor.player_id, 103U,
                                 protocol::EncodeJoinRoomResponse(1U));
    AppendNotice(result, actor.player_id, "暂不支持该观战入口");
    return result;
  }
  auto joined = registry_.Join(actor.player_id, room_id, request.password);
  if (const auto* error = std::get_if<application::ServiceError>(&joined)) {
    auto result = SingleDelivery(actor.player_id, 103U,
                                 protocol::EncodeJoinRoomResponse(1U));
    AppendNotice(result, actor.player_id, error->safe_message);
    return result;
  }

  auto added =
      entry->second.flow->AddMemberSnapshot(std::move(*actor.player_snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&added)) {
    const auto rollback = registry_.Leave(actor.player_id);
    static_cast<void>(rollback);
    return *error;
  }
  auto result =
      Decorate(room_id, std::get<RoomCommandResult>(std::move(added)));
  result.deliveries.insert(
      result.deliveries.begin(),
      RoomDelivery{.recipient = actor.player_id,
                   .message = {.opcode = 103U,
                               .body = protocol::EncodeJoinRoomResponse(0U)}});
  result.session_updates.push_back(
      {.player_id = actor.player_id, .stage = RoomSessionStage::kWaiting});
  return result;
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::HandleLeave(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  static_cast<void>(protocol::DecodeLeaveRoomRequest(packet.body));
  const auto existing = registry_.FindByPlayer(actor);
  if (!existing) {
    auto result =
        SingleDelivery(actor, 119U, protocol::EncodeLeaveRoomResponse());
    result.session_updates.push_back(
        {.player_id = actor, .stage = RoomSessionStage::kOutsideRoom});
    return result;
  }
  const auto room_id = existing->RoomId();
  auto entry = entries_.find(room_id);
  if (entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间 registry 与协调器记录不一致");
  }

  std::optional<application::MatchSettlementCommand> retained_settlement;
  const bool removes_room = existing->Members().size() == 1U;
  if (removes_room && entry->second.flow->HasUnsettledMatch()) {
    auto prepared = entry->second.flow->PrepareAbandonedSettlement(now);
    if (const auto* error = std::get_if<application::ServiceError>(&prepared)) {
      return *error;
    }
    retained_settlement.emplace(
        std::get<application::MatchSettlementCommand>(std::move(prepared)));
  } else if (removes_room) {
    auto pending = entry->second.flow->PendingSettlement();
    if (const auto* command =
            std::get_if<application::MatchSettlementCommand>(&pending)) {
      retained_settlement = *command;
    } else if (const auto* error =
                   std::get_if<application::ServiceError>(&pending);
               error != nullptr &&
               error->code != application::ErrorCode::kInvalidState) {
      return *error;
    }
  }

  bool retained_tombstone = false;
  if (retained_settlement.has_value()) {
    const auto [unused, inserted] =
        closed_pending_settlements_.emplace(room_id, *retained_settlement);
    static_cast<void>(unused);
    if (!inserted) {
      return MakeError(application::ErrorCode::kInternal,
                       "关闭房间已有未确认终局 tombstone");
    }
    retained_tombstone = true;
  }

  const auto departure = registry_.Leave(actor);
  if (!departure.has_value()) {
    if (retained_tombstone) {
      closed_pending_settlements_.erase(room_id);
    }
    return MakeError(application::ErrorCode::kInternal,
                     "已索引房间成员离开时未产生结果");
  }
  if (departure->room_removed) {
    entries_.erase(entry);
    auto result =
        SingleDelivery(actor, 119U, protocol::EncodeLeaveRoomResponse());
    result.event = RoomEvent::kRoomClosed;
    result.room_id = room_id;
    result.settlement_command = std::move(retained_settlement);
    result.session_updates.push_back(
        {.player_id = actor, .stage = RoomSessionStage::kOutsideRoom});
    return result;
  }
  if (retained_tombstone) {
    closed_pending_settlements_.erase(room_id);
    return MakeError(application::ErrorCode::kInternal,
                     "预判最后成员离开但 registry 未删除房间");
  }

  auto removed = entry->second.flow->RemoveMemberSnapshot(
      actor, *departure, "对局中断，请重新开始", now);
  if (const auto* error = std::get_if<application::ServiceError>(&removed)) {
    return *error;
  }
  auto result =
      Decorate(room_id, std::get<RoomCommandResult>(std::move(removed)));
  result.deliveries.insert(
      result.deliveries.begin(),
      RoomDelivery{.recipient = actor,
                   .message = {.opcode = 119U,
                               .body = protocol::EncodeLeaveRoomResponse()}});
  result.session_updates.push_back(
      {.player_id = actor, .stage = RoomSessionStage::kOutsideRoom});
  return result;
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::HandleRoomCommand(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now, std::string_view peer_address,
    const std::optional<application::IceLease>& ice_lease) {
  const auto room = registry_.FindByPlayer(actor);
  if (!room) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在活动房间中");
  }
  const auto entry = entries_.find(room->RoomId());
  if (entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间 registry 与命令流记录不一致");
  }
  auto handled =
      entry->second.flow->Handle(actor, packet, now, peer_address, ice_lease);
  if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
    const bool host_request_rejected =
        packet.opcode == 508U &&
        (error->code == application::ErrorCode::kPermissionDenied ||
         error->code == application::ErrorCode::kInvalidInput);
    // 换队满员/合作模式限制属于正常业务拒绝；向本人提示，不能交给 TCP
    // 致命错误路径。
    const bool team_request_rejected =
        packet.opcode == 515U &&
        (error->code == application::ErrorCode::kConflict ||
         error->code == application::ErrorCode::kInvalidState);
    const bool prize_request_rejected =
        packet.opcode == 2029U &&
        (error->code == application::ErrorCode::kPermissionDenied ||
         error->code == application::ErrorCode::kInvalidInput ||
         error->code == application::ErrorCode::kInvalidState);
    if (host_request_rejected || team_request_rejected ||
        prize_request_rejected) {
      RoomCoordinatorResult result;
      result.room_id = room->RoomId();
      AppendNotice(result, actor, error->safe_message);
      return result;
    }
    return *error;
  }
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(handled)));
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::ApplyTimer(
    const RoomTimerToken& token, std::chrono::steady_clock::time_point now) {
  const auto room = registry_.FindById(token.room_id);
  const auto entry = entries_.find(token.room_id);
  if (!room && entry == entries_.end()) {
    RoomCoordinatorResult result;
    result.event = RoomEvent::kIgnoredStaleTimer;
    result.room_id = token.room_id;
    return result;
  }
  if (!room || entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间 registry 与命令流记录不一致");
  }
  auto applied = entry->second.flow->ApplyTimer(token, now);
  if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
    return *error;
  }
  return Decorate(token.room_id,
                  std::get<RoomCommandResult>(std::move(applied)));
}

application::ServiceResult<RoomCoordinatorResult> RoomCoordinator::FinishMatch(
    domain::RoomId room_id, application::MatchCompletionKind completion,
    std::chrono::steady_clock::time_point now) {
  const auto room = registry_.FindById(room_id);
  const auto entry = entries_.find(room_id);
  if (!room || entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "权威终局所属房间已不存在");
  }
  auto finished = entry->second.flow->FinishMatch(completion, now);
  if (const auto* error = std::get_if<application::ServiceError>(&finished)) {
    return *error;
  }
  return Decorate(room_id, std::get<RoomCommandResult>(std::move(finished)));
}

application::ServiceResult<bool> RoomCoordinator::UpdatePlayerSnapshot(
    application::PlayerMenuSnapshot snapshot) {
  const auto room = registry_.FindByPlayer(snapshot.player_id);
  if (!room) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "资料快照玩家不在活动房间中");
  }
  const auto entry = entries_.find(room->RoomId());
  if (entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间 registry 与命令流记录不一致");
  }
  return entry->second.flow->UpdatePlayerSnapshot(std::move(snapshot));
}

application::ServiceResult<bool> RoomCoordinator::InstallCombat(
    domain::RoomId room_id, const RoomMatchInstallation& installation) {
  const auto room = registry_.FindById(room_id);
  const auto entry = entries_.find(room_id);
  if (!room || entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "战斗参与者所属房间不存在");
  }
  return entry->second.flow->InstallCombat(installation);
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::AbortFailedMatchStart(domain::RoomId room_id,
                                       std::string_view notice) {
  const auto room = registry_.FindById(room_id);
  const auto entry = entries_.find(room_id);
  if (!room || entry == entries_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "撤销开局的房间不存在");
  }
  auto aborted = entry->second.flow->AbortFailedMatchStart(notice);
  if (const auto* error = std::get_if<application::ServiceError>(&aborted)) {
    return *error;
  }
  return Decorate(room_id, std::get<RoomCommandResult>(std::move(aborted)));
}

application::ServiceResult<application::MatchSettlementCommand>
RoomCoordinator::PendingSettlement(domain::RoomId room_id) const {
  const auto entry = entries_.find(room_id);
  if (entry != entries_.end()) {
    return entry->second.flow->PendingSettlement();
  }
  const auto closed = closed_pending_settlements_.find(room_id);
  if (closed != closed_pending_settlements_.end()) {
    return closed->second;
  }
  return MakeError(application::ErrorCode::kInvalidState,
                   "房间没有等待耐久保存的终局命令");
}

application::ServiceResult<bool> RoomCoordinator::AcknowledgeSettlementStaged(
    domain::RoomId room_id, const domain::MatchId& match_id) {
  const auto entry = entries_.find(room_id);
  if (entry != entries_.end()) {
    return entry->second.flow->AcknowledgeSettlementStaged(match_id);
  }
  const auto closed = closed_pending_settlements_.find(room_id);
  if (closed == closed_pending_settlements_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间没有可确认的终局 tombstone");
  }
  if (closed->second.match_id != match_id) {
    return MakeError(application::ErrorCode::kConflict,
                     "终局日志回执与关闭房间 tombstone 的 match_id 不一致");
  }
  closed_pending_settlements_.erase(closed);
  return true;
}

std::size_t RoomCoordinator::RoomCount() const noexcept {
  return entries_.size();
}

std::optional<protocol::SceneInfo> RoomCoordinator::Scene(
    domain::RoomId room_id) const {
  const auto entry = entries_.find(room_id);
  if (entry == entries_.end()) {
    return std::nullopt;
  }
  return entry->second.flow->Scene();
}

std::vector<protocol::SceneInfo> RoomCoordinator::WaitingScenes() const {
  std::vector<protocol::SceneInfo> result;
  for (const auto& [id, entry] : entries_) {
    const auto room = registry_.FindById(id);
    if (room && room->Phase() == RoomPhase::kWaiting)
      result.push_back(entry.flow->Scene());
  }
  return result;
}

std::shared_ptr<RoomRuntime> RoomCoordinator::FindRoom(
    domain::PlayerId player_id) const {
  return registry_.FindByPlayer(player_id);
}

std::shared_ptr<RoomRuntime> RoomCoordinator::FindRoom(
    domain::RoomId room_id) const {
  return registry_.FindById(room_id);
}

RoomCoordinatorResult RoomCoordinator::Decorate(
    domain::RoomId room_id, RoomCommandResult result) const {
  RoomCoordinatorResult decorated{
      .event = result.event,
      .room_id = room_id,
      .deliveries = std::move(result.deliveries),
      .session_updates = {},
      .active_timers = std::move(result.active_timers),
      .combat_reject_reason = result.combat_reject_reason,
      .rule_rejection_detail = std::move(result.rule_rejection_detail),
      .settlement_command = std::move(result.settlement_command)};
  AppendPhaseUpdates(decorated, registry_.FindById(room_id));
  if (const auto room = registry_.FindById(room_id); room && room->IsHideSeek())
    for (const auto& d : decorated.deliveries) {
      const auto member = room->GetMember(d.recipient);
      if (member && member->IsObserver() &&
          (d.message.opcode == 510U || d.message.opcode == 704U))
        decorated.session_updates.push_back(
            {.player_id = d.recipient,
             .stage = d.message.opcode == 510U ? RoomSessionStage::kLoading
                                               : RoomSessionStage::kPlaying});
    }
  return decorated;
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::ApplySettlementResult(
    const application::MatchSettlementResult& settlement,
    std::chrono::steady_clock::time_point now) {
  for (auto& [room_id, entry] : entries_) {
    auto applied = entry.flow->ApplySettlementResult(settlement, now);
    if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
      return *error;
    }
    auto result = std::get<RoomCommandResult>(std::move(applied));
    if (!result.deliveries.empty()) {
      return Decorate(room_id, std::move(result));
    }
  }
  return RoomCoordinatorResult{};
}

std::optional<application::PlayerMenuSnapshot> RoomCoordinator::PlayerSnapshot(
    domain::PlayerId player) const {
  const auto room = registry_.FindByPlayer(player);
  if (!room) return std::nullopt;
  const auto entry = entries_.find(room->RoomId());
  return entry == entries_.end() ? std::nullopt
                                 : entry->second.flow->PlayerSnapshot(player);
}

void RoomCoordinator::AppendPhaseUpdates(
    RoomCoordinatorResult& result,
    const std::shared_ptr<RoomRuntime>& room) const {
  if (!room) {
    return;
  }
  std::optional<RoomSessionStage> stage;
  switch (result.event) {
    case RoomEvent::kLoadingStarted:
    case RoomEvent::kRoundLoadingStarted:
      stage = RoomSessionStage::kLoading;
      break;
    case RoomEvent::kCountdownStarted:
      stage = RoomSessionStage::kCountdown;
      break;
    case RoomEvent::kMatchStarted:
    case RoomEvent::kRoundStarted:
      stage = RoomSessionStage::kPlaying;
      break;
    case RoomEvent::kResultsStarted:
      stage = RoomSessionStage::kResults;
      break;
    case RoomEvent::kReturnedToWaiting:
      stage = RoomSessionStage::kWaiting;
      break;
    default:
      break;
  }
  if (!stage.has_value()) {
    return;
  }
  for (const auto& member : room->Members()) {
    result.session_updates.push_back(
        {.player_id = member.player_id, .stage = *stage});
  }
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::LevelIconChanged(application::PlayerMenuSnapshot snapshot) {
  const auto room = registry_.FindByPlayer(snapshot.player_id);
  if (!room) return RoomCoordinatorResult{};
  auto result =
      entries_.at(room->RoomId()).flow->LevelIconChanged(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return *error;
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(result)));
}

application::ServiceResult<RoomCoordinatorResult>
RoomCoordinator::CoupleChanged(application::PlayerMenuSnapshot snapshot) {
  const auto room = registry_.FindByPlayer(snapshot.player_id);
  if (!room) return RoomCoordinatorResult{};
  auto result =
      entries_.at(room->RoomId()).flow->CoupleChanged(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return *error;
  return Decorate(room->RoomId(),
                  std::get<RoomCommandResult>(std::move(result)));
}

}  // namespace bf::runtime
