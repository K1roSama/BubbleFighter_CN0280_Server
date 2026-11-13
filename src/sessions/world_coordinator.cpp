#include "sessions/world_coordinator.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/characters/native_title_profile.h"
#include "features/chat/megaphone_event.h"
#include "features/chat/user_message.h"
#include "protocol/battle/combat_messages.h"
#include "protocol/battle/extreme_racing_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/chat/chat_messages.h"
#include "protocol/codec/text_codec.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::runtime {

std::uint32_t WorldCoordinator::RoomChannel(domain::RoomId id) const {
  const auto room = rooms_.FindRoom(id);
  if (!room || room->Members().empty()) return 0U;
  std::uint32_t channel = 0U;
  for (const auto& member : room->Members()) {
    const auto found = online_players_.find(member.player_id);
    if (found == online_players_.end() ||
        (channel != 0U && channel != found->second.state.channel))
      return 0U;
    channel = found->second.state.channel;
    if (channel == 0U) return 0U;
  }
  return channel;
}

namespace {

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

/** @brief 有效处罚记录的发送者：509 改由桥接 65031 携带元数据，客户端 Clean
 * 聊天据此过滤。 */
void AttachCleanChatHistory(WorldCoordinatorResult& result,
                            const WorldActorContext& actor) {
  if (!actor.has_chat_penalty_history) {
    return;
  }
  for (auto& delivery : result.deliveries) {
    if (delivery.message.opcode != 509U) {
      continue;
    }
    delivery.message.body = protocol::EncodeCleanChatEnvelope(
        actor.player_id.Value(), true, false, delivery.message.body);
    delivery.message.opcode = 65031U;
  }
}

[[nodiscard]] WorldCoordinatorResult TutorialResult(
    domain::PlayerId player, LocalTutorialUpdate update) {
  WorldCoordinatorResult result;
  for (auto& delivery : update.deliveries)
    result.deliveries.push_back(
        {delivery.recipient, std::move(delivery.message)});
  result.tutorial_command = std::move(update.persistence);
  if (update.release_owner)
    result.local_release_owner = std::pair{player, *update.release_owner};
  if (update.phase) {
    auto phase = WorldSessionPhase::kMenu;
    if (*update.phase == LocalTutorialPhase::kLoading)
      phase = WorldSessionPhase::kTutorialLoading;
    if (*update.phase == LocalTutorialPhase::kPlaying)
      phase = WorldSessionPhase::kTutorialPlaying;
    if (*update.phase == LocalTutorialPhase::kSingleWaiting)
      phase = WorldSessionPhase::kSinglePlayWaiting;
    result.session_updates.push_back({player, phase, std::nullopt});
  }
  return result;
}

[[nodiscard]] bool IsRoomPhase(WorldSessionPhase phase) noexcept {
  return phase == WorldSessionPhase::kRoomWaiting ||
         phase == WorldSessionPhase::kRoomLoading ||
         phase == WorldSessionPhase::kRoomCountdown ||
         phase == WorldSessionPhase::kRoomPlaying ||
         phase == WorldSessionPhase::kRoomResults;
}

[[nodiscard]] WorldSessionPhase WorldPhase(RoomSessionStage stage) {
  switch (stage) {
    case RoomSessionStage::kWaiting:
      return WorldSessionPhase::kRoomWaiting;
    case RoomSessionStage::kLoading:
      return WorldSessionPhase::kRoomLoading;
    case RoomSessionStage::kCountdown:
      return WorldSessionPhase::kRoomCountdown;
    case RoomSessionStage::kPlaying:
      return WorldSessionPhase::kRoomPlaying;
    case RoomSessionStage::kResults:
      return WorldSessionPhase::kRoomResults;
    case RoomSessionStage::kOutsideRoom:
      return WorldSessionPhase::kMenu;
  }
  throw std::logic_error{"未知房间 Session 阶段"};
}

void Append(WorldCoordinatorResult& destination, PlazaCoordinatorResult source,
            std::optional<domain::PlayerId> ignored_update = std::nullopt) {
  for (auto& delivery : source.deliveries) {
    destination.deliveries.push_back({.recipient = delivery.recipient,
                                      .message = std::move(delivery.message)});
  }
  for (const auto& update : source.session_updates) {
    if (!ignored_update.has_value() || update.player_id != *ignored_update) {
      destination.session_updates.push_back(
          {.player_id = update.player_id,
           .phase = update.stage == PlazaSessionStage::kPlaza
                        ? WorldSessionPhase::kPlaza
                        : WorldSessionPhase::kMenu,
           .channel = std::nullopt});
    }
  }
}

void Append(WorldCoordinatorResult& destination, RoomCoordinatorResult source,
            std::optional<domain::PlayerId> ignored_update = std::nullopt) {
  destination.event = source.event;
  destination.room_id = source.room_id;
  for (auto& delivery : source.deliveries) {
    destination.deliveries.push_back({.recipient = delivery.recipient,
                                      .message = std::move(delivery.message)});
  }
  for (const auto& update : source.session_updates) {
    if (!ignored_update.has_value() || update.player_id != *ignored_update) {
      destination.session_updates.push_back({.player_id = update.player_id,
                                             .phase = WorldPhase(update.stage),
                                             .channel = std::nullopt});
    }
  }
  destination.active_timers = std::move(source.active_timers);
  destination.combat_reject_reason = source.combat_reject_reason;
  destination.rule_rejection_detail = std::move(source.rule_rejection_detail);
  destination.settlement_command = std::move(source.settlement_command);
}

void Prepend(WorldCoordinatorResult& result, domain::PlayerId recipient,
             std::uint16_t opcode, std::vector<std::uint8_t> body) {
  result.deliveries.insert(
      result.deliveries.begin(),
      WorldDelivery{.recipient = recipient,
                    .message = {.opcode = opcode, .body = std::move(body)}});
}

void InsertBeforeActorMessage(WorldCoordinatorResult& result,
                              domain::PlayerId recipient,
                              std::uint16_t before_opcode,
                              OutboundMessage message) {
  const auto position =
      std::ranges::find_if(result.deliveries, [&](const auto& delivery) {
        return delivery.recipient == recipient &&
               delivery.message.opcode == before_opcode;
      });
  result.deliveries.insert(
      position,
      WorldDelivery{.recipient = recipient, .message = std::move(message)});
}

void AppendNotice(WorldCoordinatorResult& result, domain::PlayerId recipient,
                  std::string_view message) {
  result.deliveries.push_back(
      {.recipient = recipient,
       .message = {.opcode = 109U,
                   .body = protocol::EncodeNoticeResponse(
                       protocol::Utf8ToUtf16(message))}});
}

[[nodiscard]] RoomActorContext RoomActor(WorldActorContext actor) {
  return {.player_id = actor.player_id,
          .player_snapshot = std::move(actor.player_snapshot),
          .peer_address = std::move(actor.peer_address),
          .ice_lease = std::move(actor.ice_lease)};
}

[[nodiscard]] bool HasSnapshot(const WorldActorContext& actor) noexcept {
  return actor.player_snapshot.has_value() &&
         actor.player_snapshot->player_id == actor.player_id;
}

}  // namespace

WorldCoordinatorResult WorldCoordinator::ClanMarkChanged(
    domain::PlayerId player, std::uint32_t mark) {
  WorldCoordinatorResult result;
  if (plaza_.Contains(player)) {
    Append(result, plaza_.ClanMarkChanged(player, mark));
  } else {
    Append(result, rooms_.ClanMarkChanged(player, mark));
  }
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::EquipmentChanged(application::PlayerMenuSnapshot snapshot,
                                   bool refresh_menu_slot) {
  WorldCoordinatorResult result;
  if (const auto found = online_players_.find(snapshot.player_id);
      found != online_players_.end()) {
    found->second.snapshot = snapshot;
    if (PartyMembers(snapshot.player_id).size() > 1U)
      PublishParty(result, snapshot.player_id);
  }
  if (plaza_.Contains(snapshot.player_id)) {
    auto applied = plaza_.EquipmentChanged(std::move(snapshot));
    if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
      return *error;
    }
    Append(result, std::get<PlazaCoordinatorResult>(std::move(applied)));
  } else if (rooms_.FindRoom(snapshot.player_id)) {
    auto applied = rooms_.EquipmentChanged(std::move(snapshot));
    if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
      return *error;
    }
    Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  } else if (refresh_menu_slot) {
    // 主菜单也持有席位 0；806 更新模型，503 更新后续场景使用的 UserSlot。
    result.deliveries.push_back(
        {.recipient = snapshot.player_id,
         .message = {.opcode = 503U,
                     .body = protocol::EncodeSceneMemberUpdateResponse(
                         {.slot = 0U,
                          .team = 0U,
                          .occupied = true,
                          .locked = false,
                          .ready = false,
                          .player = std::move(snapshot)})}});
  }
  return result;
}

WorldCoordinator::WorldCoordinator(RoomMatchTiming timing)
    : plaza_(), rooms_(timing) {}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::EnterDedicatedRoom(const WorldActorContext& actor) {
  if (actor.state.phase != WorldSessionPhase::kPlaza ||
      !plaza_.Contains(actor.player_id) ||
      dedicated_members_.contains(actor.player_id))
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能从自由广场进入独立房间");
  if (PartyMembers(actor.player_id).size() > 1U)
    return MakeError(application::ErrorCode::kInvalidState,
                     "独立房间的组队迁移尚未开放，请先解散队伍");
  auto left = plaza_.Leave(actor.player_id);
  if (const auto* error = std::get_if<application::ServiceError>(&left))
    return *error;
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(left)),
         actor.player_id);
  dedicated_members_.insert(actor.player_id);
  if (auto found = online_players_.find(actor.player_id);
      found != online_players_.end())
    found->second.state.phase = WorldSessionPhase::kRoomWaiting;
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::LeaveDedicatedRoom(WorldActorContext actor) {
  if (!dedicated_members_.contains(actor.player_id) || !actor.player_snapshot ||
      actor.player_snapshot->player_id != actor.player_id)
    return MakeError(application::ErrorCode::kInvalidState,
                     "独立房间返回缺少当前成员或资料");
  auto entered = plaza_.Enter(std::move(*actor.player_snapshot), 0U,
                              PartySceneState(actor.player_id, false));
  if (const auto* error = std::get_if<application::ServiceError>(&entered))
    return *error;
  dedicated_members_.erase(actor.player_id);
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(entered)));
  if (actor.state.channel != 60U)
    InsertBeforeActorMessage(
        result, actor.player_id, 112U,
        {13U, protocol::EncodeChannelSelectionResponse(true, 60U, 1U)});
  for (auto& update : result.session_updates)
    if (update.player_id == actor.player_id) update.channel = 60U;
  if (auto found = online_players_.find(actor.player_id);
      found != online_players_.end())
    found->second.state = {WorldSessionPhase::kPlaza, 60U};
  return result;
}

bool WorldCoordinator::HasTutorial(domain::PlayerId player) const {
  return tutorials_.Contains(player) || single_play_.Contains(player);
}

bool WorldCoordinator::HasSingleWaiting(domain::PlayerId player) const {
  return single_play_.Waiting(player);
}

std::optional<domain::MatchId> WorldCoordinator::LocalSessionId(
    domain::PlayerId player) const {
  return single_play_.Contains(player) ? single_play_.SessionId(player)
                                       : tutorials_.SessionId(player);
}

std::optional<std::chrono::steady_clock::time_point>
WorldCoordinator::TutorialDeadline() const {
  const auto tutor = tutorials_.Deadline();
  const auto single = single_play_.Deadline();
  return tutor && single ? std::optional{std::min(*tutor, *single)}
         : tutor         ? tutor
                         : single;
}

std::vector<WorldCoordinatorResult> WorldCoordinator::TickTutorials(
    std::chrono::steady_clock::time_point now) {
  std::vector<WorldCoordinatorResult> results;
  for (auto& [player, update] : tutorials_.Tick(now))
    results.push_back(TutorialResult(player, std::move(update)));
  for (auto& [player, update] : single_play_.Tick(now))
    results.push_back(TutorialResult(player, std::move(update)));
  return results;
}

void WorldCoordinator::TutorialAcknowledge(domain::PlayerId player,
                                           const domain::MatchId& session,
                                           bool begin, bool publish_phase) {
  const bool single = single_play_.SessionId(player) == session;
  const bool acknowledged =
      single ? single_play_.Acknowledge(player, session, begin)
             : tutorials_.Acknowledge(player, session, begin);
  if (!acknowledged || !publish_phase) return;
  if (auto found = online_players_.find(player); found != online_players_.end())
    found->second.state.phase = begin ? WorldSessionPhase::kTutorialLoading
                                : single_play_.Waiting(player)
                                    ? WorldSessionPhase::kSinglePlayWaiting
                                    : WorldSessionPhase::kMenu;
}

void WorldCoordinator::TutorialBeginFailed(domain::PlayerId player,
                                           const domain::MatchId& session,
                                           bool publish_phase) {
  if (!(single_play_.BeginFailed(player, session) ||
        tutorials_.BeginFailed(player, session)) ||
      !publish_phase)
    return;
  if (auto found = online_players_.find(player); found != online_players_.end())
    found->second.state.phase = single_play_.Waiting(player)
                                    ? WorldSessionPhase::kSinglePlayWaiting
                                    : WorldSessionPhase::kMenu;
}

WorldCoordinatorResult WorldCoordinator::InterruptTutorial(
    domain::PlayerId player, const domain::MatchId& session,
    std::string reason) {
  const bool single = single_play_.SessionId(player) == session;
  if (!single && tutorials_.SessionId(player) != session) return {};
  auto result = TutorialResult(
      player, single ? single_play_.Disconnect(player)
                     : tutorials_.Disconnect(player, std::move(reason)));
  result.deliveries.clear();
  result.session_updates.clear();
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::InitialMatchEffects(
    domain::RoomId room_id, std::chrono::steady_clock::time_point now) {
  auto applied = rooms_.InitialMatchEffects(room_id, now);
  if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::ApplySettlementResult(
    const application::MatchSettlementResult& settlement) {
  auto applied = rooms_.ApplySettlementResult(settlement,
                                              std::chrono::steady_clock::now());
  if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::Handle(
    WorldActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (packet.opcode == hide_seek_wire::native_create_opcode) {
    // Keep the original dialog's complete configuration, name and password.
    // Only the business identity changes; the original mode factory remains 1.
    auto request = protocol::DecodeCreateRoomRequest(packet.body);
    if (request.requested_scene.configuration.mode_id != 1U)
      return MakeError(application::ErrorCode::kInvalidInput,
                       "躲猫猫必须使用普通团队场景加载");
    request.requested_scene.configuration.auxiliary_label =
        HideSeekRules::kBusinessKey;
    protocol::PacketWriter writer;
    protocol::EncodeSceneInfo(writer, request.requested_scene);
    writer.WriteUtf16(protocol::Utf8ToUtf16(request.password));
    return Handle(std::move(actor), {402U, writer.Release()}, now);
  }
  if (packet.opcode == hide_seek_wire::create_opcode) {
    try {
      hide_seek_wire::Reader r(packet.body.data(), packet.body.size());
      r.header();
      const auto capabilities = r.word();
      r.end();
      if ((capabilities & hide_seek_wire::required_capabilities) !=
          hide_seek_wire::required_capabilities)
        return MakeError(application::ErrorCode::kInvalidState,
                         "客户端未提供完整躲猫猫能力");
      protocol::SceneInfo scene;
      scene.name = "躲猫猫";
      scene.capacity = 8U;
      scene.kind = 2U;
      scene.scene_type = 2U;
      scene.configuration.mode_id = 1U;
      scene.configuration.auxiliary_label = HideSeekRules::kBusinessKey;
      protocol::PacketWriter writer;
      protocol::EncodeSceneInfo(writer, scene);
      writer.WriteUtf16(u"");
      return Handle(std::move(actor), {402U, writer.Release()}, now);
    } catch (const std::invalid_argument&) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "躲猫猫创建请求版本或负载无效");
    }
  }
  if (actor.player_snapshot &&
      actor.player_snapshot->player_id == actor.player_id) {
    PlayerOnline(*actor.player_snapshot);
  }
  if (const auto found = online_players_.find(actor.player_id);
      found != online_players_.end()) {
    found->second.state = actor.state;
  }
  const auto sender = actor.player_id;
  auto handled = HandleImpl(std::move(actor), packet, now);
  if (auto* result = std::get_if<WorldCoordinatorResult>(&handled)) {
    for (const auto& update : result->session_updates) {
      if (result->tutorial_command)
        continue;  // Candidate scene/terminal phases become permissions only
                   // after SQL acknowledgement.
      if (const auto found = online_players_.find(update.player_id);
          found != online_players_.end()) {
        found->second.state.phase = update.phase;
        if (update.channel) found->second.state.channel = *update.channel;
      }
    }
    if ((packet.opcode == 402U || packet.opcode == 403U) && result->room_id) {
      const auto members = PartyMembers(sender);
      const auto room = rooms_.FindRoom(sender);
      if (members.front() == sender && room &&
          room->RoomId() == *result->room_id &&
          room->Phase() == RoomPhase::kWaiting &&
          online_players_.contains(sender) &&
          online_players_.at(sender).state.channel != 80U) {
        for (const auto peer : members) {
          if (peer == sender || !online_players_.contains(peer) ||
              online_players_.at(peer).state.channel !=
                  online_players_.at(sender).state.channel)
            continue;
          auto visited = VisitRoom(peer, room->RoomId(), now);
          if (auto* follow = std::get_if<WorldCoordinatorResult>(&visited)) {
            result->deliveries.insert(
                result->deliveries.end(),
                std::make_move_iterator(follow->deliveries.begin()),
                std::make_move_iterator(follow->deliveries.end()));
            result->session_updates.insert(result->session_updates.end(),
                                           follow->session_updates.begin(),
                                           follow->session_updates.end());
            if (follow->room_id)
              result->active_timers = std::move(follow->active_timers);
          }
        }
      }
    }
    // Successful scene transitions revoke the old scene's public recruitment,
    // including members moved by party-follow. Failed transitions have no
    // session updates.
    for (const auto& update : result->session_updates) {
      recruiting_parties_.erase(update.player_id);
    }
    if (!result->session_updates.empty() && !result->tutorial_command)
      SyncPartyPlaza(*result, sender);
  }
  return handled;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::HandleImpl(
    WorldActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (!actor.player_id.IsValid() ||
      (actor.player_snapshot.has_value() &&
       actor.player_snapshot->player_id != actor.player_id)) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "世界命令玩家标识与资料快照不一致");
  }
  if (single_play_.Contains(actor.player_id) && packet.opcode != 107U) {
    auto handled = single_play_.Handle(actor.player_id, packet, now);
    if (const auto* error = std::get_if<application::ServiceError>(&handled))
      return *error;
    return TutorialResult(actor.player_id,
                          std::get<LocalTutorialUpdate>(std::move(handled)));
  }
  if (tutorials_.Contains(actor.player_id)) {
    auto handled = tutorials_.Handle(actor.player_id, packet, now);
    if (const auto* error = std::get_if<application::ServiceError>(&handled))
      return *error;
    return TutorialResult(actor.player_id,
                          std::get<LocalTutorialUpdate>(std::move(handled)));
  }
  if (packet.opcode == 107U) {
    const auto entry = protocol::DecodeTutorialStartReport(packet.body);
    const bool single = entry.mission_key != u"rel_prologue_01";
    if ((actor.state.phase != WorldSessionPhase::kMenu &&
         !(single &&
           actor.state.phase == WorldSessionPhase::kSinglePlayWaiting &&
           single_play_.Waiting(actor.player_id))) ||
        !actor.player_snapshot || !actor.tutorial_session_id ||
        rooms_.FindRoom(actor.player_id) || plaza_.Contains(actor.player_id) ||
        PartyMembers(actor.player_id).size() != 1U)
      return MakeError(application::ErrorCode::kInvalidState,
                       "原单人教程只能由主菜单中的独立认证玩家进入");
    auto began =
        single ? single_play_.Begin(std::move(*actor.player_snapshot), packet,
                                    *actor.tutorial_session_id, now)
               : tutorials_.Begin(std::move(*actor.player_snapshot), packet,
                                  *actor.tutorial_session_id, now);
    if (const auto* error = std::get_if<application::ServiceError>(&began))
      return *error;
    std::erase_if(
        party_invitations_, [player = actor.player_id](const auto& invitation) {
          return invitation.requester == player || invitation.target == player;
        });
    recruiting_parties_.erase(actor.player_id);
    return TutorialResult(actor.player_id,
                          std::get<LocalTutorialUpdate>(std::move(began)));
  }
  if (protocol::IsPveEventOpcode(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    protocol::ValidatePveEvent(packet.opcode, packet.body);
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (protocol::IsSpecialModeClaimOpcode(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    protocol::ValidateSpecialModeClaim(packet.opcode, packet.body);
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  const bool native_combat =
      protocol::IsNativeStormOpcode(packet.opcode) ||
      protocol::IsNativeWeaponObjectOpcode(packet.opcode) ||
      protocol::IsNativeActionOpcode(packet.opcode) ||
      protocol::IsNativeHitOpcode(packet.opcode) ||
      protocol::IsNativeLifecycleOpcode(packet.opcode) ||
      protocol::IsNativeRescueOpcode(packet.opcode) ||
      protocol::IsNativeRespawnOpcode(packet.opcode);
  if (PirateMatch::Accepts(packet.opcode) && !IsRoomPhase(actor.state.phase)) {
    PirateMatch::Validate(packet);
    return WorldCoordinatorResult{};
  }
  if (PirateMapAgents::Accepts(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    PirateMapAgents::Validate(packet);
    return WorldCoordinatorResult{};
  }
  if (PirateMapAgents::AcceptsLink(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    PirateMapAgents::ValidateLink(packet);
    return WorldCoordinatorResult{};
  }
  if (!IsRoomPhase(actor.state.phase) &&
      protocol::ValidateRoomControlMessage(packet.opcode, packet.body)) {
    return WorldCoordinatorResult{};
  }
  if (MatchObjectives::Accepts(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    MatchObjectives::Validate(packet);
    return WorldCoordinatorResult{};
  }
  if (CombatRelay::IsPeerEffectOpcode(packet.opcode) &&
      !IsRoomPhase(actor.state.phase)) {
    // A late peer effect after leaving the room is length-checked and dropped.
    const std::size_t size = packet.opcode == 32824U   ? 16U
                             : packet.opcode == 32832U ? 32U
                                                       : 8U;
    if (packet.body.size() != size)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "peer effect size is invalid"};
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (packet.opcode == 2021U && !IsRoomPhase(actor.state.phase)) {
    // A late captain restore receipt after leaving the room: validate, then
    // drop.
    static_cast<void>(protocol::DecodeCaptainRestoreRequest(packet.body));
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if ((packet.opcode == 754U || packet.opcode == 755U ||
       packet.opcode == 32934U || packet.opcode == 32902U) &&
      !IsRoomPhase(actor.state.phase)) {
    // A late racing report after leaving the room: validate strictly, then
    // drop.
    if (packet.opcode == 754U)
      static_cast<void>(protocol::DecodeExtremeRacingFinish(packet.body));
    else if (packet.opcode == 755U)
      static_cast<void>(protocol::DecodeExtremeRacingCoin(packet.body));
    else if (packet.opcode == 32902U)
      static_cast<void>(
          protocol::DecodeExtremeRacingCannonProjectile(packet.body));
    else
      static_cast<void>(protocol::DecodeExtremeRacingCheckpoint(packet.body));
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (packet.opcode == 760U && !IsRoomPhase(actor.state.phase)) {
    protocol::ValidateWeaponMasterReroll(packet.body);
    return WorldCoordinatorResult{};
  }
  if (!IsRoomPhase(actor.state.phase) &&
      (packet.opcode == 701U || packet.opcode == 702U ||
       packet.opcode == 709U)) {
    if (packet.opcode == 701U) {
      static_cast<void>(protocol::DecodeLoadingCompleteRequest(packet.body));
    } else if (packet.opcode == 702U) {
      static_cast<void>(protocol::DecodeMapAssetsRequest(packet.body));
    } else {
      static_cast<void>(
          protocol::DecodePlayerObjectIdentityRequest(packet.body));
    }
    return WorldCoordinatorResult{};
  }
  if (packet.opcode == 32840U && !IsRoomPhase(actor.state.phase)) {
    // 离房响应与客户端探测队列可能交错；严格校验后忽略旧探测，不跨场景回显。
    static_cast<void>(protocol::DecodeMatchLatencyProbe(packet.body));
    return WorldCoordinatorResult{};
  }
  if (native_combat && !IsRoomPhase(actor.state.phase)) {
    // 回房/回广场响应与客户端网络发送队列并非同一时钟；切场景后可能到达一个旧动作包。
    // 先完成协议字段校验，再以结构化原因丢弃，绝不把它带入新的广场或菜单状态。
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    WorldCoordinatorResult result;
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  switch (packet.opcode) {
    case kTrainingEquipmentOpcode:
    case kTrainingResetOpcode:
      if (!IsRoomPhase(actor.state.phase)) {
        // A late training packet after leaving: validate strictly, then drop.
        protocol::PacketReader reader{packet.body};
        static_cast<void>(reader.ReadU32());
        if (packet.opcode == kTrainingEquipmentOpcode)
          static_cast<void>(reader.ReadU32());
        reader.RequireEnd();
        WorldCoordinatorResult ignored;
        ignored.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
        return ignored;
      }
      break;
    case protocol::kLobbyMinigameSyncOpcode: {
      const auto sync = protocol::DecodeLobbyMinigameSync(packet.body);
      if (actor.state.phase != WorldSessionPhase::kPlaza ||
          !plaza_.Contains(actor.player_id)) {
        // A late visual sync after leaving the plaza is validated and dropped.
        return WorldCoordinatorResult{};
      }
      auto relayed = plaza_.RelayMinigame(actor.player_id, sync);
      if (const auto* error =
              std::get_if<application::ServiceError>(&relayed)) {
        return *error;
      }
      WorldCoordinatorResult result;
      Append(result, std::get<PlazaCoordinatorResult>(std::move(relayed)));
      return result;
    }
    case kTrainingEntryOpcode:
      protocol::PacketReader{packet.body}.RequireEnd();
      return HandleCreate(std::move(actor), packet, now);
    case 109U:
    case 110U:
    case 111U:
    case 112U:
    case 202U:
    case 302U:
    case 303U:
      return HandleParty(std::move(actor), packet, now);
    case 305U:
    case 516U:
    case 2031U:
      return HandleRecruitment(std::move(actor), packet, now);
    case 141U: {
      const auto request = protocol::DecodeSurvivalQueueRequest(packet.body);
      if (request.cancel || actor.state.phase != WorldSessionPhase::kPlaza) {
        WorldCoordinatorResult result;
        result.deliveries.push_back(
            {.recipient = actor.player_id,
             .message = {.opcode = 1011U,
                         .body = protocol::EncodeSurvivalQueueResponse(
                             request.cancel ? 4U : 1U, 1U)}});
        return result;
      }
      return HandleCreate(std::move(actor), packet, now);
    }
    case 53U:
      return HandleChannel(std::move(actor), packet);
    case 105U:
      return HandleReturnToMenu(std::move(actor), packet, now);
    case 106U:
      return HandleEnterPlaza(std::move(actor), packet);
    case 108U:
      return HandlePlazaUserList(actor, packet);
    case 402U:
      return HandleCreate(std::move(actor), packet, now);
    case 403U:
      return HandleJoin(std::move(actor), packet, now);
    case 404U:
      return HandleLeaveRoom(std::move(actor), packet, now);
    case 114U:
      return HandleQuickJoin(std::move(actor), packet, now);
    case 510U:
      return HandleRoomKick(std::move(actor), packet, now);
    case 502U:
      return HandleChat(std::move(actor), packet, now);
    case 511U:
      if (actor.state.phase == WorldSessionPhase::kRoomWaiting) {
        break;
      }
      if (actor.state.phase != WorldSessionPhase::kPlaza) {
        static_cast<void>(protocol::DecodeSceneMovementRequest(packet.body));
        return WorldCoordinatorResult{};
      }
      if (auto moved = plaza_.HandleMovement(actor.player_id, packet.body);
          const auto* error = std::get_if<application::ServiceError>(&moved)) {
        return *error;
      } else {
        WorldCoordinatorResult result;
        Append(result, std::get<PlazaCoordinatorResult>(std::move(moved)));
        return result;
      }
    case 401U:
      if (actor.state.phase != WorldSessionPhase::kMenu &&
          actor.state.phase != WorldSessionPhase::kPlaza) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "当前阶段不允许查询房间列表");
      }
      break;
    default:
      if (!IsRoomPhase(actor.state.phase)) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "当前阶段不允许提交房间命令");
      }
      break;
  }

  auto handled = rooms_.Handle(RoomActor(std::move(actor)), packet, now);
  if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(handled)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::HandleChat(
    WorldActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (actor.muted) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id, "账号处于禁言期间，暂时无法发送消息");
    return result;
  }
  const auto request = protocol::DecodeNearbyChatRequest(packet.body);
  const bool in_plaza = actor.state.phase == WorldSessionPhase::kPlaza;
  const bool in_room = IsRoomPhase(actor.state.phase);
  if (!in_plaza && !in_room) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id, "请先进入广场或房间");
    return result;
  }
  const bool online_count_command = in_plaza && request.text == u"/gm list";
  if (!online_count_command &&
      (request.channel > 1U || (in_plaza && request.channel != 0U))) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id,
                 "该频道尚未加入，请使用附近或队伍聊天");
    return result;
  }

  auto parsed_message = domain::UserMessage::Parse(request.text);
  if (const auto* error =
          std::get_if<domain::UserMessageValidationError>(&parsed_message)) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id,
                 domain::UserMessageValidationErrorMessage(*error));
    return result;
  }
  if (!ConsumeChatQuota(actor.player_id, now)) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id, "发送过快，请稍后再试");
    return result;
  }

  if (in_plaza) {
    if (online_count_command) {
      WorldCoordinatorResult result;
      AppendNotice(result, actor.player_id,
                   "当前服务器的总人数为：" +
                       std::to_string(actor.online_player_count) + "人");
      return result;
    }
    auto handled = plaza_.HandleChat(
        actor.player_id, std::get<domain::UserMessage>(parsed_message),
        actor.chat_blocked_peers);
    if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
      return *error;
    }
    WorldCoordinatorResult result;
    Append(result, std::get<PlazaCoordinatorResult>(std::move(handled)));
    AttachCleanChatHistory(result, actor);
    return result;
  }

  auto handled = rooms_.HandleChat(
      actor.player_id, request.channel == 1U,
      std::get<domain::UserMessage>(parsed_message), actor.chat_blocked_peers);
  if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(handled)));
  AttachCleanChatHistory(result, actor);
  return result;
}

application::ServiceResult<application::MegaphoneCommand>
WorldCoordinator::PrepareMegaphone(
    application::MegaphoneCommand command) const {
  const auto sender = online_players_.find(command.player_id);
  if (sender == online_players_.end() ||
      (sender->second.state.phase != WorldSessionPhase::kMenu &&
       !plaza_.Contains(command.player_id) &&
       !rooms_.FindRoom(command.player_id))) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "喇叭角色已离线或场景状态无效");
  }
  command.channel =
      sender->second.state.channel == 0U ? 60U : sender->second.state.channel;
  return command;
}

WorldCoordinatorResult WorldCoordinator::MegaphoneDeliveries(
    const application::MegaphoneReceipt& receipt,
    const std::vector<domain::PlayerId>& blocked) const {
  WorldCoordinatorResult result;
  if (receipt.published) return result;
  const auto definition = domain::GetMegaphoneDefinition(receipt.item_id);
  if (!definition) return result;
  const auto body = protocol::EncodeMegaphoneResponse(
      receipt.sender_name, receipt.text, receipt.item_id);
  for (const auto& [id, player] : online_players_) {
    if (std::ranges::find(blocked, id) != blocked.end()) continue;
    const auto channel =
        player.state.channel == 0U ? 60U : player.state.channel;
    if (definition->type == 103U && channel != receipt.channel) continue;
    result.deliveries.push_back(
        {.recipient = id, .message = {.opcode = 509U, .body = body}});
  }
  return result;
}

bool WorldCoordinator::ConsumeChatQuota(
    domain::PlayerId player_id, std::chrono::steady_clock::time_point now) {
  auto& times = chat_times_[player_id];
  const auto cutoff = now - std::chrono::seconds(10);
  while (!times.empty() && times.front() <= cutoff) {
    times.pop_front();
  }
  if (times.size() >= 8U) {
    return false;
  }
  times.push_back(now);
  return true;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleChannel(WorldActorContext actor,
                                const protocol::Packet& packet) {
  const auto request = protocol::DecodeChannelSelectionRequest(packet.body);
  if (actor.state.phase != WorldSessionPhase::kMenu &&
      actor.state.phase != WorldSessionPhase::kPlaza) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能从主菜单或广场选择频道");
  }
  if (request.channel != 60U && request.channel != 80U) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id, "请从主界面选择受支持的频道");
    return result;
  }
  if (request.channel == 80U && !actor.has_clan_membership) {
    WorldCoordinatorResult result;
    AppendNotice(result, actor.player_id, "请先加入或创建家族");
    return result;
  }
  if (!HasSnapshot(actor)) {
    return MakeError(application::ErrorCode::kInternal,
                     "进入频道缺少一致性玩家资料快照");
  }
  // Menu/free plaza and native channel 60 share the existing free-plaza pool.
  auto entered = plaza_.Enter(std::move(*actor.player_snapshot),
                              request.channel == 60U ? 0U : request.channel,
                              PartySceneState(actor.player_id, false));
  if (const auto* error = std::get_if<application::ServiceError>(&entered)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(entered)));
  Prepend(result, actor.player_id, 13U,
          protocol::EncodeChannelSelectionResponse(true, request.channel, 1U));
  for (auto& update : result.session_updates) {
    if (update.player_id == actor.player_id) {
      update.channel = request.channel;
    }
  }
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleReturnToMenu(
    WorldActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  static_cast<void>(protocol::DecodeReturnToMenuRequest(packet.body));
  if (!HasSnapshot(actor)) {
    return MakeError(application::ErrorCode::kInternal,
                     "返回主菜单缺少一致性玩家资料快照");
  }

  OutboundMessage menu;
  try {
    menu = {.opcode = 112U,
            .body = protocol::EncodeMenuStateResponse(*actor.player_snapshot)};
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "主菜单资料不能编码为已验证协议");
  }

  WorldCoordinatorResult result;
  if (actor.state.phase == WorldSessionPhase::kPlaza) {
    auto left = plaza_.Leave(actor.player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      return *error;
    }
    Append(result, std::get<PlazaCoordinatorResult>(std::move(left)),
           actor.player_id);
  } else if (IsRoomPhase(actor.state.phase)) {
    auto left = rooms_.Handle(
        {.player_id = actor.player_id, .player_snapshot = std::nullopt},
        {.opcode = 404U, .body = {}}, now);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      return *error;
    }
    Append(result, std::get<RoomCoordinatorResult>(std::move(left)),
           actor.player_id);
  } else if (actor.state.phase != WorldSessionPhase::kMenu) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前阶段不能返回主菜单");
  }

  if (actor.state.channel != 0U) {
    result.deliveries.push_back(
        {.recipient = actor.player_id,
         .message = {
             .opcode = 13U,
             .body = protocol::EncodeChannelSelectionResponse(true, 0U, 1U)}});
  }
  result.deliveries.push_back(
      {.recipient = actor.player_id, .message = std::move(menu)});
  result.session_updates.push_back({.player_id = actor.player_id,
                                    .phase = WorldSessionPhase::kMenu,
                                    .channel = 0U});
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleEnterPlaza(WorldActorContext actor,
                                   const protocol::Packet& packet) {
  static_cast<void>(protocol::DecodeEnterPlazaRequest(packet.body));
  if (actor.state.phase != WorldSessionPhase::kMenu &&
      actor.state.phase != WorldSessionPhase::kPlaza) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能从主菜单进入自由广场");
  }
  if (!HasSnapshot(actor)) {
    return MakeError(application::ErrorCode::kInternal,
                     "进入广场缺少一致性玩家资料快照");
  }
  auto entered = plaza_.Enter(std::move(*actor.player_snapshot), 0U,
                              PartySceneState(actor.player_id, false));
  if (const auto* error = std::get_if<application::ServiceError>(&entered)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(entered)));
  if (actor.state.channel != 60U) {
    Prepend(result, actor.player_id, 13U,
            protocol::EncodeChannelSelectionResponse(true, 60U, 1U));
  }
  for (auto& update : result.session_updates) {
    if (update.player_id == actor.player_id) {
      update.channel = 60U;
    }
  }
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::AssetsCommitted(application::PlayerMenuSnapshot snapshot) {
  const auto online = online_players_.find(snapshot.player_id);
  if (online != online_players_.end()) online->second.snapshot = snapshot;
  auto refreshed = rooms_.AssetsCommitted(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&refreshed))
    return *error;
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(refreshed)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleCreate(WorldActorContext actor,
                               const protocol::Packet& packet,
                               std::chrono::steady_clock::time_point now) {
  // The training bridge entry is also offered from the main menu; that player
  // is not in the plaza.
  const bool from_menu = packet.opcode == kTrainingEntryOpcode &&
                         actor.state.phase == WorldSessionPhase::kMenu;
  if (!from_menu && (actor.state.phase != WorldSessionPhase::kPlaza ||
                     !plaza_.Contains(actor.player_id))) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能从自由广场创建房间");
  }
  auto created = rooms_.Handle(RoomActor(actor), packet, now);
  if (const auto* error = std::get_if<application::ServiceError>(&created)) {
    return *error;
  }
  auto room_result = std::get<RoomCoordinatorResult>(std::move(created));
  if (!room_result.room_id.has_value() || from_menu) {
    WorldCoordinatorResult result;
    Append(result, std::move(room_result));
    return result;
  }

  auto left = plaza_.Leave(actor.player_id);
  if (const auto* error = std::get_if<application::ServiceError>(&left)) {
    const auto rollback = rooms_.Handle(
        {.player_id = actor.player_id, .player_snapshot = std::nullopt},
        {.opcode = 404U, .body = {}}, now);
    static_cast<void>(rollback);
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(left)),
         actor.player_id);
  Append(result, std::move(room_result));
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleQuickJoin(WorldActorContext actor,
                                  const protocol::Packet& packet,
                                  std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeQuickJoinRequest(packet.body);
  const auto reject = [&](std::uint32_t status) {
    WorldCoordinatorResult result;
    result.deliveries.push_back(
        {actor.player_id, {120U, protocol::EncodeQuickJoinResponse(status)}});
    return result;
  };
  // 47D280 的快速进入白名单。频道 80 使用另一个 C2S113 队列协议。
  constexpr std::array modes{1U,  3U,  8U,  11U, 12U, 13U, 14U, 16U, 18U, 19U,
                             20U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 101U};
  const auto channel = [](std::uint32_t value) {
    return value == 0U ? 60U : value;
  };
  if (std::ranges::find(modes, request.mode_id) == modes.end() ||
      channel(actor.state.channel) != 60U)
    return reject(5U);
  if (!HasSnapshot(actor))
    return MakeError(application::ErrorCode::kInternal,
                     "快速进入缺少一致性玩家资料快照");
  const auto group = PartyMembers(actor.player_id);
  if (group.front() != actor.player_id) return reject(2U);
  if (group.size() > 4U && (request.mode_id == 16U || request.mode_id == 101U))
    return reject(request.mode_id == 16U ? 4U : 3U);
  for (const auto id : group) {
    const auto peer = online_players_.find(id);
    if (peer == online_players_.end() ||
        channel(peer->second.state.channel) != 60U || rooms_.FindRoom(id) ||
        HasTutorial(id) ||
        (peer->second.state.phase != WorldSessionPhase::kMenu &&
         peer->second.state.phase != WorldSessionPhase::kPlaza) ||
        (plaza_.Contains(id) !=
         (peer->second.state.phase == WorldSessionPhase::kPlaza)))
      return reject(2U);
  }

  std::optional<domain::RoomId> selected;
  std::uint8_t party_team = 0U;
  for (const auto& scene : rooms_.WaitingScenes()) {
    const domain::RoomId id{scene.scene_id};
    const auto room = rooms_.FindRoom(id);
    if (!room || scene.locked != 0U ||
        scene.configuration.mode_id != request.mode_id)
      continue;
    const auto members = room->Members();
    // 0 是自由广场的本地频道表示；60 是同一自由频道的显式选择。
    // 检查每名成员，不能把 room_channel() 的未知/混合零值当成自由房间。
    if (std::ranges::any_of(members, [&](const auto& member) {
          const auto peer = online_players_.find(member.player_id);
          return peer == online_players_.end() ||
                 channel(peer->second.state.channel) != 60U;
        }))
      continue;
    if (std::ranges::any_of(request.history, [&](const auto& previous) {
          return channel(previous.channel) == 60U &&
                 previous.scene_id == scene.scene_id &&
                 previous.field_68 == scene.field_68;
        }))
      continue;
    const auto usable =
        static_cast<std::size_t>(room->Capacity()) - room->LockedSeats().size();
    if (usable < members.size() + group.size() ||
        (request.size_option == 1U && usable > 6U) ||
        (request.size_option == 2U && usable <= 6U))
      continue;
    if ((request.mode_id == 16U || request.mode_id == 101U) &&
        members.size() + group.size() > 4U)
      continue;
    const bool cooperative = request.mode_id == 8U || request.mode_id == 16U ||
                             request.mode_id == 28U || request.mode_id == 101U;
    if (group.size() > 1U && !cooperative) {
      const auto blue = static_cast<std::size_t>(std::ranges::count_if(
          members, [](const auto& member) { return member.team == 1U; }));
      const auto red = members.size() - blue;
      const auto half = static_cast<std::size_t>(room->Capacity() / 2U);
      if (std::min(blue, red) + group.size() > half) continue;
      party_team = blue <= red ? 1U : 2U;
    }
    selected = id;
    break;
  }
  if (!selected) return reject(2U);

  // 世界 executor 内完成全组校验、加入和离场，外部看不到中间结果。
  // 先暂存所有房间投递；任何加入失败都撤销已加入成员，不广播半支队伍。
  protocol::PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(selected->Value()));
  writer.WriteUtf16(u"");
  writer.WriteU32(0U);
  const protocol::Packet join{.opcode = 403U, .body = writer.Release()};
  WorldCoordinatorResult admitted;
  std::vector<domain::PlayerId> joined_players;
  const auto rollback = [&] {
    for (auto iterator = joined_players.rbegin();
         iterator != joined_players.rend(); ++iterator)
      static_cast<void>(
          rooms_.Handle({.player_id = *iterator}, {.opcode = 404U}, now));
  };
  for (const auto id : group) {
    auto joined = rooms_.Handle(
        {.player_id = id, .player_snapshot = online_players_.at(id).snapshot},
        join, now);
    if (const auto* error = std::get_if<application::ServiceError>(&joined)) {
      rollback();
      return *error;
    }
    auto result = std::get<RoomCoordinatorResult>(std::move(joined));
    if (!result.room_id) {
      rollback();
      return reject(2U);
    }
    joined_players.push_back(id);
    Append(admitted, std::move(result));
    if (party_team != 0U &&
        rooms_.FindRoom(id)->GetMember(id)->team != party_team) {
      auto switched = rooms_.Handle({.player_id = id}, {.opcode = 515U}, now);
      if (const auto* error =
              std::get_if<application::ServiceError>(&switched)) {
        rollback();
        return *error;
      }
      if (rooms_.FindRoom(id)->GetMember(id)->team != party_team) {
        rollback();
        return reject(2U);
      }
      Append(admitted, std::get<RoomCoordinatorResult>(std::move(switched)));
    }
  }
  WorldCoordinatorResult result;
  for (const auto id : group) {
    if (!plaza_.Contains(id)) continue;
    // 前面的成员关系检查和这里之间没有异步挂起，离场索引不会改变。
    auto left = plaza_.Leave(id);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      rollback();
      return *error;
    }
    Append(result, std::get<PlazaCoordinatorResult>(std::move(left)), id);
  }
  for (auto& delivery : admitted.deliveries) {
    if (delivery.recipient == actor.player_id &&
        delivery.message.opcode == 103U)
      delivery.message = {120U, protocol::EncodeQuickJoinResponse(0U)};
    result.deliveries.push_back(std::move(delivery));
  }
  result.event = RoomEvent::kMemberJoined;
  result.room_id = selected;
  result.session_updates = std::move(admitted.session_updates);
  result.active_timers = std::move(admitted.active_timers);
  return result;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::HandleJoin(
    WorldActorContext actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (actor.state.phase != WorldSessionPhase::kPlaza ||
      !plaza_.Contains(actor.player_id)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能从自由广场加入房间");
  }
  auto joined = rooms_.Handle(RoomActor(actor), packet, now);
  if (const auto* error = std::get_if<application::ServiceError>(&joined)) {
    return *error;
  }
  auto room_result = std::get<RoomCoordinatorResult>(std::move(joined));
  if (!room_result.room_id.has_value()) {
    WorldCoordinatorResult result;
    Append(result, std::move(room_result));
    return result;
  }

  auto left = plaza_.Leave(actor.player_id);
  if (const auto* error = std::get_if<application::ServiceError>(&left)) {
    const auto rollback = rooms_.Handle(
        {.player_id = actor.player_id, .player_snapshot = std::nullopt},
        {.opcode = 404U, .body = {}}, now);
    static_cast<void>(rollback);
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<PlazaCoordinatorResult>(std::move(left)),
         actor.player_id);
  Append(result, std::move(room_result));
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleLeaveRoom(WorldActorContext actor,
                                  const protocol::Packet& packet,
                                  std::chrono::steady_clock::time_point now) {
  static_cast<void>(protocol::DecodeLeaveRoomRequest(packet.body));
  if (!IsRoomPhase(actor.state.phase) || !rooms_.FindRoom(actor.player_id)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在活动房间中");
  }
  if (!HasSnapshot(actor)) {
    return MakeError(application::ErrorCode::kInternal,
                     "离开房间进入广场缺少资料快照");
  }

  auto entered = plaza_.Enter(std::move(*actor.player_snapshot), 0U,
                              PartySceneState(actor.player_id, false));
  if (const auto* error = std::get_if<application::ServiceError>(&entered)) {
    return *error;
  }
  auto left = rooms_.Handle(
      {.player_id = actor.player_id, .player_snapshot = std::nullopt}, packet,
      now);
  if (const auto* error = std::get_if<application::ServiceError>(&left)) {
    const auto rollback = plaza_.Leave(actor.player_id);
    static_cast<void>(rollback);
    return *error;
  }

  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(left)),
         actor.player_id);
  Append(result, std::get<PlazaCoordinatorResult>(std::move(entered)));
  if (actor.state.channel != 60U) {
    InsertBeforeActorMessage(
        result, actor.player_id, 112U,
        {.opcode = 13U,
         .body = protocol::EncodeChannelSelectionResponse(true, 60U, 1U)});
  }
  for (auto& update : result.session_updates) {
    if (update.player_id == actor.player_id) {
      update.channel = 60U;
    }
  }
  return result;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::ApplyTimer(
    const RoomTimerToken& token, std::chrono::steady_clock::time_point now) {
  auto applied = rooms_.ApplyTimer(token, now);
  if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::FinishMatch(domain::RoomId room_id,
                              application::MatchCompletionKind completion,
                              std::chrono::steady_clock::time_point now) {
  auto finished = rooms_.FinishMatch(room_id, completion, now);
  if (const auto* error = std::get_if<application::ServiceError>(&finished)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(finished)));
  return result;
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::Disconnect(
    domain::PlayerId player_id, WorldSessionPhase phase,
    std::chrono::steady_clock::time_point now) {
  chat_times_.erase(player_id);
  WorldCoordinatorResult result;
  if (HasTutorial(player_id)) {
    auto tutorial =
        TutorialResult(player_id, single_play_.Contains(player_id)
                                      ? single_play_.Disconnect(player_id)
                                      : tutorials_.Disconnect(player_id));
    result.tutorial_command = std::move(tutorial.tutorial_command);
    result.local_release_owner = std::move(tutorial.local_release_owner);
  }
  LeaveParty(result, player_id, false);
  online_players_.erase(player_id);
  std::erase_if(party_invitations_, [player_id](const auto& invitation) {
    return invitation.requester == player_id || invitation.target == player_id;
  });
  if (dedicated_members_.erase(player_id) != 0U) {
    // Room membership is removed through the DS connection, not the local
    // coordinator.
  } else if (phase == WorldSessionPhase::kPlaza) {
    auto left = plaza_.Leave(player_id);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      return *error;
    }
    Append(result, std::get<PlazaCoordinatorResult>(std::move(left)),
           player_id);
  } else if (IsRoomPhase(phase)) {
    auto left =
        rooms_.Handle({.player_id = player_id, .player_snapshot = std::nullopt},
                      {.opcode = 404U, .body = {}}, now);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      return *error;
    }
    Append(result, std::get<RoomCoordinatorResult>(std::move(left)), player_id);
  }
  std::erase_if(result.deliveries, [player_id](const auto& delivery) {
    return delivery.recipient == player_id;
  });
  return result;
}

application::ServiceResult<bool> WorldCoordinator::InstallCombat(
    domain::RoomId room_id, const RoomMatchInstallation& installation) {
  return rooms_.InstallCombat(room_id, installation);
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::AbortFailedMatchStart(domain::RoomId room_id,
                                        std::string_view notice) {
  auto aborted = rooms_.AbortFailedMatchStart(room_id, notice);
  if (const auto* error = std::get_if<application::ServiceError>(&aborted)) {
    return *error;
  }
  WorldCoordinatorResult result;
  Append(result, std::get<RoomCoordinatorResult>(std::move(aborted)));
  return result;
}

application::ServiceResult<application::MatchSettlementCommand>
WorldCoordinator::PendingSettlement(domain::RoomId room_id) const {
  return rooms_.PendingSettlement(room_id);
}

application::ServiceResult<bool> WorldCoordinator::AcknowledgeSettlementStaged(
    domain::RoomId room_id, const domain::MatchId& match_id) {
  return rooms_.AcknowledgeSettlementStaged(room_id, match_id);
}

const PlazaDirectory& WorldCoordinator::Plaza() const noexcept {
  return plaza_;
}

const RoomCoordinator& WorldCoordinator::Rooms() const noexcept {
  return rooms_;
}

bool WorldCoordinator::AuthorizeInstantRespawn(
    const application::UseItemCommand& command) {
  return rooms_.AuthorizeInstantRespawn(command);
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::LevelIconChanged(application::PlayerMenuSnapshot snapshot) {
  WorldCoordinatorResult result;
  if (plaza_.Contains(snapshot.player_id)) {
    auto applied = plaza_.LevelIconChanged(snapshot);
    if (const auto* error = std::get_if<application::ServiceError>(&applied))
      return *error;
    Append(result, std::get<PlazaCoordinatorResult>(std::move(applied)));
  } else if (rooms_.FindRoom(snapshot.player_id)) {
    auto applied = rooms_.LevelIconChanged(snapshot);
    if (const auto* error = std::get_if<application::ServiceError>(&applied))
      return *error;
    Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  } else {
    result.deliveries.push_back(
        {.recipient = snapshot.player_id,
         .message = {
             .opcode = 973U,
             .body = protocol::EncodeLevelIconResponse({.slot = 0U,
                                                        .team = 0U,
                                                        .occupied = true,
                                                        .locked = false,
                                                        .ready = false,
                                                        .player = snapshot})}});
  }
  if (const auto found = online_players_.find(snapshot.player_id);
      found != online_players_.end())
    found->second.snapshot = std::move(snapshot);
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::CoupleChanged(application::PlayerMenuSnapshot snapshot) {
  WorldCoordinatorResult result;
  if (const auto found = online_players_.find(snapshot.player_id);
      found != online_players_.end()) {
    application::SyncNativeTitleEquipmentFlags(found->second.snapshot,
                                               snapshot);
    found->second.snapshot.couple = snapshot.couple;
  }
  if (rooms_.FindRoom(snapshot.player_id)) {
    auto applied = rooms_.CoupleChanged(std::move(snapshot));
    if (const auto* error = std::get_if<application::ServiceError>(&applied))
      return *error;
    Append(result, std::get<RoomCoordinatorResult>(std::move(applied)));
  } else if (plaza_.Contains(snapshot.player_id)) {
    auto applied = plaza_.CoupleChanged(snapshot);
    if (const auto* error = std::get_if<application::ServiceError>(&applied))
      return *error;
    Append(result, std::get<PlazaCoordinatorResult>(std::move(applied)));
  }
  return result;
}

}  // namespace bf::runtime
