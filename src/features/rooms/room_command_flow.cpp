#include "features/rooms/room_command_flow.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "features/battle/bomb_maps.h"
#include "features/battle/event_mole_profile.h"
#include "features/battle/mode_weapon.h"
#include "features/battle/native_match_duration.h"
#include "features/battle/native_team_spawn.h"
#include "features/battle/pirate_maps.h"
#include "features/battle/sniper_weapon.h"
#include "features/battle/zombie_pve_maps.h"
#include "features/characters/native_title_profile.h"
#include "features/practice/horde_tutor_profile.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/chat/chat_messages.h"
#include "protocol/clans/clan_messages.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_item_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::runtime {
namespace {

constexpr std::string_view kSoccerMap = "rel_event_Worldcup00";
constexpr std::string_view kSoccerMapName = "足球大战";

[[nodiscard]] bool DefaultDurationEcho(std::uint32_t current,
                                       std::uint32_t requested) {
  if (current == requested) return true;
  // 98C040 selects the closest of these five labels. Merely confirming that
  // dialog turns a 239/241-second default into 240000 (or a 2-minute event into
  // 180000); that is not evidence of a deliberate host duration change.
  constexpr std::array options{180'000U, 240'000U, 300'000U, 360'000U,
                               1'200'000U};
  const auto nearest =
      std::ranges::min(options, {}, [current](std::uint32_t value) {
        return value > current ? value - current : current - value;
      });
  return requested == nearest;
}

void NormalizeSoccerConfiguration(protocol::SceneConfiguration& configuration) {
  // MapMeta.isSoccerEvent and GameOption.soccerEventMap identify one released
  // pitch. Its real MapMeta key also selects the original client's preview and
  // load branch.
  if (configuration.mode_id == 11U) {
    configuration.family_key = kSoccerMap;
    configuration.map_key = kSoccerMap;
    configuration.display_name = kSoccerMapName;
    configuration.auxiliary_label.clear();
    for (auto& map : configuration.rotating_maps) {
      map.family_key = kSoccerMap;
      map.map_key = kSoccerMap;
    }
    return;
  }
  // Mode changes can carry the previous mode's map fields back to the server.
  if (configuration.family_key == kSoccerMap ||
      configuration.map_key == kSoccerMap) {
    configuration.family_key = "Random";
    configuration.map_key.clear();
    configuration.display_name = "随机地图";
    configuration.auxiliary_label.clear();
  }
  for (auto& map : configuration.rotating_maps) {
    if (map.family_key == kSoccerMap || map.map_key == kSoccerMap) {
      map = {.family_key = "Random", .map_key = {}};
    }
  }
}

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

[[nodiscard]] application::ServiceError PendingJournalError() {
  return application::ServiceError{
      .code = application::ErrorCode::kStorageUnavailable,
      .safe_message = "终局快照尚未获得耐久日志回执，暂不能返回等待房间",
      .correlation_id = {},
      .is_retryable = true,
  };
}

[[nodiscard]] bool FitsProtocolMilliseconds(std::chrono::milliseconds duration,
                                            bool allow_zero) noexcept {
  const auto count = duration.count();
  return count >= (allow_zero ? 0 : 1) &&
         static_cast<std::uint64_t>(count) <=
             std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] std::uint32_t ProtocolMilliseconds(
    std::chrono::milliseconds duration) {
  return static_cast<std::uint32_t>(duration.count());
}

[[nodiscard]] std::optional<RoomMemberSnapshot> MemberBySeat(
    const std::vector<RoomMemberSnapshot>& members, std::uint32_t seat) {
  const auto iterator = std::ranges::find(
      members, static_cast<std::uint16_t>(seat), &RoomMemberSnapshot::seat);
  if (iterator == members.end()) {
    return std::nullopt;
  }
  return *iterator;
}

/**
 * @brief 队长模式专用包：32894 变身/饮料、707 饮料申请、710/27 队长击败、2021
 * 恢复、 32812 属性 29/11 清理。其他 707/32894/710/32812
 * 保持原有道具、目标与战斗路径。
 */
bool CaptainPacket(const protocol::Packet& packet) {
  if (packet.opcode == 2021U) return true;
  if (packet.opcode == 710U)
    return !packet.body.empty() && packet.body.front() == 27U;
  if ((packet.opcode != 707U && packet.opcode != 32894U &&
       packet.opcode != 32812U) ||
      packet.body.size() != 8U)
    return false;
  protocol::PacketReader reader{packet.body};
  const auto first = reader.ReadU32();
  const auto type = reader.ReadU32();
  if (packet.opcode == 707U) return first == 0xFFFFFFFFU && type == 104U;
  if (packet.opcode == 32812U) return type == 29U || type == 11U;
  return type == 104U || type == 230U || type == 231U || type == 235U ||
         type == 236U;
}

/** @brief 754/755/32934 的严格格式校验；不在竞速局时只校验后丢弃。 */
void ValidateExtremeRacingRequest(const protocol::Packet& packet) {
  if (packet.opcode == 754U)
    static_cast<void>(protocol::DecodeExtremeRacingFinish(packet.body));
  else if (packet.opcode == 755U)
    static_cast<void>(protocol::DecodeExtremeRacingCoin(packet.body));
  else
    static_cast<void>(protocol::DecodeExtremeRacingCheckpoint(packet.body));
}

}  // namespace

RoomCommandResult RoomCommandFlow::ClanMarkChanged(domain::PlayerId player,
                                                   std::uint32_t mark) {
  auto result = MakeResult(RoomEvent::kNoChange);
  const auto found = players_.find(player);
  if (found == players_.end()) {
    return result;
  }
  found->second.clan_mark = mark;
  for (const auto& member : room_->Members()) {
    if (member.player_id != player) {
      continue;
    }
    protocol::PacketWriter writer;
    writer.WriteU32(member.seat);
    writer.WriteU32(mark);
    const OutboundMessage message{.opcode = 991U, .body = writer.Release()};
    Broadcast(result, std::span{&message, 1U});
    break;
  }
  return result;
}

application::ServiceResult<std::unique_ptr<RoomCommandFlow>>
RoomCommandFlow::Create(std::shared_ptr<RoomRuntime> room,
                        protocol::SceneInfo scene,
                        std::vector<application::PlayerMenuSnapshot> players,
                        RoomMatchTiming timing) {
  if (!room) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间命令路由缺少运行时对象");
  }
  const auto members = room->Members();
  const auto room_id = room->RoomId().Value();
  if (room_id > std::numeric_limits<std::uint16_t>::max() ||
      scene.scene_id != static_cast<std::uint16_t>(room_id) ||
      scene.kind != 2U || scene.scene_type != 2U ||
      scene.capacity != room->Capacity() ||
      scene.occupants != room->Participants().size() ||
      scene.observers != members.size() - room->Participants().size() ||
      (scene.configuration.field_12 != 0U) != room->UserHosted() ||
      scene.configuration.mode_id != room->ModeId()) {
    return MakeError(application::ErrorCode::kInternal,
                     "SceneInfo 与房间运行时身份、容量或模式不一致");
  }
  if (!FitsProtocolMilliseconds(timing.loading_timeout, false) ||
      !FitsProtocolMilliseconds(timing.countdown, true) ||
      !FitsProtocolMilliseconds(timing.match_duration, false) ||
      timing.result_transition < std::chrono::milliseconds::zero() ||
      timing.result_duration <= std::chrono::milliseconds::zero() ||
      timing.result_transition > timing.result_duration) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间计时超出运行时或协议范围");
  }
  if (timing.user_host_prize.minimum_amount == 0U ||
      timing.user_host_prize.minimum_amount >
          timing.user_host_prize.maximum_amount ||
      timing.user_host_prize.maximum_amount > 2147483647U)
    return MakeError(application::ErrorCode::kInternal, "主办奖金配置范围无效");
  if (players.size() != members.size()) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间成员资料快照数量不一致");
  }

  std::map<domain::PlayerId, application::PlayerMenuSnapshot> snapshots;
  for (auto& player : players) {
    if (!player.player_id.IsValid() ||
        !room->GetMember(player.player_id).has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "房间资料快照包含非成员玩家");
    }
    const auto [unused_iterator, inserted] =
        snapshots.emplace(player.player_id, std::move(player));
    static_cast<void>(unused_iterator);
    if (!inserted) {
      return MakeError(application::ErrorCode::kInternal,
                       "房间资料快照包含重复玩家");
    }
  }

  NormalizePirateConfiguration(scene.configuration);
  NormalizeZombiePveConfiguration(scene.configuration);
  NormalizeBombConfiguration(scene.configuration);
  NormalizeSoccerConfiguration(scene.configuration);
  NormalizeEventMoleConfiguration(scene.configuration);
  NormalizeCrazyRacingConfiguration(scene.configuration);
  if (timing.use_room_duration) ApplyNativeMatchDuration(scene.configuration);
  if (scene.configuration.mode_id == 19U) {
    scene.configuration.field_8 |= 1U;
  }
  auto flow = std::unique_ptr<RoomCommandFlow>(new RoomCommandFlow(
      std::move(room), std::move(scene), std::move(snapshots), timing));
  try {
    static_cast<void>(protocol::EncodeSceneSessionResponse(
        flow->RoomSnapshot(flow->room_->HostPlayerId())));
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间资料快照不能编码为已验证协议");
  }
  return flow;
}

RoomCommandFlow::RoomCommandFlow(
    std::shared_ptr<RoomRuntime> room, protocol::SceneInfo scene,
    std::map<domain::PlayerId, application::PlayerMenuSnapshot> players,
    RoomMatchTiming timing)
    : room_(std::move(room)),
      scene_(std::move(scene)),
      players_(std::move(players)),
      timing_(timing),
      default_match_duration_(timing.match_duration) {}

bool RoomCommandFlow::NativeLoopbackOrderUncertain(
    domain::PlayerId player) const noexcept {
  if (!combat_) return true;
  const auto actor = combat_->actors_.find(player);
  return actor == combat_->actors_.end() ||
         combat_->NativeLoopbackOrderUncertain(
             actor->second.participant.actor_object_id);
}

std::uint64_t RoomCommandFlow::ObservedNativeConstructors(
    domain::PlayerId receiver, std::uint32_t owner_object) const noexcept {
  return combat_ ? combat_->ObservedNativeConstructors(receiver, owner_object)
                 : 0U;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::Handle(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now, std::string_view peer_address,
    const std::optional<application::IceLease>& ice_lease) {
  if (packet.opcode == protocol::kRoomNetworkRegister)
    return HandleNetworkRegister(actor, packet.body, peer_address, ice_lease);
  if (packet.opcode == 65077U) return HandleIceSignal(actor, packet.body, now);
  if (packet.opcode == protocol::kRoomNetworkMovement)
    return HandleNetworkMovement(actor, packet.body, now);
  const auto member = room_->GetMember(actor);
  const bool watch_constructor =
      (room_->ModeId() == 17U || room_->ModeId() == 21U) && member &&
      !member->IsObserver() &&
      (room_->Phase() == RoomPhase::kLoading ||
       room_->Phase() == RoomPhase::kCountdown ||
       room_->Phase() == RoomPhase::kPlaying) &&
      CombatRelay::IsNativeConstructorOpcode(packet.opcode);
  const auto owner_object =
      member ? 1'000'000U + static_cast<std::uint32_t>(member->seat) * 100'000U
             : 0U;
  const auto previous =
      watch_constructor && combat_
          ? combat_->ObservedNativeConstructors(actor, owner_object)
          : 0U;
  const auto retain_gap = [&](bool failed) {
    if (!watch_constructor) return;
    if (combat_) {
      if (failed ||
          combat_->ObservedNativeConstructors(actor, owner_object) == previous)
        combat_->MarkNativeConstructorGap(actor, packet.opcode);
    } else if (room_->Phase() == RoomPhase::kLoading ||
               room_->Phase() == RoomPhase::kCountdown ||
               room_->Phase() == RoomPhase::kPlaying) {
      // Original 701 skips an existing slot, so starting combat cannot erase a
      // local routing1 constructor that was refused before the snapshot
      // existed.
      native_prestart_gaps_.try_emplace(actor, packet.opcode);
    }
  };
  auto result = [&]() {
    try {
      auto handled = HandleImpl(actor, packet, now);
      retain_gap(false);
      return handled;
    } catch (...) {
      retain_gap(true);
      throw;
    }
  }();
  if (auto* value = std::get_if<RoomCommandResult>(&result);
      value && packet.opcode != protocol::kRoomNetworkRequest &&
      packet.opcode != protocol::kRoomItemRequest &&
      !(packet.opcode == protocol::kLegacyRoomItemRequest &&
        protocol::LegacyRoomItemLayout(packet.body)) &&
      (value->event != RoomEvent::kNoChange || packet.opcode == 709U)) {
    AppendNetworkState(*value);
    AppendNetworkPeers(*value);
  }
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleImpl(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (packet.opcode == protocol::kRoomItemRequest)
    return HandleItemSetting(actor, packet.body, protocol::kRoomItemResponse);
  if (packet.opcode == protocol::kLegacyRoomItemRequest &&
      protocol::LegacyRoomItemLayout(packet.body))
    return HandleItemSetting(actor, packet.body,
                             protocol::kLegacyRoomItemResponse);
  if (packet.opcode == protocol::kRoomNetworkRequest)
    return HandleNetworkSetting(actor, packet.body);
  if (!room_->GetMember(actor).has_value() ||
      room_->Phase() == RoomPhase::kClosed) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在活动房间中");
  }
  if (packet.opcode == hide_seek_wire::request_opcode)
    return HandleHideSeek(actor, packet, now);
  if (packet.opcode == 32918U ||
      (packet.opcode == 32892U && room_->ModeId() == kEventMoleMode)) {
    if (packet.opcode == 32918U)
      static_cast<void>(protocol::DecodeObservedMoleState(packet.body));
    else
      static_cast<void>(protocol::DecodeObservedMoleSpawn(packet.body));
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = CombatRejectReason::kUnrecoveredAction;
    result.rule_rejection_detail =
        "EventMole object registration blocked: decoded packet does not prove cookie/owner/type";
    return result;
  }
  if (packet.opcode == 2028U || packet.opcode == 2029U)
    return HandleUserHost(actor, packet, now);
  if (packet.opcode == 756U || packet.opcode == 757U || packet.opcode == 759U ||
      packet.opcode == 758U ||
      (room_->ModeId() == 21U &&
       (packet.opcode == 708U || packet.opcode == 32894U)) ||
      (room_->ModeId() == 21U && packet.opcode == 2026U &&
       !packet.body.empty() && packet.body[0] == 32U)) {
    CrazyRacingMatch::Validate(packet);
    auto result = MakeResult(RoomEvent::kNoChange);
    if (room_->ModeId() != 21U || !crazy_racing_ || !combat_ ||
        room_->Phase() != RoomPhase::kPlaying)
      return result;
    const auto timers = room_->ActiveTimers();
    const auto expired = std::ranges::find_if(timers, [&](const auto& timer) {
      return timer.kind == RoomTimerKind::kMatchTimeout &&
             now >= timer.deadline;
    });
    if (expired != timers.end()) return ApplyTimer(*expired, now);
    bool accepted = false;
    if (packet.opcode == 757U || packet.opcode == 759U ||
        packet.opcode == 708U || packet.opcode == 32894U) {
      auto messages = crazy_racing_->Interact(actor, packet, *combat_, now);
      accepted = !messages.empty();
      for (const auto& target : room_->Members())
        if (packet.opcode != 32894U || target.player_id != actor)
          for (const auto& message : messages)
            result.deliveries.push_back({target.player_id, message});
    } else {
      const auto previous = crazy_racing_->Winner();
      accepted = crazy_racing_->Handle(actor, room_->HostPlayerId(), packet,
                                       *combat_, now);
      if (accepted && packet.opcode == 756U && previous == 0U) {
        for (const auto& target : room_->Members())
          result.deliveries.push_back(
              {target.player_id, {771U, {crazy_racing_->Winner()}}});
      }
    }
    if (!accepted)
      result.combat_reject_reason = CombatRejectReason::kInvalidLifecycleState;
    auto advanced = AdvanceObjectives(now);
    if (auto* value = std::get_if<RoomCommandResult>(&advanced)) {
      value->combat_reject_reason = result.combat_reject_reason;
      value->deliveries.insert(value->deliveries.begin(),
                               result.deliveries.begin(),
                               result.deliveries.end());
    }
    return advanced;
  }
  if (packet.opcode == protocol::kSmartPingOpcode) {
    const auto ping = protocol::DecodeSmartPing(packet.body);
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!combat_ || room_->Phase() != RoomPhase::kPlaying ||
        (objectives_ && objectives_->BetweenRounds())) {
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    if (ping.kind == 4U &&
        (!objectives_ || !objectives_->SmartPingItem(ping.object, now))) {
      result.combat_reject_reason = CombatRejectReason::kUnknownTarget;
      return result;
    }
    auto handled = combat_->HandleSmartPing(actor, ping, now);
    if (const auto* error = std::get_if<application::ServiceError>(&handled))
      return *error;
    auto accepted = std::get<CombatRelayResult>(std::move(handled));
    result.combat_reject_reason = accepted.reject_reason;
    for (auto& delivery : accepted.deliveries)
      result.deliveries.push_back({.recipient = delivery.recipient,
                                   .message = std::move(delivery.message)});
    return result;
  }
  if (packet.opcode == kTrainingResetOpcode ||
      packet.opcode == kTrainingEquipmentOpcode) {
    protocol::PacketReader reader{packet.body};
    const auto first = reader.ReadU32();
    const auto item =
        packet.opcode == kTrainingEquipmentOpcode ? reader.ReadU32() : 0U;
    reader.RequireEnd();
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!training_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    auto changed = packet.opcode == kTrainingResetOpcode
                       ? training_->Reset(actor, first, *combat_)
                       : training_->Equip(actor, first, item, *combat_);
    result.combat_reject_reason = changed.reject_reason;
    for (auto& delivery : changed.deliveries)
      result.deliveries.push_back(
          {delivery.recipient, std::move(delivery.message)});
    return result;
  }
  if (packet.opcode == 950U || packet.opcode == 32922U) {
    return HandleMapRegion(actor, packet, now);
  }
  if (packet.opcode == 2021U && !captain_) {
    // Restore receipts outside a captain match are validated and dropped.
    static_cast<void>(protocol::DecodeCaptainRestoreRequest(packet.body));
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (captain_ && CaptainPacket(packet)) {
    auto prior = AdvanceObjectives(now);
    if (std::holds_alternative<application::ServiceError>(prior)) return prior;
    auto result = std::get<RoomCommandResult>(std::move(prior));
    if (!combat_ || room_->Phase() != RoomPhase::kPlaying ||
        result.settlement_command)
      return result;
    auto event = captain_->Handle(actor, packet, *combat_, now);
    result.combat_reject_reason = event.reject_reason;
    for (auto& delivery : event.deliveries)
      result.deliveries.push_back(
          {delivery.recipient, std::move(delivery.message)});
    return result;
  }
  if (infection_ && InfectionMatch::AcceptsFever(packet)) {
    if (packet.opcode == 707U || packet.opcode == 32894U)
      MatchObjectives::Validate(packet);
    else
      protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    auto prior = AdvanceObjectives(now);
    if (std::holds_alternative<application::ServiceError>(prior)) return prior;
    auto result = std::get<RoomCommandResult>(std::move(prior));
    if (!combat_ || room_->Phase() != RoomPhase::kPlaying ||
        result.settlement_command)
      return result;
    auto event = infection_->HandleFever(actor, packet, *combat_, now);
    result.combat_reject_reason = event.reject_reason;
    for (auto& delivery : event.deliveries)
      result.deliveries.push_back(
          {delivery.recipient, std::move(delivery.message)});
    auto advanced = AdvanceObjectives(now);
    if (std::holds_alternative<application::ServiceError>(advanced))
      return advanced;
    auto current = std::get<RoomCommandResult>(std::move(advanced));
    current.combat_reject_reason = result.combat_reject_reason;
    current.deliveries.insert(current.deliveries.begin(),
                              result.deliveries.begin(),
                              result.deliveries.end());
    return current;
  }
  if (packet.opcode == 515U && room_->Phase() == RoomPhase::kPlaying &&
      (infection_ || zombie_pve_) && combat_) {
    protocol::ValidateInfectionTransitionRequest(packet.body);
    if (infection_) {
      // A late 515 must not turn an expired human-survival round into a zombie
      // win.
      const auto timers = room_->ActiveTimers();
      const auto expired = std::ranges::find_if(timers, [&](const auto& timer) {
        return timer.kind == RoomTimerKind::kMatchTimeout &&
               now >= timer.deadline;
      });
      if (expired != timers.end()) return ApplyTimer(*expired, now);
    }
    auto result = MakeResult(RoomEvent::kNoChange);
    const auto messages = infection_
                              ? infection_->Transition(actor, *combat_, now)
                              : zombie_pve_->Transition(actor, *combat_);
    Broadcast(result, messages);
    if (infection_) {
      // The final conversion must reach every client before 705. Re-evaluate
      // immediately: the last respawn permission may have removed all timers.
      auto advanced = AdvanceObjectives(now);
      if (auto* value = std::get_if<RoomCommandResult>(&advanced))
        value->deliveries.insert(value->deliveries.begin(),
                                 result.deliveries.begin(),
                                 result.deliveries.end());
      return advanced;
    }
    if (zombie_pve_ && zombie_pve_->Winner()) {
      auto finished =
          FinishMatch(application::MatchCompletionKind::kCompleted, now);
      if (auto* value = std::get_if<RoomCommandResult>(&finished))
        value->deliveries.insert(value->deliveries.begin(),
                                 result.deliveries.begin(),
                                 result.deliveries.end());
      return finished;
    }
    return result;
  }
  if ((protocol::IsPveEventOpcode(packet.opcode) &&
       !(room_->ModeId() == 3U && packet.opcode == 729U)) ||
      (zombie_pve_ && (packet.opcode == 707U || packet.opcode == 32894U))) {
    if (protocol::IsPveEventOpcode(packet.opcode))
      protocol::ValidatePveEvent(packet.opcode, packet.body);
    else
      MatchObjectives::Validate(packet);
    if (!zombie_pve_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
      auto result = MakeResult(RoomEvent::kNoChange);
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    auto prior = AdvanceObjectives(now);
    if (std::holds_alternative<application::ServiceError>(prior)) return prior;
    auto result = std::get<RoomCommandResult>(std::move(prior));
    if (room_->Phase() != RoomPhase::kPlaying || result.settlement_command)
      return result;
    auto event = zombie_pve_->HandleEvent(actor, packet, *combat_, now);
    result.combat_reject_reason = event.reject_reason;
    for (auto& delivery : event.deliveries)
      result.deliveries.push_back({.recipient = delivery.recipient,
                                   .message = std::move(delivery.message)});
    auto advanced = AdvanceObjectives(now);
    if (std::holds_alternative<application::ServiceError>(advanced))
      return advanced;
    auto current = std::get<RoomCommandResult>(std::move(advanced));
    current.combat_reject_reason = result.combat_reject_reason;
    current.deliveries.insert(
        current.deliveries.begin(),
        std::make_move_iterator(result.deliveries.begin()),
        std::make_move_iterator(result.deliveries.end()));
    return current;
  }
  if (protocol::IsSpecialModeClaimOpcode(packet.opcode)) {
    protocol::ValidateSpecialModeClaim(packet.opcode, packet.body);
    if (zombie_pve_ && combat_ && room_->Phase() == RoomPhase::kPlaying) {
      zombie_pve_->Claim(actor, packet);
      return AdvanceObjectives(now);
    }
    // 7CA9E0/7CA940 是客户端预测，不是服务端胜负授权。尤其 DB7BD0 在 905 后
    // 还会发送 725(1)，不能让迟到预测重复结算或污染下一局。
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = room_->Phase() == RoomPhase::kPlaying
                                      ? CombatRejectReason::kUnrecoveredAction
                                      : CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (PirateMapAgents::AcceptsLink(packet.opcode)) {
    PirateMapAgents::ValidateLink(packet);
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!combat_ || !pirate_ || room_->Phase() != RoomPhase::kPlaying)
      return result;
    Broadcast(result, map_agents_.Tick(*combat_, now));
    auto linked = map_agents_.HandleLink(actor, packet, *combat_, now);
    result.combat_reject_reason = linked.reject_reason;
    for (auto& delivery : linked.deliveries)
      result.deliveries.push_back({.recipient = delivery.recipient,
                                   .message = std::move(delivery.message)});
    return result;
  }
  if (PirateMapAgents::Accepts(packet.opcode)) {
    PirateMapAgents::Validate(packet);
    auto result = MakeResult(RoomEvent::kNoChange);
    if (combat_ && room_->Phase() == RoomPhase::kPlaying)
      Broadcast(result, map_agents_.Tick(*combat_, now));
    const auto host = room_->GetMember(room_->HostPlayerId());
    const auto phase = room_->Phase();
    const bool staged_npc_mode =
        zombie_pve_ || room_->ModeId() == 101U || room_->ModeId() == 102U;
    if (staged_npc_mode && scene_.configuration.submode_id != 4U &&
        (phase == RoomPhase::kLoading || phase == RoomPhase::kCountdown ||
         (phase == RoomPhase::kPlaying && !combat_))) {
      // 7E3F30/7E4000 describe a one-shot path destination. Dropping it while
      // retaining only32892 leaves peers without the destination, and applying
      // all spawns first also rejects a legitimate remove/reuse of one slot.
      if (actor == room_->HostPlayerId() && host && host->object_id)
        return StageNpcPrestart(actor, packet);
      return result;
    }
    if (scene_.configuration.submode_id == 4U ||
        (room_->ModeId() != 101U && room_->ModeId() != 102U &&
         room_->ModeId() != 16U) ||
        actor != room_->HostPlayerId() || !host || !host->object_id ||
        (phase != RoomPhase::kLoading && phase != RoomPhase::kCountdown &&
         phase != RoomPhase::kPlaying) ||
        (phase == RoomPhase::kLoading && packet.opcode != 32892U) ||
        !(zombie_pve_ ? zombie_pve_->MapMessage(packet, combat_.get())
                      : map_agents_.Apply(packet, *host->object_id,
                                          combat_.get(), now))) {
      if (zombie_pve_ && actor == room_->HostPlayerId() &&
          phase == RoomPhase::kPlaying) {
        protocol::PacketReader reader{packet.body};
        result.rule_rejection_detail =
            "PVE map agent rejected opcode=" + std::to_string(packet.opcode) +
            " object_or_spawner=" + std::to_string(reader.ReadU32());
        if (packet.opcode == 32890U || packet.opcode == 32893U)
          *result.rule_rejection_detail +=
              " slot=" + std::to_string(reader.ReadU32());
      }
      return result;
    }
    const OutboundMessage message{.opcode = packet.opcode, .body = packet.body};
    if (phase == RoomPhase::kLoading) {
      loading_map_agents_.push_back(message);
    } else {
      for (const auto& member : room_->Members()) {
        if (member.player_id != actor) {
          result.deliveries.push_back(
              {.recipient = member.player_id, .message = message});
        }
      }
    }
    if ((packet.opcode == 32893U || (zombie_pve_ && packet.opcode == 32890U)) &&
        phase == RoomPhase::kPlaying) {
      // Install the earliest real expiry even when no deferred action follows.
      // A late first binding is removed before its queued weapon/fire can run.
      auto advanced = AdvanceObjectives(now);
      if (const auto* error = std::get_if<application::ServiceError>(&advanced))
        return *error;
      auto timed = std::get<RoomCommandResult>(std::move(advanced));
      timed.deliveries.insert(
          timed.deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
      result = std::move(timed);
      if (room_->Phase() != RoomPhase::kPlaying) return result;
      protocol::PacketReader reader{packet.body};
      const auto object = reader.ReadU32();
      const auto deferred_actions =
          zombie_pve_ ? zombie_pve_->TakeDeferredActions(object, now)
                      : map_agents_.TakeDeferredActions(object);
      for (const auto& deferred : deferred_actions) {
        auto replayed = HandleCombat(actor, deferred, now);
        if (const auto* error =
                std::get_if<application::ServiceError>(&replayed))
          return *error;
        auto next = std::get<RoomCommandResult>(std::move(replayed));
        next.deliveries.insert(
            next.deliveries.begin(),
            std::make_move_iterator(result.deliveries.begin()),
            std::make_move_iterator(result.deliveries.end()));
        result = std::move(next);
        if (room_->Phase() != RoomPhase::kPlaying) break;
      }
    }
    return result;
  }
  if (PirateMatch::Accepts(packet.opcode)) {
    return HandleCombat(actor, packet, now);
  }
  if (CombatRelay::IsPeerEffectOpcode(packet.opcode)) {
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!combat_ || room_->Phase() != RoomPhase::kPlaying) {
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    auto relayed = combat_->RelayPeerEffect(actor, packet, now);
    result.combat_reject_reason = relayed.reject_reason;
    for (auto& delivery : relayed.deliveries)
      result.deliveries.push_back(
          {delivery.recipient, std::move(delivery.message)});
    return result;
  }
  if (packet.opcode == 32902U) {
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!racing_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
      static_cast<void>(
          protocol::DecodeExtremeRacingCannonProjectile(packet.body));
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    combat_->SetHost(room_->HostPlayerId());
    auto fired = racing_->CannonProjectile(actor, packet, *combat_, now);
    result.combat_reject_reason = fired.reject_reason;
    for (auto& delivery : fired.deliveries)
      result.deliveries.push_back(
          {delivery.recipient, std::move(delivery.message)});
    return result;
  }
  if (packet.opcode == 754U || packet.opcode == 755U ||
      packet.opcode == 32934U) {
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!racing_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
      ValidateExtremeRacingRequest(packet);
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    combat_->SetHost(room_->HostPlayerId());
    auto update = racing_->Request(actor, packet, *combat_, now);
    if (update.rejection)
      result.combat_reject_reason = CombatRejectReason::kInvalidLifecycleState;
    return ApplyRacingUpdate(std::move(result), std::move(update), now);
  }
  if (packet.opcode == 760U ||
      (packet.opcode == 2026U && !packet.body.empty() &&
       packet.body.front() == 39U)) {
    if (packet.opcode == 760U)
      protocol::ValidateWeaponMasterReroll(packet.body);
    else
      static_cast<void>(protocol::DecodeWeaponMasterAssist(packet.body));
    auto result = MakeResult(RoomEvent::kNoChange);
    if (!weapon_master_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
      result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
      return result;
    }
    auto update = weapon_master_->Request(actor, packet, *combat_, now);
    result.combat_reject_reason = update.rejection;
    Broadcast(result, update.messages);
    PreserveCombatPerformance();
    if (update.finished) {
      auto finished =
          FinishMatch(application::MatchCompletionKind::kCompleted, now);
      if (auto* value = std::get_if<RoomCommandResult>(&finished))
        value->deliveries.insert(value->deliveries.begin(),
                                 result.deliveries.begin(),
                                 result.deliveries.end());
      return finished;
    }
    if (update.score_changed) AppendScore(result, 255U);
    if (weapon_master_->Deadline()) {
      room_->SetObjectiveDeadline(weapon_master_->Deadline());
      room_->SuspendMatchTimeout();
      result.active_timers = room_->ActiveTimers();
    }
    return result;
  }
  // 7CBF90 sends soccer goals through the reliable 710/event-21 path as well
  // as the peer effect 32878. Other modes retain the opaque 710 policy.
  if (MatchObjectives::Accepts(packet.opcode) ||
      (room_->ModeId() == 11U && packet.opcode == 710U)) {
    return HandleObjective(actor, packet, now);
  }

  if (room_->Phase() != RoomPhase::kWaiting && packet.opcode != 506U &&
      packet.opcode != 517U && packet.opcode != 518U &&
      protocol::ValidateRoomControlMessage(packet.opcode, packet.body)) {
    return MakeResult(RoomEvent::kNoChange);
  }

  if (protocol::IsNativeStormOpcode(packet.opcode) ||
      protocol::IsNativeWeaponObjectOpcode(packet.opcode) ||
      protocol::IsNativeActionOpcode(packet.opcode) ||
      protocol::IsNativeHitOpcode(packet.opcode) ||
      protocol::IsNativeLifecycleOpcode(packet.opcode) ||
      protocol::IsNativeRescueOpcode(packet.opcode) ||
      protocol::IsNativeRespawnOpcode(packet.opcode)) {
    return HandleCombat(actor, packet, now);
  }

  switch (packet.opcode) {
    case 503U:
      return HandleStart(actor, packet.body, now, false);
    case 504U:
      return HandleStart(actor, packet.body, now, true);
    case 505U:
      return HandleStatus(actor, packet.body);
    case 506U:
      protocol::PacketReader{packet.body}.RequireEnd();
      if (scene_.configuration.submode_id != 4U ||
          room_->Phase() != RoomPhase::kPlaying)
        return MakeResult(RoomEvent::kNoChange);
      return FinishMapBrowse();
    case 508U:
      return HandleTransferHost(actor, packet.body);
    case 509U:
      return HandleSeatLock(actor, packet.body);
    case 511U:
      return HandleSceneMovement(actor, packet.body);
    case 513U:
      return HandleConfiguration(actor, packet.body);
    case 514U:
      return HandleRotatingMap(actor, packet.body);
    case 515U:
      return HandleSwitchTeam(actor, packet.body);
    case 517U:
      return HandleLatencyProbe(actor, packet.body, now);
    case 518U:
      return HandleLatencyReport(actor, packet.body, now);
    case 32840U:
      return HandleMatchLatencyProbe(actor, packet.body);
    case 701U:
    case 702U:
    case 709U:
      return HandleLoading(actor, packet.opcode, packet.body, now);
    default:
      return MakeError(application::ErrorCode::kInvalidState,
                       "当前房间流程不接受该 opcode");
  }
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleSceneMovement(domain::PlayerId actor,
                                     std::span<const std::uint8_t> body) const {
  const auto movement = protocol::DecodeSceneMovementRequest(body);
  if (room_->Phase() != RoomPhase::kWaiting) {
    return MakeResult(RoomEvent::kNoChange);
  }
  const OutboundMessage outbound{
      .opcode = 514U,
      .body = protocol::EncodeSceneMovementResponse(actor, movement)};
  auto result = MakeResult(RoomEvent::kNoChange);
  for (const auto& member : room_->Members()) {
    if (member.player_id != actor) {
      result.deliveries.push_back(
          {.recipient = member.player_id, .message = outbound});
    }
  }
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleChat(
    domain::PlayerId actor, bool team_only, const domain::UserMessage& message,
    std::span<const domain::PlayerId> blocked_peers) const {
  const auto sender = room_->GetMember(actor);
  const auto player = players_.find(actor);
  if (!sender.has_value() || player == players_.end() ||
      room_->Phase() == RoomPhase::kClosed) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在活动房间中");
  }

  OutboundMessage outbound;
  std::optional<OutboundMessage> enemy_text;
  try {
    outbound = {
        .opcode = 509U,
        .body = protocol::EncodeNearbyChatResponse(
            {.sender_slot = static_cast<std::int16_t>(sender->seat),
             .sender_name = protocol::Utf8ToUtf16(player->second.nickname),
             .team_only = team_only,
             .text = message.Text()})};
    if (room_->ModeId() == 19U && room_->Phase() == RoomPhase::kPlaying &&
        !team_only) {
      // B44060 的 -3
      // 分支只显示聊天窗；避免敌人通过气泡或关键词动作暴露隐藏位置。
      enemy_text = OutboundMessage{
          .opcode = 509U,
          .body = protocol::EncodeNearbyChatResponse(
              {.sender_slot = -3,
               .sender_name = protocol::Utf8ToUtf16(player->second.nickname),
               .team_only = false,
               .text = message.Text()})};
    }
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间聊天身份不能编码为已验证协议");
  }

  auto result = MakeResult(RoomEvent::kNoChange);
  const bool survival_chat =
      (room_->ModeId() == 23U || room_->ModeId() == 25U) &&
      room_->Phase() == RoomPhase::kPlaying && combat_;
  const auto eliminated = [&](const RoomMemberSnapshot& member) {
    return member.IsObserver() ||
           (combat_ &&
            combat_->LifeState(member.player_id) == CombatLifeState::kDefeated);
  };
  for (const auto& recipient : room_->Members()) {
    if (recipient.player_id == actor ||
        (team_only && recipient.team != sender->team) ||
        (survival_chat && eliminated(*sender) && !eliminated(recipient)) ||
        std::ranges::find(blocked_peers, recipient.player_id) !=
            blocked_peers.end()) {
      continue;
    }
    result.deliveries.push_back(
        {.recipient = recipient.player_id,
         .message = enemy_text && recipient.team != sender->team ? *enemy_text
                                                                 : outbound});
  }
  result.deliveries.push_back(
      {.recipient = actor, .message = std::move(outbound)});
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleStatus(
    domain::PlayerId actor, std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeRoomStatusRequest(body);
  if (request.ready && !room_->GetMember(actor)->IsObserver()) {
    const auto message = ItemRejection(players_.at(actor));
    if (!message.empty()) {
      static_cast<void>(room_->SetReady(actor, false));
      auto result = MakeResult(RoomEvent::kNoChange);
      result.deliveries.push_back(
          {actor,
           {109U,
            protocol::EncodeNoticeResponse(protocol::Utf8ToUtf16(message))}});
      const std::array messages{MemberUpdate(actor)};
      Broadcast(result, messages);
      return result;
    }
  }
  auto changed = room_->SetReady(actor, request.ready);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }

  auto result = MakeResult(std::get<RoomEvent>(changed));
  const std::array messages{MemberUpdate(actor)};
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleSwitchTeam(
    domain::PlayerId actor, std::span<const std::uint8_t> body) {
  static_cast<void>(protocol::DecodeSwitchTeamRequest(body));
  auto changed = room_->SwitchTeam(actor);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }
  const auto member = room_->GetMember(actor);
  if (!member.has_value()) {
    return MakeError(application::ErrorCode::kInternal,
                     "切换队伍后成员快照丢失");
  }

  std::vector<OutboundMessage> messages{
      {.opcode = 504U,
       .body = protocol::EncodeRoomTeamChangedResponse(
           {.slot = member->seat, .team = member->team})}};
  if (actor != room_->HostPlayerId()) {
    messages.push_back(MemberUpdate(actor));
  }
  auto result = MakeResult(std::get<RoomEvent>(changed));
  if (result.event == RoomEvent::kTeamChanged)
    CancelUserHostAuthorization(result);
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleTransferHost(domain::PlayerId actor,
                                    std::span<const std::uint8_t> body) {
  const auto nickname =
      protocol::Utf16ToUtf8(protocol::DecodeTransferHostRequest(body));
  if (actor != room_->HostPlayerId()) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有当前房主可以移交房主");
  }
  const auto target =
      std::ranges::find_if(players_, [&nickname](const auto& entry) {
        return entry.second.nickname == nickname;
      });
  if (target == players_.end()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "目标玩家不在当前房间");
  }
  auto changed = room_->TransferHost({.actor = actor, .target = target->first});
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }
  auto result = MakeResult(std::get<RoomEvent>(changed));
  if (result.event != RoomEvent::kHostChanged) {
    return result;
  }
  scene_.field_40 = target->second.nickname;
  const auto target_member = room_->GetMember(target->first);
  if (!target_member.has_value()) {
    return MakeError(application::ErrorCode::kInternal,
                     "移交房主后成员快照丢失");
  }
  const std::array messages{
      OutboundMessage{.opcode = 501U,
                      .body = protocol::EncodeRoomSceneUpdateResponse(scene_)},
      OutboundMessage{
          .opcode = 502U,
          .body = protocol::EncodeRoomHostChangedResponse(target_member->seat)},
      MemberUpdate(actor), MemberUpdate(target->first)};
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleSeatLock(
    domain::PlayerId actor, std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeSeatLockRequest(body);
  if (request.slot > std::numeric_limits<std::uint16_t>::max()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "锁定席位超出运行时范围");
  }
  auto changed = room_->SetSeatLocked(
      actor, static_cast<std::uint16_t>(request.slot), request.locked);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }

  auto result = MakeResult(std::get<RoomEvent>(changed));
  const std::array messages{
      OutboundMessage{.opcode = 512U,
                      .body = protocol::EncodeRoomSeatLockChangedResponse(
                          {.slot = request.slot, .locked = request.locked})}};
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleConfiguration(domain::PlayerId actor,
                                     std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeRoomConfigurationRequest(body);
  auto configuration = scene_.configuration;
  auto duration_override = host_duration_override_;
  protocol::ApplyRoomConfiguration(configuration, request);
  // 513
  // 的配置更新不能凭一个标志把已有参赛席变成固定观战席，也不能撤销已确定的出资身份。
  configuration.field_12 = room_->UserHosted() ? 1U : 0U;
  if (room_->UserHosted() &&
      (configuration.mode_id == 23U || configuration.submode_id == 4U ||
       (configuration.field_8 & 0x80U) != 0U))
    return MakeError(
        application::ErrorCode::kInvalidState,
        "主办观战房间不支持32席模式、地图浏览或尚未恢复的特殊赏金卡分支");
  if (configuration.mode_id == 19U) {
    configuration.field_8 |= 1U;
  } else if (scene_.configuration.mode_id == 19U) {
    configuration.field_8 &= ~1U;
  }
  NormalizePirateConfiguration(configuration);
  NormalizeZombiePveConfiguration(configuration);
  NormalizeBombConfiguration(configuration);
  NormalizeSoccerConfiguration(configuration);
  NormalizeEventMoleConfiguration(configuration);
  NormalizeCrazyRacingConfiguration(configuration);

  const auto& previous = scene_.configuration;
  if (timing_.use_room_duration && configuration.submode_id != 4U) {
    if (configuration.mode_id != previous.mode_id ||
        previous.submode_id == 4U) {
      // A mode-change packet commonly echoes the prior mode's UI duration.
      // Discard that cache, including when leaving a native mode for a legacy
      // one.
      duration_override.reset();
      configuration.selected_duration_ms = 0U;
      configuration.field_24 = 0U;
      configuration.field_28 = 0U;
      configuration.field_32 = 0U;
    } else if (request.selected_duration_ms == 0U) {
      duration_override.reset();
    } else if (request.selected_duration_ms != previous.selected_duration_ms &&
               (duration_override ||
                !DefaultDurationEcho(previous.selected_duration_ms,
                                     request.selected_duration_ms))) {
      duration_override = request.selected_duration_ms;
    }
    if (duration_override && *duration_override > 3'600'000U)
      return MakeError(application::ErrorCode::kInvalidInput,
                       "房间时长超过一小时上限");
    ApplyNativeMatchDuration(configuration, duration_override);
  }
  const bool rules_changed =
      configuration.mode_id != previous.mode_id ||
      configuration.submode_id != previous.submode_id ||
      configuration.selected_duration_ms != previous.selected_duration_ms ||
      configuration.score_target != previous.score_target ||
      configuration.field_319 != previous.field_319 ||
      configuration.field_320 != previous.field_320;
  const bool individual_factions_changed =
      configuration.mode_id == 13U || configuration.mode_id == 23U ||
      previous.mode_id == 13U || previous.mode_id == 23U;
  auto changed = room_->ApplyConfigurationMode(actor, configuration.mode_id,
                                               rules_changed);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }
  scene_.configuration = std::move(configuration);
  host_duration_override_ = duration_override;

  std::vector<OutboundMessage> messages;
  const auto members = room_->Members();
  if ((room_->ModeId() == 101U || room_->ModeId() == 102U) ||
      room_->ModeId() == 16U || room_->ModeId() == 8U ||
      room_->ModeId() == 28U || individual_factions_changed) {
    for (const auto& member : members) {
      if (member.IsObserver()) continue;
      messages.push_back({.opcode = 504U,
                          .body = protocol::EncodeRoomTeamChangedResponse(
                              {.slot = member.seat, .team = member.team})});
    }
  }
  messages.push_back({.opcode = 505U,
                      .body = protocol::EncodeRoomConfigurationResponse(
                          scene_.configuration)});
  for (const auto& member : members) {
    if (member.player_id != room_->HostPlayerId()) {
      messages.push_back(MemberUpdate(member.player_id));
    }
  }

  auto result = MakeResult(std::get<RoomEvent>(changed));
  CancelUserHostAuthorization(result);
  if (rules_changed) user_host_amount_ = 0U;
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleRotatingMap(domain::PlayerId actor,
                                   std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeRotatingMapSelectionRequest(body);
  auto configuration = scene_.configuration;
  protocol::ApplyRotatingMapSelection(configuration, request);
  NormalizePirateConfiguration(configuration);
  NormalizeZombiePveConfiguration(configuration);
  NormalizeBombConfiguration(configuration);
  NormalizeSoccerConfiguration(configuration);
  NormalizeEventMoleConfiguration(configuration);
  NormalizeCrazyRacingConfiguration(configuration);
  if (timing_.use_room_duration)
    ApplyNativeMatchDuration(configuration, host_duration_override_);

  auto changed = room_->ApplyRotatingMapChange(actor);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }
  scene_.configuration = std::move(configuration);

  std::vector<OutboundMessage> messages{
      {.opcode = 505U,
       .body =
           protocol::EncodeRoomConfigurationResponse(scene_.configuration)}};
  for (const auto& member : room_->Members()) {
    if (member.player_id != room_->HostPlayerId()) {
      messages.push_back(MemberUpdate(member.player_id));
    }
  }
  auto result = MakeResult(std::get<RoomEvent>(changed));
  CancelUserHostAuthorization(result);
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleStart(
    domain::PlayerId actor, std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now, bool rotating) {
  protocol::StartSceneResponse start_response;
  std::optional<protocol::StartMatchRequest> normal;
  std::optional<protocol::StartRotatingMatchRequest> rotation;
  bool force = false;
  const bool map_browse = scene_.configuration.submode_id == 4U;
  if (room_->IsHideSeek()) {
    if (map_browse || rotating)
      return MakeError(application::ErrorCode::kInvalidState,
                       "躲猫猫不支持地图浏览或轮换开局");
    if (room_->Phase() == RoomPhase::kWaiting) hide_seek_.reset();
    if (!timing_.hide_seek_scene || !timing_.hide_seek_scene->Valid())
      return MakeError(application::ErrorCode::kInvalidState,
                       "躲猫猫地图模型与碰撞尚未核验，不能开始");
    for (const auto& member : room_->Participants()) {
      const auto capability = hide_seek_capabilities_.find(member.player_id);
      if (capability == hide_seek_capabilities_.end() ||
          (capability->second & hide_seek_wire::required_capabilities) !=
              hide_seek_wire::required_capabilities)
        return MakeError(application::ErrorCode::kInvalidState,
                         "仍有客户端未完成躲猫猫能力协商，不能开始");
    }
    timing_.match_duration = std::chrono::seconds{270};
    timing_.use_room_duration = false;
  }
  if (!map_browse && room_->ModeId() == 26U &&
      (room_->Participants().size() < 6U || room_->Participants().size() > 8U))
    return MakeError(application::ErrorCode::kInvalidState,
                     "武器大师仅支持 6～8 名参赛玩家");

  if (rotating) {
    rotation = protocol::DecodeStartRotatingMatchRequest(body);
    force = rotation->force;
    start_response.map_key = rotation->map_keys.front();
    start_response.mode_key = rotation->mode_key;
  } else {
    normal = protocol::DecodeStartMatchRequest(body);
    force = normal->force;
    start_response.map_key = normal->map_key;
    start_response.mode_key = normal->mode_key;
  }
  if (room_->IsHideSeek()) {
    normal->map_key = timing_.hide_seek_scene->map_key;
    normal->mode_key = timing_.hide_seek_scene->map_key;
    start_response.map_key = normal->map_key;
    start_response.mode_key = normal->mode_key;
    scene_.configuration.map_key = normal->map_key;
    scene_.configuration.family_key = normal->mode_key;
    scene_.configuration.selected_duration_ms = 240'000U;
    scene_.configuration.field_24 = 240'000U;
  }
  const auto refuse_start = [&](std::string_view detail) {
    auto result = MakeResult(RoomEvent::kNoChange);
    result.rule_rejection_detail = std::string{detail};
    result.deliveries.push_back(
        {.recipient = actor,
         .message = {.opcode = 109U,
                     .body = protocol::EncodeNoticeResponse(
                         protocol::Utf8ToUtf16(detail))}});
    return result;
  };
  for (const auto& member : room_->Participants()) {
    const auto& snapshot = players_.at(member.player_id);
    const auto message = ItemRejection(snapshot);
    if (!message.empty())
      return refuse_start(snapshot.nickname + "：" + message);
  }
  if (room_->ModeId() == kHordeTutorMode)
    return refuse_start(
        "HordeTutor103请使用原107单人教程入口，不能通过普通房间开局");
  if (room_->ModeId() == kEventMoleMode)
    return refuse_start(EventMoleStartBlocker());
  if (room_->ModeId() == 21U &&
      (room_->Participants().size() < 4U || room_->Participants().size() > 12U))
    return refuse_start("BFRunner原跑道需要4至12名参赛者");
  if (IsEventMoleMap(start_response.map_key) ||
      IsEventMoleMap(start_response.mode_key) ||
      (rotation && std::ranges::any_of(rotation->map_keys, IsEventMoleMap)))
    return refuse_start(
        "EventMole 专用地图不能作为其他模式的普通 PVP 地图开局");
  if (room_->IsTraining()) {
    if (rotation || !normal)
      return MakeError(application::ErrorCode::kInvalidState,
                       "训练场不支持轮换地图");
    start_response.map_key = kTrainingMap;
    start_response.mode_key = kTrainingMap;
    normal->map_key = kTrainingMap;
    normal->mode_key = kTrainingMap;
    normal->display_name = kTrainingName;
  }

  if (!map_browse && (room_->ModeId() == 8U || room_->ModeId() == 28U)) {
    if (room_->Members().size() < 2U) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "僵尸模式至少需要两名玩家");
    }
    // B41510 resolves this field through MapMeta (47A040), except native Random
    // families. Zombie/ZombieGhost are mode labels, not map metadata keys:
    // using them here makes 47BBB0 dereference a null row before the map can
    // load. Keep the requested map key; SceneConfiguration::mode_id selects
    // infection.
  }
  if ((room_->ModeId() == 101U || room_->ModeId() == 102U)) {
    const auto difficulty =
        SelectPirateMap(scene_.configuration.family_key, 1U, room_->ModeId())
            .difficulty;
    const auto& selected =
        SelectPirateMap({.family_key = start_response.mode_key,
                         .map_key = start_response.map_key},
                        difficulty, room_->ModeId());
    start_response.map_key = selected.directory;
    start_response.mode_key = selected.key;
    if (rotation.has_value()) {
      for (std::size_t index = 0U; index < rotation->map_keys.size(); ++index) {
        const auto& map = SelectPirateMap(
            {.family_key = scene_.configuration.rotating_maps[index].family_key,
             .map_key = rotation->map_keys[index]},
            selected.difficulty, room_->ModeId());
        rotation->map_keys[index] = map.directory;
      }
      rotation->map_keys.front() = selected.directory;
      rotation->mode_key = selected.key;
      rotation->display_name = selected.display_name;
    } else if (normal.has_value()) {
      normal->map_key = selected.directory;
      normal->mode_key = selected.key;
      normal->display_name = selected.display_name;
    }
  }

  if (room_->ModeId() == 16U) {
    const auto members = room_->Members();
    if (std::ranges::any_of(
            members, [](const auto& member) { return member.seat >= 4U; }))
      return MakeError(
          application::ErrorCode::kInvalidState,
          "僵尸 Boss 的原生 NPC 使用后续席位号段，请使用前四个玩家席位");
    // 503/504 may still carry a map from the previous mode. Resolve against the
    // same PVE catalog as 402/513/514; never start an ordinary or pirate map.
    auto selection =
        protocol::SceneRotatingMap{.family_key = start_response.mode_key,
                                   .map_key = start_response.map_key};
    if (!FindZombiePveMap(selection.family_key) &&
        !(selection.family_key.empty() &&
          FindZombiePveMap(selection.map_key))) {
      selection = {.family_key = scene_.configuration.family_key,
                   .map_key = scene_.configuration.map_key};
    }
    const auto& selected = SelectZombiePveMap(selection);
    start_response.map_key = selected.key;
    start_response.mode_key = selected.key;
    if (rotation) {
      for (std::size_t index = 0U; index < rotation->map_keys.size(); ++index) {
        const auto& map = SelectZombiePveMap(
            {.family_key = scene_.configuration.rotating_maps[index].family_key,
             .map_key = rotation->map_keys[index]});
        rotation->map_keys[index] = map.key;
      }
      rotation->map_keys.front() = selected.key;
      rotation->mode_key = selected.key;
      rotation->display_name = selected.display_name;
    } else {
      normal->map_key = selected.key;
      normal->mode_key = selected.key;
      normal->display_name = selected.display_name;
    }
  }

  if (room_->ModeId() == 19U) {
    // Resolve cached and Random 503/504 selections against released bomb maps.
    auto selection =
        protocol::SceneRotatingMap{.family_key = start_response.mode_key,
                                   .map_key = start_response.map_key};
    if (!FindBombMap(selection.family_key) &&
        !(selection.family_key.empty() && FindBombMap(selection.map_key))) {
      selection = {.family_key = scene_.configuration.family_key,
                   .map_key = scene_.configuration.map_key};
    }
    const auto& selected = SelectBombMap(selection);
    start_response.map_key = selected.key;
    start_response.mode_key = selected.key;
    if (rotation) {
      for (std::size_t index = 0U; index < rotation->map_keys.size(); ++index) {
        const auto& map = SelectBombMap(
            {.family_key = scene_.configuration.rotating_maps[index].family_key,
             .map_key = rotation->map_keys[index]});
        rotation->map_keys[index] = map.key;
      }
      rotation->map_keys.front() = selected.key;
      rotation->mode_key = selected.key;
      rotation->display_name = selected.display_name;
    } else {
      normal->map_key = selected.key;
      normal->mode_key = selected.key;
      normal->display_name = selected.display_name;
    }
  }

  if (room_->ModeId() == 21U) {
    const auto* track = GetCrazyRacingMap(start_response.mode_key);
    if (!track) track = GetCrazyRacingMap(start_response.map_key);
    if (!track) track = GetCrazyRacingMap(scene_.configuration.map_key);
    if (!track)
      return MakeError(application::ErrorCode::kInvalidInput,
                       "BFRunner track is unknown");
    start_response.map_key = track->key;
    start_response.mode_key = track->key;
    if (rotation) {
      for (std::size_t index = 0; index < rotation->map_keys.size(); ++index) {
        auto& key = rotation->map_keys[index];
        const auto* rotation_track = GetCrazyRacingMap(key);
        if (!rotation_track)
          rotation_track = GetCrazyRacingMap(
              scene_.configuration.rotating_maps[index].family_key);
        if (!rotation_track) rotation_track = track;
        key = rotation_track->key;
      }
      rotation->mode_key = track->key;
    } else {
      normal->map_key = track->key;
      normal->mode_key = track->key;
    }
  }
  if (room_->ModeId() == 11U) {
    // 503/504 can contain cached ordinary or Random selections. Never load
    // those into soccer, which requires spawnNpc01 and Goalpost_T1/T2 from this
    // pitch.
    start_response.map_key = kSoccerMap;
    start_response.mode_key = kSoccerMap;
    if (rotation) {
      for (auto& map : rotation->map_keys) map = kSoccerMap;
      rotation->mode_key = kSoccerMap;
      rotation->display_name = kSoccerMapName;
    } else {
      normal->map_key = kSoccerMap;
      normal->mode_key = kSoccerMap;
      normal->display_name = kSoccerMapName;
    }
  }

  if (room_->UserHosted()) {
    const auto status = UserHostStatus(actor);
    if (status != protocol::UserHostStartStatus::kAccepted)
      return UserHostReply(actor, status);
    if (!user_host_authorization_ || !UserHostRosterMatches() ||
        now < user_host_authorization_->eligible_at ||
        now >= user_host_authorization_->expires_at)
      return UserHostReply(actor, protocol::UserHostStartStatus::kCannotStart);
  }
  auto selected_timing = timing_;
  selected_timing.match_duration = default_match_duration_;
  if (room_->IsTraining())
    selected_timing.countdown = std::chrono::milliseconds::zero();
  auto timed_configuration = scene_.configuration;
  if (timing_.use_room_duration) {
    // Resolve the selected/random map before choosing the clock. The untrusted
    // C2S503/504 rule array is not used as a duration source.
    timed_configuration.family_key = start_response.mode_key;
    timed_configuration.map_key = start_response.map_key;
    ApplyNativeMatchDuration(timed_configuration, host_duration_override_);
    const auto duration = protocol::ConfiguredMatchDurationMs(
        timed_configuration, ProtocolMilliseconds(default_match_duration_));
    if (duration > 3'600'000U) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "房间时长超过一小时上限");
    }
    selected_timing.match_duration = std::chrono::milliseconds{duration};
  }
  if (room_->IsHideSeek()) {
    // The native input controller stops when the S2C704 clock expires.
    // Keep that clock identical to the independent mode state machine.
    const HideSeekRules rules;
    selected_timing.match_duration =
        std::chrono::milliseconds{rules.hiding_ms + rules.hunting_ms};
  }
  const bool random_teams =
      (scene_.configuration.field_8 & protocol::kSceneRandomTeams) != 0U;
  auto changed =
      room_->StartLoading(actor, force, now, selected_timing, random_teams);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    if (room_->UserHosted())
      return UserHostReply(actor, protocol::UserHostStartStatus::kCannotStart);
    return *error;
  }
  if (room_->UserHosted()) {
    frozen_user_host_prize_ = user_host_authorization_->terms;
    user_host_authorization_.reset();
  }
  timing_ = selected_timing;
  scene_.configuration.selected_duration_ms =
      timed_configuration.selected_duration_ms;
  scene_.configuration.field_24 = timed_configuration.field_24;
  scene_.configuration.field_28 = timed_configuration.field_28;
  scene_.configuration.field_32 = timed_configuration.field_32;
  training_.reset();
  pirate_.reset();
  infection_.reset();
  weapon_master_.reset();
  racing_.reset();
  crazy_racing_.reset();
  captain_.reset();
  zombie_pve_.reset();
  if (!map_browse && room_->ModeId() == 16U)
    zombie_pve_ = std::make_unique<ZombiePveMatch>(start_response.map_key);
  map_agents_ = {};
  if (!map_browse && room_->ModeId() == 101U)
    map_agents_.Configure(
        101U, start_response.map_key,
        SelectPirateMap(start_response.mode_key, 1U).difficulty);
  loading_map_agents_.clear();
  npc_prestart_.clear();
  npc_prestart_bytes_ = 0U;
  native_prestart_gaps_.clear();
  pending_pirate_wave_start_.reset();
  objectives_.reset();
  pending_soccer_ball_.reset();
  map_assets_.clear();
  latency_probes_.clear();
  combat_.reset();
  pending_network_movements_.clear();
  active_match_.reset();
  scene_.kind = 3U;
  if (room_->ModeId() == 19U) {
    // B41510 仅在此位开启时写入回合索引；7AFA10 在决胜回合读取 field_0
    // 的进攻队伍。
    scene_.configuration.field_8 |= 1U;
    scene_.configuration.field_280 = 0U;
    start_response.field_0 = 1;
  }
  if (rotation.has_value()) {
    rotating_match_ = rotation;
    normal_match_.reset();
    scene_.configuration.family_key = rotation->mode_key;
    scene_.configuration.map_key = rotation->map_keys.front();
    scene_.configuration.display_name = rotation->display_name;
    for (std::size_t index = 0U; index < rotation->map_keys.size(); ++index) {
      auto& map = scene_.configuration.rotating_maps[index];
      map.map_key = rotation->map_keys[index];
      if (room_->ModeId() == 16U || room_->ModeId() == 19U ||
          room_->ModeId() == 21U) {
        map.family_key = map.map_key;
      }
      if ((room_->ModeId() == 101U || room_->ModeId() == 102U)) {
        const auto difficulty =
            SelectPirateMap(rotation->mode_key, 1U, room_->ModeId()).difficulty;
        map.family_key = SelectPirateMap(map, difficulty, room_->ModeId()).key;
        if (index == 0U) {
          map.family_key = rotation->mode_key;
        }
      }
    }
  } else {
    normal_match_ = normal;
    rotating_match_.reset();
    scene_.configuration.family_key = normal->mode_key;
    scene_.configuration.map_key = normal->map_key;
    scene_.configuration.display_name = normal->display_name;
  }

  std::vector<OutboundMessage> messages;
  if (room_->IsHideSeek() || random_teams ||
      (room_->ModeId() == 101U || room_->ModeId() == 102U) ||
      room_->ModeId() == 16U || room_->ModeId() == 8U ||
      room_->ModeId() == 28U || room_->ModeId() == 13U ||
      room_->ModeId() == 23U) {
    for (const auto& member : room_->Members()) {
      if (member.IsObserver()) continue;
      messages.push_back({.opcode = 504U,
                          .body = protocol::EncodeRoomTeamChangedResponse(
                              {.slot = member.seat, .team = member.team})});
    }
  }
  messages.push_back({.opcode = 505U,
                      .body = protocol::EncodeRoomConfigurationResponse(
                          scene_.configuration)});
  if (zombie_pve_) messages.push_back(zombie_pve_->RescueTasks());
  messages.push_back(
      {.opcode = 510U,
       .body = protocol::EncodeStartSceneResponse(start_response)});

  auto result = MakeResult(std::get<RoomEvent>(changed));
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleLoading(
    domain::PlayerId actor, std::uint16_t opcode,
    std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now) {
  if (room_->IsHideSeek() && room_->Phase() == RoomPhase::kPlaying &&
      hide_seek_ && hide_seek_observer_loading_.contains(actor)) {
    auto& flags = hide_seek_observer_loading_.at(actor);
    if (opcode == 709U) {
      const auto r = protocol::DecodePlayerObjectIdentityRequest(body);
      const auto recorded = room_->RecordObjectId(actor, r.object_id);
      if (const auto* error = std::get_if<application::ServiceError>(&recorded))
        return *error;
    } else if (opcode == 702U) {
      static_cast<void>(protocol::DecodeMapAssetsRequest(body));
      flags |= 1U;
    } else {
      static_cast<void>(protocol::DecodeLoadingCompleteRequest(body));
      flags |= 2U;
    }
    auto result = MakeResult(RoomEvent::kNoChange);
    if (flags == 3U) {
      flags |= 4U;
      for (const auto& m : room_->Participants())
        result.deliveries.push_back(
            {actor, {701U, protocol::EncodeActivateSlotResponse(m.seat)}});
      result.deliveries.push_back(
          {actor, {702U, protocol::EncodeLoadingOptionsResponse({})}});
      result.deliveries.push_back(
          {actor, {703U, protocol::EncodeCountdownResponse(0U)}});
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch())
                          .count();
      const auto state = hide_seek_->Match().Snapshot(
          hide_seek_->Match().Appearances().front().player);
      result.deliveries.push_back(
          {actor,
           {704U, protocol::EncodeMatchBeginResponse(
                      {.duration_ms = state.remaining_ms,
                       .scene_id = scene_.scene_id})}});
      AppendHideSeek(result, hide_seek_->Observe(actor.Value(), ms));
      // A late observer missed earlier 32810 events. Reconstruct only the
      // live hunters' temporary bars and selection for this recipient.
      if (combat_)
        for (const auto& visible : hide_seek_->Match().Appearances()) {
          const auto found =
              combat_->actors_.find(domain::PlayerId{visible.player});
          if (visible.role != HideSeekRole::kHunter ||
              found == combat_->actors_.end())
            continue;
          const auto selected =
              found->second.selected_weapon_bar == 2U ? 2U : 0U;
          for (const auto bar : {0U, 2U}) {
            protocol::PacketWriter weapon;
            weapon.WriteU32(bar == 0U ? 3001U : 7001U);
            weapon.WriteU32(bar);
            weapon.WriteU32(visible.actor);
            weapon.WriteU32(0U);
            weapon.WriteU32(bar == selected ? 1U : 0U);
            result.deliveries.push_back({actor, {32810U, weapon.Release()}});
          }
        }
    }
    return result;
  }
  if (room_->Phase() != RoomPhase::kLoading &&
      room_->Phase() != RoomPhase::kCountdown &&
      room_->Phase() != RoomPhase::kPlaying) {
    if (opcode == 701U) {
      static_cast<void>(protocol::DecodeLoadingCompleteRequest(body));
    } else if (opcode == 702U) {
      static_cast<void>(protocol::DecodeMapAssetsRequest(body));
    } else {
      static_cast<void>(protocol::DecodePlayerObjectIdentityRequest(body));
    }
    return MakeResult(RoomEvent::kNoChange);
  }
  if (opcode == 709U) {
    const auto request = protocol::DecodePlayerObjectIdentityRequest(body);
    if (room_->ModeId() == 11U && actor == room_->HostPlayerId()) {
      const auto host = room_->GetMember(actor);
      const auto host_object =
          1'000'000U + static_cast<std::uint32_t>(host->seat) * 100'000U;
      const auto ball = request.object_id;
      if (MatchObjectives::SoccerBallObject(host_object, ball)) {
        if ((pending_soccer_ball_ && *pending_soccer_ball_ != ball) ||
            (combat_ && objectives_ &&
             !objectives_->RegisterSoccerBall(actor, ball, *combat_)))
          return MakeError(
              application::ErrorCode::kInvalidInput,
              "soccer ball registration conflicts with the active object");
        pending_soccer_ball_ = ball;
        return MakeResult(RoomEvent::kNoChange);
      }
    }
    auto changed = room_->RecordObjectId(actor, request.object_id);
    if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
      return MakeError(error->code, error->safe_message + " object_id=" +
                                        std::to_string(request.object_id));
    }
    return MakeResult(std::get<RoomEvent>(changed));
  }

  if (opcode == 702U) {
    auto request = protocol::DecodeMapAssetsRequest(body);
    if (room_->Phase() == RoomPhase::kCountdown ||
        room_->Phase() == RoomPhase::kPlaying) {
      const bool recovered = actor == room_->HostPlayerId() && objectives_ &&
                             objectives_->RecoverInfectionSupplies(request);
      map_assets_.insert_or_assign(actor, std::move(request));
      if (recovered) return AdvanceObjectives(now);
      return MakeResult(RoomEvent::kNoChange);
    }
    if (room_->Phase() != RoomPhase::kLoading) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "当前房间阶段不接受地图资产报告");
    }
    map_assets_.insert_or_assign(actor, std::move(request));
    auto changed = room_->RecordAssetsReceived(actor, now);
    if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
      return *error;
    }
    return CompleteLoadingEvent(std::get<RoomEvent>(changed), now);
  }

  static_cast<void>(protocol::DecodeLoadingCompleteRequest(body));
  if (room_->Phase() == RoomPhase::kCountdown ||
      room_->Phase() == RoomPhase::kPlaying) {
    return MakeResult(RoomEvent::kNoChange);
  }
  if (room_->Phase() != RoomPhase::kLoading) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前房间阶段不接受加载完成报告");
  }
  auto changed = room_->RecordLoadCompleted(actor, now);
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    return *error;
  }
  return CompleteLoadingEvent(std::get<RoomEvent>(changed), now);
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleMatchLatencyProbe(
    domain::PlayerId actor, std::span<const std::uint8_t> body) const {
  const auto probe = protocol::DecodeMatchLatencyProbe(body);
  const auto member = room_->GetMember(actor);
  // 不重新发布计时器：开局持久化尚未确认时，内部 Playing 计时必须留在原编排内。
  RoomCommandResult result;
  // 与 Python
  // 对照一致：迟到、阶段外或冒用其他席位的探测不回显，也不改变房间计时。
  if (!member.has_value() || probe.slot != member->seat ||
      (room_->Phase() != RoomPhase::kLoading &&
       room_->Phase() != RoomPhase::kCountdown &&
       room_->Phase() != RoomPhase::kPlaying)) {
    return result;
  }
  result.deliveries.push_back(
      {.recipient = actor,
       .message = {.opcode = 32840U,
                   .body = protocol::EncodeMatchLatencyProbe(probe)}});
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleLatencyProbe(domain::PlayerId actor,
                                    std::span<const std::uint8_t> body,
                                    std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeRoomLatencyProbeRequest(body);
  const auto member = room_->GetMember(actor);
  if (!member.has_value() || request.slot != member->seat) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "延迟探测席位不是当前玩家");
  }

  auto [probe, unused_inserted] = latency_probes_.try_emplace(request.slot);
  static_cast<void>(unused_inserted);
  probe->second.started_at = now;
  probe->second.pending_players.clear();
  for (const auto& current : room_->Members()) {
    probe->second.pending_players.insert(current.player_id);
  }

  auto result = MakeResult(RoomEvent::kNoChange);
  const std::array messages{OutboundMessage{
      .opcode = 517U,
      .body = protocol::EncodeRoomLatencyProbeResponse(request.slot)}};
  Broadcast(result, messages);
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleLatencyReport(
    domain::PlayerId actor, std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now) {
  const auto request = protocol::DecodeRoomLatencyReportRequest(body);
  const auto reporter = room_->GetMember(actor);
  if (!reporter.has_value() || request.slot != reporter->seat) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "延迟报告席位不是当前玩家");
  }

  auto result = MakeResult(RoomEvent::kNoChange);
  auto probe = latency_probes_.find(request.token);
  constexpr auto probe_lifetime = std::chrono::seconds{5};
  if (probe == latency_probes_.end()) {
    return result;
  }
  if (now < probe->second.started_at ||
      now - probe->second.started_at > probe_lifetime) {
    latency_probes_.erase(probe);
    return result;
  }
  if (!probe->second.pending_players.contains(actor) ||
      request.token > std::numeric_limits<std::uint16_t>::max()) {
    return result;
  }
  const auto members = room_->Members();
  const auto target = MemberBySeat(members, request.token);
  if (!target.has_value()) {
    return result;
  }

  probe->second.pending_players.erase(actor);
  result.deliveries.push_back(
      {.recipient = target->player_id,
       .message = {.opcode = 518U,
                   .body = protocol::EncodeRoomLatencyReportResponse(
                       {.slot = request.slot,
                        .fps = request.fps,
                        .stage = request.stage})}});
  if (probe->second.pending_players.empty()) {
    latency_probes_.erase(probe);
  }
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::InitialMatchEffects(
    std::chrono::steady_clock::time_point now) {
  if (hide_seek_) return AdvanceHideSeek(now);
  if (scene_.configuration.submode_id == 4U)
    return MakeResult(RoomEvent::kNoChange);
  auto prestart = MakeResult(RoomEvent::kNoChange);
  if ((zombie_pve_ || pirate_) && combat_ &&
      room_->Phase() == RoomPhase::kPlaying) {
    auto pending = std::move(npc_prestart_);
    npc_prestart_.clear();
    npc_prestart_bytes_ = 0U;
    for (const auto& [sender, packet] : pending) {
      if (sender != room_->HostPlayerId()) continue;
      auto replayed = Handle(sender, packet, now);
      if (const auto* error = std::get_if<application::ServiceError>(&replayed))
        return *error;
      auto current = std::get<RoomCommandResult>(std::move(replayed));
      current.deliveries.insert(
          current.deliveries.begin(),
          std::make_move_iterator(prestart.deliveries.begin()),
          std::make_move_iterator(prestart.deliveries.end()));
      if (!current.rule_rejection_detail)
        current.rule_rejection_detail =
            std::move(prestart.rule_rejection_detail);
      if (!current.combat_reject_reason)
        current.combat_reject_reason = prestart.combat_reject_reason;
      prestart = std::move(current);
      if (room_->Phase() != RoomPhase::kPlaying) return prestart;
    }
  }
  if (pirate_ && combat_ && room_->Phase() == RoomPhase::kPlaying &&
      pending_pirate_wave_start_) {
    const auto request = std::move(*pending_pirate_wave_start_);
    pending_pirate_wave_start_.reset();
    auto started = HandleCombat(room_->HostPlayerId(), request, now);
    if (auto* value = std::get_if<RoomCommandResult>(&started)) {
      value->deliveries.insert(
          value->deliveries.begin(),
          std::make_move_iterator(prestart.deliveries.begin()),
          std::make_move_iterator(prestart.deliveries.end()));
    }
    return started;
  }
  if (racing_ && combat_ && room_->Phase() == RoomPhase::kPlaying) {
    auto result = MakeResult(RoomEvent::kNoChange);
    try {
      const auto started = racing_->Start(*combat_, now);
      Broadcast(result, started.messages);
    } catch (const std::invalid_argument&) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "极限竞速需要 2～16 名真实参赛者");
    }
    PreserveCombatPerformance();
    room_->SetObjectiveDeadline(racing_->Deadline());
    room_->SuspendMatchTimeout();
    result.active_timers = room_->ActiveTimers();
    return result;
  }
  if (weapon_master_ && combat_ && room_->Phase() == RoomPhase::kPlaying) {
    const auto started = weapon_master_->Start(*combat_);
    PreserveCombatPerformance();
    auto result = MakeResult(RoomEvent::kNoChange);
    Broadcast(result, started.messages);
    if (started.score_changed) AppendScore(result, 255U);
    return result;
  }
  auto result = AdvanceObjectives(now);
  if (auto* value = std::get_if<RoomCommandResult>(&result);
      value != nullptr && (room_->ModeId() == 5U || room_->ModeId() == 6U ||
                           room_->ModeId() == 7U || room_->ModeId() == 16U)) {
    // Native SingleScoreRankBoard only draws rows whose ScoreData status is 1.
    // PVE NPC activity need not change player combat stats, so seed the roster
    // after durable start instead of waiting for the first human casualty.
    AppendScore(*value, 255U);
  }
  if (auto* value = std::get_if<RoomCommandResult>(&result)) {
    value->deliveries.insert(
        value->deliveries.begin(),
        std::make_move_iterator(prestart.deliveries.begin()),
        std::make_move_iterator(prestart.deliveries.end()));
    if (!value->rule_rejection_detail)
      value->rule_rejection_detail = std::move(prestart.rule_rejection_detail);
    if (!value->combat_reject_reason)
      value->combat_reject_reason = prestart.combat_reject_reason;
  }
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::AdvanceObjectives(std::chrono::steady_clock::time_point now) {
  if (hide_seek_) return AdvanceHideSeek(now);
  if (!objectives_ || !combat_ || room_->Phase() != RoomPhase::kPlaying) {
    return MakeResult(RoomEvent::kNoChange);
  }
  auto update = objectives_->Tick(*combat_, now, round_deadline_);
  if (crazy_racing_ && crazy_racing_->Finished(now)) update.finished = true;
  auto retired_agents = map_agents_.Tick(*combat_, now);
  update.messages.insert(update.messages.end(),
                         std::make_move_iterator(retired_agents.begin()),
                         std::make_move_iterator(retired_agents.end()));
  if (zombie_pve_) {
    auto messages = zombie_pve_->Tick(*combat_, now);
    update.messages.insert(update.messages.end(), messages.begin(),
                           messages.end());
    update.finished = zombie_pve_->Winner() != 0U;
  }
  if (infection_) {
    auto messages = infection_->Tick(*combat_, now);
    update.messages.insert(update.messages.end(),
                           std::make_move_iterator(messages.begin()),
                           std::make_move_iterator(messages.end()));
    update.finished = infection_->Winner() != 0U;
  }
  if (weapon_master_)
    update.finished = weapon_master_->Tick(*combat_, now).finished;
  std::vector<CombatDelivery> captain_deliveries;
  if (captain_) {
    // No authenticated damage-amount producer exists here: 32933 is
    // display-only HP and must not mint 10/14 credits. Consume elapsed time
    // before tick clears pending/active state, so an expired skill cannot bank
    // that interval.
    for (std::uint8_t team = 1U; team <= 2U; ++team) {
      const auto player = captain_->Selected(team);
      const auto progress =
          player ? captain_->GetFeverProgress(*player) : std::nullopt;
      if (!progress) {
        continue;
      }
      const auto life = combat_->LifeGeneration(*player);
      if (!life) {
        // A departure removes the combat actor before CaptainAssignments ticks.
        captain_fever_clock_.erase(*player);
        continue;
      }
      auto& clock = captain_fever_clock_[*player];
      if (clock.assignment != progress->assignment || clock.life != *life) {
        clock = {.assignment = progress->assignment,
                 .life = *life,
                 .last = now,
                 .serial = clock.serial};
        continue;
      }
      const auto steps = (now - clock.last) / std::chrono::milliseconds{100};
      if (steps <= 0) continue;
      clock.last += std::chrono::milliseconds{100} * steps;
      // Consume wall time even while ready/pending/active, never bank it for
      // the next skill cycle. A bubbled/nonalive role cannot receive time
      // credits.
      if (progress->ready || progress->pending || progress->active ||
          combat_->LifeState(*player) != CombatLifeState::kAlive)
        continue;
      static_cast<void>(
          captain_->CreditFever(*player,
                                {.assignment = progress->assignment,
                                 .serial = ++clock.serial,
                                 .amount = static_cast<std::uint32_t>(
                                     std::min<std::int64_t>(steps, 1400) * 50),
                                 .life = progress->life},
                                *combat_, now));
    }
    auto captained = captain_->Tick(*combat_, now);
    update.messages.insert(
        update.messages.end(),
        std::make_move_iterator(captained.broadcasts.begin()),
        std::make_move_iterator(captained.broadcasts.end()));
    captain_deliveries = std::move(captained.deliveries);
  }
  if (racing_) {
    // Timer-only handoff must bind new770 to the room's current host too.
    combat_->SetHost(room_->HostPlayerId());
    auto raced = racing_->Tick(now);
    update.messages.insert(update.messages.end(),
                           std::make_move_iterator(raced.messages.begin()),
                           std::make_move_iterator(raced.messages.end()));
    for (auto& message : raced.host_messages)
      captain_deliveries.push_back(
          {.recipient = room_->HostPlayerId(), .message = std::move(message)});
    update.finished = update.finished ||
                      raced.completion == ExtremeRacingCompletion::kFiveStages;
  }
  if (pirate_) {
    if (auto cleared = pirate_->Advance(*combat_, now)) {
      update.messages.push_back(std::move(*cleared));
    }
  }
  PreserveCombatPerformance();
  if (update.finished) {
    auto finished =
        FinishMatch(application::MatchCompletionKind::kCompleted, now);
    if (auto* value = std::get_if<RoomCommandResult>(&finished)) {
      RoomCommandResult effects;
      Broadcast(effects, update.messages);
      value->deliveries.insert(
          value->deliveries.begin(),
          std::make_move_iterator(effects.deliveries.begin()),
          std::make_move_iterator(effects.deliveries.end()));
    }
    return finished;
  }
  if (update.restart_round) {
    auto restarted = room_->RestartRound(now);
    if (const auto* error =
            std::get_if<application::ServiceError>(&restarted)) {
      return *error;
    }
    combat_->ResetRound();
    map_assets_.clear();
    scene_.kind = 3U;
    auto result = MakeResult(RoomEvent::kRoundLoadingStarted);
    const std::array messages{
        OutboundMessage{.opcode = 510U,
                        .body = protocol::EncodeStartSceneResponse(
                            {.map_key = scene_.configuration.map_key,
                             .mode_key = scene_.configuration.family_key,
                             .round_index = objectives_->RoundIndex(),
                             .field_0 = static_cast<std::int8_t>(
                                 1U + objectives_->RoundIndex() % 2U)})}};
    Broadcast(result, messages);
    return result;
  }
  auto respawn_deliveries = combat_->TickRespawns(now);
  auto deadline = objectives_->Deadline();
  if (crazy_racing_ && crazy_racing_->Deadline()) {
    if (!deadline || *crazy_racing_->Deadline() < *deadline)
      deadline = crazy_racing_->Deadline();
    // Confirmed goal wins over a later ordinary timeout. The2s window adapts
    // the native delayed event32; it is not an original server stop capture.
    room_->SuspendMatchTimeout();
  }
  if (captain_) {
    // The native time increment is measured in 100ms units. Publish a real
    // room timer so silent captains charge and expired skills retire without
    // needing another network request. This is our scheduling adapter; it
    // preserves the native 321-then-generic double addition/readiness order.
    const auto captain_due = now + std::chrono::milliseconds{100};
    if (!deadline || captain_due < *deadline) deadline = captain_due;
  }
  if (const auto respawn_due = combat_->RespawnDeadline();
      respawn_due && (!deadline || *respawn_due < *deadline))
    deadline = respawn_due;
  if (weapon_master_ && weapon_master_->Deadline()) {
    deadline = weapon_master_->Deadline();
    room_->SuspendMatchTimeout();
  }
  if (racing_) {
    // Five stages outlast the room clock; the racing core owns every deadline.
    const auto racing_due = racing_->Deadline();
    if (racing_due && (!deadline || *racing_due < *deadline))
      deadline = racing_due;
    room_->SuspendMatchTimeout();
  }
  const auto guardian_due = map_agents_.Deadline();
  if (guardian_due && (!deadline || *guardian_due < *deadline))
    deadline = guardian_due;
  if (zombie_pve_) {
    const auto drop_due = zombie_pve_->Deadline();
    if (drop_due && (!deadline || *drop_due < *deadline)) deadline = drop_due;
  }
  if (infection_) {
    const auto respawn_due = infection_->Deadline();
    if (respawn_due && (!deadline || *respawn_due < *deadline))
      deadline = respawn_due;
  }
  if (pirate_) {
    const auto wave_deadline = pirate_->Deadline(*combat_);
    if (wave_deadline.has_value() &&
        (!deadline.has_value() || *wave_deadline < *deadline)) {
      deadline = wave_deadline;
    }
  }
  room_->SetObjectiveDeadline(deadline);
  if (objectives_->BetweenRounds() ||
      (objectives_->FuseActive() && now >= round_deadline_)) {
    room_->SuspendMatchTimeout();
  }
  auto result = MakeResult(RoomEvent::kNoChange);
  for (auto& delivery : respawn_deliveries)
    result.deliveries.push_back({.recipient = delivery.recipient,
                                 .message = std::move(delivery.message)});
  for (auto& delivery : captain_deliveries)
    result.deliveries.push_back({.recipient = delivery.recipient,
                                 .message = std::move(delivery.message)});
  for (const auto& message : update.messages) {
    if (message.opcode != 705U) {
      Broadcast(result, std::span{&message, 1U});
    }
  }
  if (update.score_changed) {
    AppendScore(result, 255U);
  }
  for (const auto& message : update.messages) {
    if (message.opcode == 705U) {
      Broadcast(result, std::span{&message, 1U});
    }
  }
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleObjective(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  MatchObjectives::Validate(packet);
  if (room_->Phase() != RoomPhase::kPlaying || !objectives_ || !combat_) {
    return MakeResult(RoomEvent::kNoChange);
  }
  auto prior = AdvanceObjectives(now);
  if (const auto* error = std::get_if<application::ServiceError>(&prior)) {
    return *error;
  }
  auto result = std::get<RoomCommandResult>(std::move(prior));
  if (room_->Phase() != RoomPhase::kPlaying ||
      result.settlement_command.has_value() || objectives_->BetweenRounds()) {
    return result;
  }
  auto update = objectives_->Handle(actor, packet, *combat_, now);
  for (auto& delivery : update.deliveries)
    result.deliveries.push_back(
        {delivery.recipient, std::move(delivery.message)});
  CombatRelayResult relayed{.accepted = !update.messages.empty()};
  for (const auto& member : room_->Members()) {
    if (update.exclude_recipient == member.player_id) {
      continue;
    }
    for (const auto& message : update.messages) {
      relayed.deliveries.push_back(
          {.recipient = member.player_id, .message = message});
    }
  }
  combat_->RecordItemEffectConstructors(relayed, actor, packet);
  for (auto& delivery : relayed.deliveries)
    result.deliveries.push_back({.recipient = delivery.recipient,
                                 .message = std::move(delivery.message)});
  PreserveCombatPerformance();
  auto advanced = AdvanceObjectives(now);
  if (const auto* error = std::get_if<application::ServiceError>(&advanced)) {
    return *error;
  }
  auto current = std::get<RoomCommandResult>(std::move(advanced));
  current.rule_rejection_detail = std::move(update.rejection_detail);
  current.deliveries.insert(current.deliveries.begin(),
                            std::make_move_iterator(result.deliveries.begin()),
                            std::make_move_iterator(result.deliveries.end()));
  if (update.score_changed && !current.settlement_command.has_value()) {
    AppendScore(current, 255U);
  }
  return current;
}

std::string RoomCommandFlow::ItemRejection(
    const application::PlayerMenuSnapshot& snapshot) const {
  if (room_->RestrictedItems().empty()) return {};
  constexpr std::array<std::string_view, 4> slots{"primary", "secondary",
                                                  "melee", "grenade"};
  std::array<bool, 4> present{};
  std::vector<std::uint32_t> ids;
  for (const auto& item : snapshot.items) {
    if (!item.equipped) continue;
    if (item.item_id.Value() <= std::numeric_limits<std::uint32_t>::max())
      ids.push_back(static_cast<std::uint32_t>(item.item_id.Value()));
    const auto slot = std::ranges::find(slots, item.slot_code);
    if (slot != slots.end())
      present[static_cast<std::size_t>(slot - slots.begin())] = true;
  }
  domain::AppendDefaultRoomWeapons(ids, present);
  return domain::RestrictedEquipmentNotice(room_->RestrictedItems(), ids);
}

void RoomCommandFlow::AppendItemState(RoomCommandResult& result) const {
  for (const auto& [player, opcode] : item_setting_clients_) {
    if (room_->GetMember(player))
      result.deliveries.push_back(
          {player,
           {opcode, protocol::EncodeRoomItemState(
                        static_cast<std::uint32_t>(room_->RoomId().Value()),
                        room_->Generation(), room_->ItemRulesRevision(),
                        room_->Phase() == RoomPhase::kWaiting &&
                            room_->HostPlayerId() == player,
                        0U, room_->RestrictedItems())}});
  }
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleItemSetting(domain::PlayerId actor,
                                   std::span<const std::uint8_t> body,
                                   std::uint16_t response_opcode) {
  const auto request = protocol::DecodeRoomItemRequest(body);
  if (!room_->GetMember(actor) || room_->Phase() == RoomPhase::kClosed)
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "玩家不在当前房间");
  auto result = MakeResult(RoomEvent::kNoChange);
  if (request.room != room_->RoomId().Value()) return result;
  item_setting_clients_.insert_or_assign(actor, response_opcode);
  std::uint32_t status = 0U;
  std::string notice;
  if (request.action) {
    if (request.generation != room_->Generation() ||
        request.revision != room_->ItemRulesRevision()) {
      status = 2U;
      notice = "房间设置已变化，请确认最新禁用列表后重试";
    } else {
      auto changed = room_->SetRestrictedItems(actor, request.items);
      if (const auto* error =
              std::get_if<application::ServiceError>(&changed)) {
        status = 1U;
        notice = error->safe_message;
      } else {
        result.event = std::get<RoomEvent>(changed);
        if (result.event == RoomEvent::kConfigurationChanged) {
          CancelUserHostAuthorization(result);
          for (const auto& member : room_->Members()) {
            const std::array messages{MemberUpdate(member.player_id)};
            Broadcast(result, messages);
            result.deliveries.push_back(
                {member.player_id,
                 {109U, protocol::EncodeNoticeResponse(
                            u"房主已修改禁用物品，请检查配装后重新准备")}});
          }
        }
      }
    }
  }
  if (!notice.empty())
    result.deliveries.push_back({actor,
                                 {109U, protocol::EncodeNoticeResponse(
                                            protocol::Utf8ToUtf16(notice))}});
  AppendItemState(result);
  for (auto& delivery : result.deliveries) {
    if (delivery.recipient == actor &&
        delivery.message.opcode == response_opcode)
      delivery.message.body = protocol::EncodeRoomItemState(
          static_cast<std::uint32_t>(room_->RoomId().Value()),
          room_->Generation(), room_->ItemRulesRevision(),
          room_->Phase() == RoomPhase::kWaiting &&
              room_->HostPlayerId() == actor,
          status, room_->RestrictedItems());
  }
  if (!request.action)
    std::erase_if(result.deliveries,
                  [&](const auto& d) { return d.recipient != actor; });
  return result;
}

void RoomCommandFlow::AppendNetworkState(RoomCommandResult& result) const {
  AppendItemState(result);
  for (const auto player : network_setting_clients_) {
    if (!room_->GetMember(player)) continue;
    const auto selected = room_->NetworkMode();
    const auto version = room_setting_versions_.contains(player)
                             ? room_setting_versions_.at(player)
                             : 1U;
    result.deliveries.push_back(
        {player,
         {protocol::kRoomNetworkResponse,
          protocol::EncodeRoomNetworkState(
              {.room = static_cast<std::uint32_t>(room_->RoomId().Value()),
               .generation = room_->Generation(),
               .selected = selected,
               .actual = protocol::RoomNetworkMode::kServerDirect,
               .editable = room_->Phase() == RoomPhase::kWaiting &&
                           room_->HostPlayerId() == player,
               .p2p_available = true,
               .reason = selected == protocol::RoomNetworkMode::kP2pPreferred
                             ? 2U
                             : 0U,
               .global_rules = room_->GlobalRules()},
              version)}});
  }
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleNetworkSetting(domain::PlayerId actor,
                                      std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeRoomNetworkRequest(body);
  if (!room_->GetMember(actor))
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "玩家不在当前房间");
  network_setting_clients_.insert(actor);
  if (request.version == 2U) room_setting_versions_[actor] = 2U;
  auto result = MakeResult(RoomEvent::kNoChange);
  if (request.action) {
    if (request.room != room_->RoomId().Value() ||
        request.generation != room_->Generation()) {
      result.deliveries.push_back(
          {actor,
           {109U, protocol::EncodeNoticeResponse(
                      u"房间状态已变化，请重新打开房间设置")}});
    } else {
      auto changed = room_->SetRoomSettings(
          actor, request.mode,
          request.version == 2U ? request.global_rules : room_->GlobalRules());
      if (const auto* error = std::get_if<application::ServiceError>(&changed))
        result.deliveries.push_back(
            {actor,
             {109U, protocol::EncodeNoticeResponse(
                        protocol::Utf8ToUtf16(error->safe_message))}});
      else {
        result.event = std::get<RoomEvent>(changed);
        if (result.event == RoomEvent::kConfigurationChanged) {
          network_keys_.clear();
          network_epoch_ = 0;
          network_generation_ = 0;
          for (const auto& member : room_->Members())
            result.deliveries.push_back(
                {member.player_id, MemberUpdate(member.player_id)});
        }
      }
    }
  }
  AppendNetworkState(result);
  AppendNetworkPeers(result);
  if (!request.action)
    std::erase_if(result.deliveries,
                  [&](const auto& item) { return item.recipient != actor; });
  return result;
}

RoomCommandResult RoomCommandFlow::StageNpcPrestart(
    domain::PlayerId actor, const protocol::Packet& packet) {
  auto result = MakeResult(RoomEvent::kNoChange);
  // Local defensive limits only; every retained frame is authenticated again
  // after install. Never coalesce a destination across removal/slot reuse.
  constexpr std::size_t maximum_packets = 1024U;
  constexpr std::size_t maximum_bytes = 256U * 1024U;
  if (packet.body.size() > 4096U || npc_prestart_.size() >= maximum_packets ||
      packet.body.size() > maximum_bytes - npc_prestart_bytes_) {
    result.rule_rejection_detail =
        "NPC prestart buffer full opcode=" + std::to_string(packet.opcode);
    return result;
  }
  npc_prestart_.emplace_back(actor, packet);
  npc_prestart_bytes_ += packet.body.size();
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleCombat(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (packet.opcode == 903U &&
      (room_->ModeId() == 101U || room_->ModeId() == 102U) && !combat_ &&
      (room_->Phase() == RoomPhase::kLoading ||
       room_->Phase() == RoomPhase::kCountdown ||
       room_->Phase() == RoomPhase::kPlaying)) {
    PirateMatch::Validate(packet);
    protocol::PacketReader reader{packet.body};
    // 原版只请求一次并等待 904；不能把开局阶段的首波握手作为普通动作丢弃。
    if (actor == room_->HostPlayerId() && reader.ReadU32() == 1U) {
      pending_pirate_wave_start_ = packet;
    }
    return MakeResult(RoomEvent::kNoChange);
  }
  if (zombie_pve_ && !PirateMatch::Accepts(packet.opcode) &&
      (room_->Phase() == RoomPhase::kLoading ||
       room_->Phase() == RoomPhase::kCountdown ||
       (room_->Phase() == RoomPhase::kPlaying && !combat_))) {
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    const auto host = room_->GetMember(room_->HostPlayerId());
    if (actor == room_->HostPlayerId() && host && host->object_id &&
        ZombiePveMatch::PrestartNpcAction(packet, host->seat))
      return StageNpcPrestart(actor, packet);
    auto ignored = MakeResult(RoomEvent::kNoChange);
    ignored.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return ignored;
  }
  if (room_->Phase() != RoomPhase::kPlaying ||
      (objectives_ && objectives_->BetweenRounds())) {
    // 原版客户端可能在倒计时结束前初始化武器，也可能在结算/回房切换后留下一个已发送
    // 的动作包。严格校验字段后丢弃这些竞态消息，既不污染战斗账本，也不把正常切场景
    // 竞态升级为致命 Session 错误。只有 Playing 阶段才允许对象认证和同行转发。
    if (PirateMatch::Accepts(packet.opcode)) {
      PirateMatch::Validate(packet);
    } else {
      protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    }
    auto ignored = MakeResult(RoomEvent::kNoChange);
    ignored.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return ignored;
  }
  if (!combat_) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "本局战斗参与者快照尚未安装");
  }

  combat_->SetHost(room_->HostPlayerId());
  if (hide_seek_ && (protocol::IsNativeActionOpcode(packet.opcode) ||
                     protocol::IsNativeHitOpcode(packet.opcode) ||
                     protocol::IsNativeLifecycleOpcode(packet.opcode) ||
                     protocol::IsNativeRescueOpcode(packet.opcode) ||
                     protocol::IsNativeWeaponObjectOpcode(packet.opcode) ||
                     protocol::IsNativeStormOpcode(packet.opcode))) {
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    // Only weapon presentation enters the ordinary relay. Native hit, health,
    // death, rescue, grenade and locomotion reports cannot mutate mode state.
    bool visual = false;
    if (combat_->actors_.contains(actor)) {
      const auto mode = hide_seek_->Match().Snapshot(actor.Value());
      const bool hunter = mode.role == HideSeekRole::kHunter &&
                          mode.phase != HideSeekPhase::kResults;
      if (hunter && packet.opcode == 32810U) {
        const auto request =
            protocol::DecodeNativeAction(packet.opcode, packet.body);
        const auto& change = *request.weapon_switch;
        visual = (change.bar_index == 0U && change.template_id == 3001U) ||
                 (change.bar_index == 2U && change.template_id == 7001U);
      } else if (hunter && mode.phase != HideSeekPhase::kHiding &&
                 packet.opcode == 32925U) {
        visual = protocol::DecodeNativeAction(packet.opcode, packet.body)
                     .compressed_fire->bar_index == 0U;
      } else if (hunter && mode.phase == HideSeekPhase::kHunting &&
                 packet.opcode == 32811U) {
        visual = true;
      } else if (hunter && mode.phase != HideSeekPhase::kHiding &&
                 packet.opcode == 33281U) {
        visual = protocol::DecodeNativeWeaponObject(packet.opcode, packet.body)
                     .template_id == 7001U;
      }
    }
    if (!visual) return MakeResult(RoomEvent::kNoChange);
  }
  if (crazy_racing_ && protocol::IsNativeRespawnOpcode(packet.opcode)) {
    const auto request =
        protocol::DecodeNativeRespawn(packet.opcode, packet.body);
    const auto member = room_->GetMember(actor).value();
    if (member.object_id && request.actor_object_id != *member.object_id)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kInvalidValue,
          "BFRunner respawn belongs to another character"};
    const auto spawn = crazy_racing_->Spawn(actor);
    if (!spawn || request.respawn_point != spawn->point ||
        packet.opcode == 705U) {
      auto rejected = MakeResult(RoomEvent::kNoChange);
      rejected.combat_reject_reason = CombatRejectReason::kRespawnMismatch;
      return rejected;
    }
  }
  if (weapon_master_ && weapon_master_->Finishing()) {
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = CombatRejectReason::kInactiveMatchPhase;
    return result;
  }
  if (weapon_master_ && weapon_master_->StaleSwitch(actor, packet, *combat_)) {
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = CombatRejectReason::kModeWeaponRestricted;
    return result;
  }
  if (packet.opcode == 32804U && !infection_ && !zombie_pve_) {
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    return MakeResult(RoomEvent::kNoChange);
  }
  if (zombie_pve_ && !PirateMatch::Accepts(packet.opcode) &&
      !zombie_pve_->PrepareAction(actor, packet, *combat_)) {
    static_cast<void>(zombie_pve_->DeferAction(actor, packet, *combat_, now));
    return MakeResult(RoomEvent::kNoChange);
  }
  if (infection_ && !infection_->PrepareAction(actor, packet, *combat_)) {
    return MakeResult(RoomEvent::kNoChange);
  }
  if (captain_ && !captain_->PrepareAction(actor, packet, *combat_)) {
    auto result = MakeResult(RoomEvent::kNoChange);
    result.combat_reject_reason = CombatRejectReason::kModeWeaponRestricted;
    return result;
  }
  const auto map_action =
      map_agents_.PrepareAction(actor, packet, *combat_, now);
  if ((map_action && !*map_action) ||
      (!map_action && pirate_ &&
       !pirate_->PrepareAction(actor, packet, *combat_))) {
    return MakeResult(RoomEvent::kNoChange);
  }
  application::ServiceResult<CombatRelayResult> handled;
  if (packet.opcode == 32905U && infection_) {
    handled = infection_->ReportDuration(actor, packet, *combat_);
  } else if (packet.opcode == 32772U && infection_) {
    handled = infection_->HandleBlast(actor, packet, *combat_, now);
  } else if (packet.opcode == 32898U &&
             (room_->ModeId() == 3U || room_->ModeId() == 21U)) {
    handled = combat_->Handle(actor, packet, now);
  } else if (PirateMatch::Accepts(packet.opcode)) {
    PirateMatch::Validate(packet);
    if (!pirate_) {
      return MakeResult(RoomEvent::kNoChange);
    }
    handled = pirate_->Handle(actor, packet, *combat_, now);
  } else {
    if (packet.opcode == 32887U) {
      // The first string is a name copied from the local scene. Rebuild it from
      // the authenticated profile; the original receiver uses only the text.
      const auto decoded =
          protocol::DecodeNativeAction(packet.opcode, packet.body);
      protocol::PacketReader reader{packet.body};
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadUtf16(256U));
      const auto text = reader.ReadUtf16(domain::kMaximumUserMessageCodeUnits);
      protocol::PacketWriter writer;
      writer.WriteU32(decoded.actor_object_id);
      writer.WriteUtf16(protocol::Utf8ToUtf16(players_.at(actor).nickname));
      writer.WriteUtf16(text);
      handled = combat_->Handle(
          actor, {.opcode = packet.opcode, .body = writer.Release()}, now);
    } else if (racing_ && packet.opcode == 32772U) {
      const auto weapon =
          protocol::DecodeNativeWeaponObject(packet.opcode, packet.body)
              .template_id;
      handled = weapon == 60001U || weapon == 60002U
                    ? application::ServiceResult<
                          CombatRelayResult>{racing_->StoneBlast(actor, packet,
                                                                 *combat_, now)}
                    : combat_->Handle(actor, packet, now);
    } else {
      handled = combat_->Handle(actor, packet, now);
    }
  }
  if (const auto* error = std::get_if<application::ServiceError>(&handled)) {
    return *error;
  }
  auto combat_result = std::get<CombatRelayResult>(std::move(handled));
  auto result = MakeResult(RoomEvent::kNoChange);
  result.combat_reject_reason = combat_result.reject_reason;
  result.deliveries.reserve(combat_result.deliveries.size());
  for (auto& delivery : combat_result.deliveries) {
    result.deliveries.push_back({.recipient = delivery.recipient,
                                 .message = std::move(delivery.message)});
  }
  if (combat_result.accepted) {
    // A direct native movement supersedes any older P2P envelope from the same
    // sender; otherwise the next room tick could relay the stale envelope.
    if (packet.opcode == 32769U || packet.opcode == 32885U)
      pending_network_movements_.erase(actor);
    if (crazy_racing_ && packet.opcode == 32926U) {
      const auto spawn = crazy_racing_->Spawn(actor);
      if (spawn) {
        combat_->actors_.at(actor).position = spawn->position;
        crazy_racing_->ObservePosition(actor, spawn->position, *combat_, now);
      }
    }
    if (infection_) infection_->Observe(actor, packet, *combat_, now);
    if (racing_) racing_->Observe(actor, packet, *combat_, now);
    if (crazy_racing_) crazy_racing_->Observe(actor, packet, *combat_, now);
    if (zombie_pve_) zombie_pve_->ObserveScoreUnits(*combat_);
    if (pirate_ && packet.opcode == 32841U) {
      const auto [wave, group] = pirate_->LastSpawnGroup();
      map_agents_.ObserveGroup(wave, group, now);
    }
    WeaponMasterUpdate weapon_update;
    if (weapon_master_) {
      weapon_update = weapon_master_->Observe(actor, packet, *combat_, now);
      Broadcast(result, weapon_update.messages);
    }
    const bool score_changed =
        PreserveCombatPerformance() || weapon_update.score_changed;
    if (weapon_update.finished) {
      auto finished =
          FinishMatch(application::MatchCompletionKind::kCompleted, now);
      if (auto* value = std::get_if<RoomCommandResult>(&finished))
        value->deliveries.insert(value->deliveries.begin(),
                                 result.deliveries.begin(),
                                 result.deliveries.end());
      return finished;
    }
    if (pirate_ && pirate_->Winner() != 0U) {
      auto finished =
          FinishMatch(application::MatchCompletionKind::kCompleted, now);
      if (auto* value = std::get_if<RoomCommandResult>(&finished)) {
        value->deliveries.insert(
            value->deliveries.begin(),
            std::make_move_iterator(result.deliveries.begin()),
            std::make_move_iterator(result.deliveries.end()));
      }
      return finished;
    }
    if (objectives_) {
      auto advanced = AdvanceObjectives(now);
      if (const auto* error =
              std::get_if<application::ServiceError>(&advanced)) {
        return *error;
      }
      auto objective_result = std::get<RoomCommandResult>(std::move(advanced));
      result.deliveries.insert(
          result.deliveries.end(),
          std::make_move_iterator(objective_result.deliveries.begin()),
          std::make_move_iterator(objective_result.deliveries.end()));
      result.active_timers = std::move(objective_result.active_timers);
      if (objective_result.event != RoomEvent::kNoChange) {
        result.event = objective_result.event;
      }
      if (objective_result.settlement_command.has_value()) {
        result.event = objective_result.event;
        result.settlement_command =
            std::move(objective_result.settlement_command);
        return result;
      }
    }
    if (OrdinaryScoreTargetReached()) {
      auto finished =
          FinishMatch(application::MatchCompletionKind::kCompleted, now);
      if (const auto* error =
              std::get_if<application::ServiceError>(&finished)) {
        return *error;
      }
      auto terminal = std::get<RoomCommandResult>(std::move(finished));
      terminal.deliveries.insert(
          terminal.deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
      terminal.combat_reject_reason = result.combat_reject_reason;
      return terminal;
    }
    if (score_changed) {
      AppendScore(result, 255U);
    }
  }
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::CompleteLoadingEvent(
    RoomEvent event, std::chrono::steady_clock::time_point now) {
  auto result = MakeResult(event);
  if (event != RoomEvent::kCountdownStarted &&
      event != RoomEvent::kMatchStarted && event != RoomEvent::kRoundStarted) {
    return result;
  }

  scene_.kind = 4U;
  std::vector<OutboundMessage> messages;
  for (const auto& member : room_->Members()) {
    messages.push_back(
        {.opcode = 701U,
         .body = protocol::EncodeActivateSlotResponse(member.seat)});
  }
  messages.push_back(
      {.opcode = 702U, .body = protocol::EncodeLoadingOptionsResponse({})});
  if (zombie_pve_) messages.push_back(ZombiePveMatch::SkillSlots());
  messages.push_back({.opcode = 703U,
                      .body = protocol::EncodeCountdownResponse(
                          ProtocolMilliseconds(timing_.countdown))});
  if (event == RoomEvent::kMatchStarted || event == RoomEvent::kRoundStarted) {
    messages.push_back(
        {.opcode = 704U,
         .body = protocol::EncodeMatchBeginResponse(
             {.duration_ms = (scene_.configuration.mode_id == 23U ||
                              scene_.configuration.mode_id == 25U)
                                 ? 0x7FFFFFFFU
                                 : ProtocolMilliseconds(timing_.match_duration),
              .scene_id = scene_.scene_id})});
  }
  Broadcast(result, messages);
  if (event == RoomEvent::kRoundStarted) {
    round_deadline_ = now + timing_.match_duration;
    if (objectives_) {
      objectives_->StartBombRound(now);
      room_->SetObjectiveDeadline(objectives_->Deadline());
      result.active_timers = room_->ActiveTimers();
    }
    AppendScore(result, 255U);
  }
  for (const auto& member : room_->Members()) {
    if (member.player_id != room_->HostPlayerId()) {
      for (const auto& message : loading_map_agents_) {
        result.deliveries.push_back(
            {.recipient = member.player_id, .message = message});
      }
    }
  }
  loading_map_agents_.clear();
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::ApplyTimer(
    const RoomTimerToken& token, std::chrono::steady_clock::time_point now) {
  const auto active_timers = room_->ActiveTimers();
  if (std::ranges::find(active_timers, token) == active_timers.end()) {
    return MakeResult(RoomEvent::kIgnoredStaleTimer);
  }
  if (now < token.deadline) {
    return MakeResult(RoomEvent::kNoChange);
  }
  if (token.kind == RoomTimerKind::kCombatTick) {
    auto applied = room_->ApplyTimer(token, now);
    if (const auto* error = std::get_if<application::ServiceError>(&applied))
      return *error;
    const auto event = std::get<RoomEvent>(applied);
    auto result = MakeResult(event);
    if (event == RoomEvent::kCombatTickDue && combat_ &&
        room_->Phase() == RoomPhase::kPlaying)
      AppendMovementTick(result);
    else
      pending_network_movements_.clear();
    result.active_timers = room_->ActiveTimers();
    return result;
  }
  if (hide_seek_ && token.kind == RoomTimerKind::kMatchTimeout)
    return AdvanceHideSeek(now);
  if (token.kind == RoomTimerKind::kModeObjective) {
    static_cast<void>(room_->ApplyTimer(token, now));
    return AdvanceObjectives(now);
  }
  if (token.kind == RoomTimerKind::kMatchTimeout && room_->ModeId() == 19U &&
      objectives_) {
    room_->SuspendMatchTimeout();
    return AdvanceObjectives(now);
  }
  if ((token.kind == RoomTimerKind::kResultsReturn ||
       token.kind == RoomTimerKind::kResultsDisplay) &&
      pending_settlement_.has_value()) {
    return PendingJournalError();
  }
  std::optional<application::MatchSettlementCommand> prepared_settlement;
  if (token.kind == RoomTimerKind::kLoadingTimeout &&
      active_match_.has_value()) {
    PreserveCombatPerformance();
    auto command = BuildSettlementCommand(
        application::MatchCompletionKind::kInterrupted, now);
    if (const auto* error = std::get_if<application::ServiceError>(&command)) {
      return *error;
    }
    prepared_settlement =
        std::get<application::MatchSettlementCommand>(std::move(command));
  }
  if (token.kind == RoomTimerKind::kMatchTimeout &&
      scene_.configuration.submode_id == 4U)
    return FinishMapBrowse();
  if (token.kind == RoomTimerKind::kMatchTimeout &&
      !active_match_.has_value()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "对局尚未安装已持久化 match_id，不能生成终局");
  }
  if (token.kind == RoomTimerKind::kMatchTimeout) {
    // 先生成不含外部副作用的终局命令，再推进房间状态，避免校验失败后房间已经进入
    // Results。
    PreserveCombatPerformance();
    auto command = BuildSettlementCommand(
        application::MatchCompletionKind::kCompleted, now);
    if (const auto* error = std::get_if<application::ServiceError>(&command)) {
      return MakeError(
          application::ErrorCode::kInternal,
          "已验证本局无法生成可靠结算命令：" + error->safe_message);
    }
    prepared_settlement.emplace(
        std::get<application::MatchSettlementCommand>(std::move(command)));
  }
  auto applied = room_->ApplyTimer(token, now);
  if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
    return *error;
  }
  const auto event = std::get<RoomEvent>(applied);
  auto result = MakeResult(event);
  if (event == RoomEvent::kMatchStarted || event == RoomEvent::kRoundStarted) {
    const std::array messages{OutboundMessage{
        .opcode = 704U,
        .body = protocol::EncodeMatchBeginResponse(
            {.duration_ms = (scene_.configuration.mode_id == 23U ||
                             scene_.configuration.mode_id == 25U)
                                ? 0x7FFFFFFFU
                                : ProtocolMilliseconds(timing_.match_duration),
             .scene_id = scene_.scene_id})}};
    Broadcast(result, messages);
    if (event == RoomEvent::kRoundStarted) {
      scene_.kind = 4U;
      round_deadline_ = now + timing_.match_duration;
      if (objectives_) {
        objectives_->StartBombRound(now);
        room_->SetObjectiveDeadline(objectives_->Deadline());
        result.active_timers = room_->ActiveTimers();
      }
      AppendScore(result, 255U);
    }
  } else if (event == RoomEvent::kResultsStarted) {
    combat_.reset();
    pending_network_movements_.clear();
    if (!prepared_settlement.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "终局状态转换缺少预生成结算命令");
    }
    pending_settlement_.emplace(std::move(*prepared_settlement));
    result.settlement_command = *pending_settlement_;
    AppendMatchResult(result, *pending_settlement_);
  } else if (event == RoomEvent::kResultsDisplayDue) {
    const std::array messages{OutboundMessage{.opcode = 733U, .body = {}}};
    Broadcast(result, messages);
  } else if (event == RoomEvent::kReturnedToWaiting) {
    combat_.reset();
    pending_network_movements_.clear();
    npc_prestart_.clear();
    npc_prestart_bytes_ = 0U;
    crazy_racing_.reset();
    scene_.kind = 2U;
    map_assets_.clear();
    latency_probes_.clear();
    normal_match_.reset();
    rotating_match_.reset();
    active_match_.reset();
    pending_settlement_.reset();
    objectives_.reset();
    pending_soccer_ball_.reset();
    if (prepared_settlement.has_value()) {
      pending_settlement_ = std::move(prepared_settlement);
      result.settlement_command = *pending_settlement_;
    }
    const auto notice = token.kind == RoomTimerKind::kLoadingTimeout
                            ? std::string_view{"加载超时，请重新开始"}
                            : std::string_view{};
    AppendReturnToWaiting(result, notice);
  }
  AppendNetworkState(result);
  AppendNetworkPeers(result);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::FinishMatch(
    application::MatchCompletionKind completion,
    std::chrono::steady_clock::time_point now) {
  if (room_->Phase() != RoomPhase::kPlaying) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只有 Playing 房间允许提交权威终局");
  }
  if (scene_.configuration.submode_id == 4U) return FinishMapBrowse();
  if (!active_match_.has_value()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "对局尚未安装已持久化 match_id，不能生成终局");
  }

  // 构造命令可能因终局类型、时间或冻结快照不合法而失败；必须在房间状态变化前完成。
  PreserveCombatPerformance();
  if (captain_ && combat_) captain_->Stop(*combat_);
  auto command = BuildSettlementCommand(completion, now);
  if (const auto* error = std::get_if<application::ServiceError>(&command)) {
    return *error;
  }

  auto finished = room_->FinishMatch(now);
  if (const auto* error = std::get_if<application::ServiceError>(&finished)) {
    return *error;
  }
  const auto event = std::get<RoomEvent>(finished);
  if (event != RoomEvent::kResultsStarted) {
    return MakeError(application::ErrorCode::kInternal,
                     "权威终局没有进入 Results 阶段");
  }

  auto result = MakeResult(event);
  combat_.reset();
  pending_network_movements_.clear();
  pending_settlement_.emplace(
      std::get<application::MatchSettlementCommand>(std::move(command)));
  result.settlement_command = *pending_settlement_;
  AppendMatchResult(result, *pending_settlement_);
  return result;
}

application::ServiceResult<application::MatchSettlementCommand>
RoomCommandFlow::PrepareAbandonedSettlement(
    std::chrono::steady_clock::time_point now) const {
  const auto members = room_->Members();
  if (!HasUnsettledMatch() || members.size() != 1U) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只有最后一名成员离开 Playing 房间时才能准备放弃终局");
  }
  auto command =
      BuildSettlementCommand(application::MatchCompletionKind::kAbandoned, now);
  if (const auto* error = std::get_if<application::ServiceError>(&command)) {
    return *error;
  }
  auto value =
      std::get<application::MatchSettlementCommand>(std::move(command));
  const auto participant =
      std::ranges::find(value.participants, members.front().player_id,
                        &application::MatchSettlementParticipant::player_id);
  if (participant == value.participants.end()) {
    if (members.front().IsObserver() &&
        std::ranges::all_of(value.participants,
                            [](const auto& player) { return player.departed; }))
      return value;
    return MakeError(application::ErrorCode::kInternal,
                     "最后房间成员不在已持久化对局开局名单中");
  }
  participant->departed = true;
  return value;
}

application::ServiceResult<application::MatchSettlementCommand>
RoomCommandFlow::PendingSettlement() const {
  if (!pending_settlement_.has_value()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间没有等待耐久保存的终局命令");
  }
  return *pending_settlement_;
}

application::ServiceResult<bool> RoomCommandFlow::AcknowledgeSettlementStaged(
    const domain::MatchId& match_id) {
  if (!pending_settlement_.has_value()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间没有可确认的终局命令");
  }
  if (pending_settlement_->match_id != match_id) {
    return MakeError(application::ErrorCode::kConflict,
                     "终局日志回执与房间待确认 match_id 不一致");
  }
  if (active_match_.has_value() && active_match_->match_id == match_id) {
    active_match_->terminal_staged = true;
  }
  pending_settlement_.reset();
  return true;
}

application::ServiceResult<bool> RoomCommandFlow::InstallCombat(
    const RoomMatchInstallation& installation) {
  if (room_->Phase() != RoomPhase::kPlaying) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只有 Playing 房间允许安装战斗参与者快照");
  }
  if (combat_) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前对局已经安装战斗账本");
  }

  const bool map_browse = scene_.configuration.submode_id == 4U;
  if (!map_browse && room_->ModeId() == 26U &&
      (room_->Participants().size() < 6U || room_->Participants().size() > 8U))
    return MakeError(application::ErrorCode::kInvalidState,
                     "武器大师仅支持 6～8 名参赛玩家");
  const bool practice = map_browse || room_->IsTraining();
  if (practice == installation.match_id.has_value())
    return MakeError(application::ErrorCode::kInternal,
                     "正式对局必须有持久化身份，练习会话必须没有正式身份");

  auto participants = installation.participants;
  for (auto& participant : participants) {
    if (room_->IsHideSeek()) {
      // Ephemeral combat permission only. Lobby/SQL equipment snapshots keep
      // the player's real inventory, and the next scene restores that loadout.
      participant.weapon_templates = {3001U, std::nullopt, 7001U, std::nullopt,
                                      std::nullopt};
      participant.ordinary_respawn_allowed = false;
      participant.lucci_pouch = participant.chocolate_box = false;
      participant.equipment_items.clear();
    }
    participant.ordinary_spawn_point.reset();
    participant.ordinary_spawn_range.reset();
    participant.ordinary_spawn_poses.clear();
  }
  if (!domain::ValidMatchScoreModel(installation.score_model)) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "本局计分模型尚未恢复");
  }

  const auto members = room_->Participants();
  if (participants.size() != members.size()) {
    return MakeError(application::ErrorCode::kInternal,
                     "战斗参与者数量与房间成员不一致");
  }
  for (const auto& member : members) {
    const auto participant = std::ranges::find(participants, member.player_id,
                                               &CombatParticipant::player_id);
    if (participant == participants.end() || !member.object_id.has_value() ||
        participant->actor_object_id != member.object_id.value() ||
        participant->team_code != member.team) {
      return MakeError(application::ErrorCode::kInternal,
                       "战斗参与者与房间对象或队伍注册快照不一致");
    }
    participant->seat_index = static_cast<std::uint8_t>(member.seat);
  }

  if (!practice && FindNativeTeamSpawnModeMap(room_->ModeId(),
                                              scene_.configuration.map_key)) {
    std::vector<NativeTeamSpawnSeat> spawn_roster;
    spawn_roster.reserve(members.size());
    for (const auto& member : members)
      spawn_roster.push_back({.seat = static_cast<std::uint8_t>(member.seat),
                              .team = member.team});
    for (const auto& member : members) {
      const auto range = NativeTeamRespawnRange(
          room_->ModeId(), scene_.configuration.map_key, spawn_roster,
          static_cast<std::uint8_t>(member.seat));
      if (!range)
        return MakeError(application::ErrorCode::kInternal,
                         "原普通团队出生点名单无效");
      const auto participant = std::ranges::find(participants, member.player_id,
                                                 &CombatParticipant::player_id);
      if (range->first == range->last)
        participant->ordinary_spawn_point = range->first;
      else
        participant->ordinary_spawn_range = range;
      for (auto point = range->first; point <= range->last; ++point)
        if (const auto pose = NativeTeamRespawnPose(
                room_->ModeId(), scene_.configuration.map_key, point))
          participant->ordinary_spawn_poses.push_back(*pose);
    }
  }

  std::vector<domain::PlayerId> observers;
  for (const auto& member : room_->Members()) {
    if (member.IsObserver()) observers.push_back(member.player_id);
  }

  if (practice) {
    try {
      auto combat = std::make_unique<CombatRelay>(
          participants, false, observers,
          room_->IsTraining() ? kTrainingMode : 0U,
          (room_->GlobalRules() & protocol::kRoomRuleNoGrenades) != 0U);
      combat->SetHost(room_->HostPlayerId());
      if (room_->IsTraining())
        training_ = std::make_unique<TrainingRoomMatch>(
            room_->HostPlayerId(), participants.front().actor_object_id,
            *combat);
      combat->EnableMovementTickAggregation();
      combat_ = std::move(combat);
      room_->EnableCombatTick(std::chrono::steady_clock::now());
      map_region_pending_.clear();
      return true;
    } catch (const std::invalid_argument&) {
      return MakeError(application::ErrorCode::kInternal,
                       "地图浏览参与者身份或对象 ID 无效");
    }
  }

  std::map<domain::PlayerId, domain::MatchResultParticipantInput> roster;
  std::map<domain::PlayerId, protocol::NativeScorePlayer> score_players;
  for (const auto& member : members) {
    const auto profile = players_.find(member.player_id);
    if (profile == players_.end() ||
        member.player_id.Value() > std::numeric_limits<std::uint32_t>::max() ||
        profile->second.progress.level < 1 ||
        profile->second.progress.level > 1000) {
      return MakeError(application::ErrorCode::kInternal,
                       "开局缺少可编码的原生成绩身份");
    }
    score_players.emplace(
        member.player_id,
        protocol::NativeScorePlayer{
            .player_id = static_cast<std::uint32_t>(member.player_id.Value()),
            .seat = static_cast<std::uint8_t>(member.seat),
            .team = member.team,
            .nickname = profile->second.nickname,
            .level = static_cast<std::uint32_t>(profile->second.progress.level),
            .performance = {},
            .reward = std::nullopt,
            .mode = room_->ModeId()});
    roster.emplace(member.player_id,
                   domain::MatchResultParticipantInput{
                       .player_id = member.player_id,
                       .seat_index = static_cast<std::uint8_t>(member.seat),
                       .team_code = member.team,
                       .performance = {},
                       .departed = false,
                       .racing_tie_break = room_->ModeId() == 24U
                                               ? std::optional<std::int32_t>{0}
                                               : std::nullopt});
  }
  try {
    auto combat = std::make_unique<CombatRelay>(
        participants,
        domain::IndividualMatchScoreModel(installation.score_model), observers,
        room_->ModeId(),
        (room_->GlobalRules() & protocol::kRoomRuleNoGrenades) != 0U);
    for (const auto& [player, opcode] : native_prestart_gaps_)
      combat->MarkNativeConstructorGap(player, opcode);
    active_match_.emplace(
        ActiveMatch{.match_id = *installation.match_id,
                    .score_model = installation.score_model,
                    .score_target = scene_.configuration.score_target,
                    .started_at = installation.started_at,
                    .participants = std::move(roster),
                    .score_players = std::move(score_players)});
    combat_ = std::move(combat);
    if (room_->IsHideSeek()) {
      if (!timing_.hide_seek_scene || !timing_.hide_seek_scene->Valid())
        throw std::invalid_argument("unverified hide_seek_v1 scene");
      std::vector<HideSeekMember> hide_members;
      std::vector<HideSeekPlayer> hide_hunters;
      for (const auto& member : members) {
        hide_members.push_back({member.player_id.Value(), *member.object_id,
                                static_cast<std::uint8_t>(member.seat)});
        if (member.team == 2U) hide_hunters.push_back(member.player_id.Value());
      }
      std::random_device random;
      const auto epoch =
          (static_cast<std::uint64_t>(random()) << 32U) | random();
      hide_seek_ = std::make_unique<HideSeekSession>(
          HideSeekMatch({}, *timing_.hide_seek_scene, hide_members,
                        epoch ? epoch : 1U, random(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            installation.started_at.time_since_epoch())
                            .count(),
                        hide_hunters));
    }
    native_prestart_gaps_.clear();
    if (zombie_pve_) zombie_pve_->Install(room_->HostPlayerId(), *combat_);
    infection_.reset();
    weapon_master_.reset();
    racing_.reset();
    crazy_racing_.reset();
    captain_.reset();
    if (room_->ModeId() == 8U || room_->ModeId() == 28U) {
      static thread_local std::mt19937 random{std::random_device{}()};
      const auto index = std::uniform_int_distribution<std::size_t>{
          0U, members.size() - 1U}(random);
      // Keep a playable human opening after 704 instead of transforming in the
      // same batch. Five seconds restores the observed 5–6 second opening;
      // this is server timing, not a hard-coded constant recovered from the PE.
      infection_ = std::make_unique<InfectionMatch>(
          room_->ModeId(), members[index].player_id,
          active_match_->started_at + std::chrono::seconds{5});
    }
    if (room_->ModeId() == 26U)
      weapon_master_ = std::make_unique<WeaponMasterMatch>(
          timing_.weapon_master, std::random_device{}());
    if (room_->ModeId() == 20U) {
      // Both teams need a living candidate; CaptainAssignments rejects
      // one-sided rosters.
      std::array<bool, 2U> teams{};
      for (const auto& participant : participants)
        if (participant.team_code == 1U || participant.team_code == 2U)
          teams[participant.team_code - 1U] = true;
      if (!teams[0] || !teams[1])
        throw std::invalid_argument{"captain mode requires red and blue teams"};
      captain_ = std::make_unique<CaptainAssignments>(std::random_device{}());
      captain_fever_clock_.clear();
    }
    if (room_->ModeId() == 24U)
      // Unknown maps throw std::invalid_argument and refuse the start below.
      racing_ = std::make_unique<ExtremeRacingMatch>(ExtremeRacingScene::Load(
          scene_.configuration.map_key, std::random_device{}()));
    if (room_->ModeId() == 21U)
      crazy_racing_ = std::make_unique<CrazyRacingMatch>(
          scene_.configuration.map_key, participants);
    pirate_.reset();
    if ((room_->ModeId() == 101U || room_->ModeId() == 102U)) {
      const auto host = room_->GetMember(room_->HostPlayerId());
      if (!host.has_value() || !host->object_id.has_value()) {
        throw std::invalid_argument{"pirate host object missing"};
      }
      pirate_ = std::make_unique<PirateMatch>(room_->HostPlayerId(),
                                              *host->object_id);
    }
    const auto assets = map_assets_.find(room_->HostPlayerId());
    std::map<domain::PlayerId, std::u16string> nicknames;
    for (const auto& [id, profile] : active_match_->score_players)
      nicknames.emplace(id, protocol::Utf8ToUtf16(profile.nickname));
    objectives_ = std::make_unique<MatchObjectives>(
        MatchObjectiveRules{.mode = room_->ModeId(),
                            .target = scene_.configuration.score_target,
                            .map_key = scene_.configuration.map_key,
                            .nicknames = std::move(nicknames)},
        participants,
        assets == map_assets_.end() ? protocol::MapAssetsRequest{}
                                    : assets->second,
        installation.started_at);
    map_region_pending_.clear();
    combat_->SetHost(room_->HostPlayerId());
    if (pending_soccer_ball_)
      static_cast<void>(objectives_->RegisterSoccerBall(
          room_->HostPlayerId(), *pending_soccer_ball_, *combat_));
    round_deadline_ = installation.started_at + timing_.match_duration;
    combat_->EnableMovementTickAggregation();
    room_->EnableCombatTick(std::chrono::steady_clock::now());
  } catch (const std::invalid_argument&) {
    return MakeError(application::ErrorCode::kInternal,
                     "战斗参与者身份或对象 ID 无效");
  }
  return true;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::FinishMapBrowse() {
  if (scene_.configuration.submode_id != 4U ||
      room_->Phase() != RoomPhase::kPlaying || active_match_ ||
      pending_settlement_)
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前会话不是可结束的地图浏览");
  auto returned = room_->ReturnToWaiting();
  if (const auto* error = std::get_if<application::ServiceError>(&returned))
    return *error;
  combat_.reset();
  pending_network_movements_.clear();
  objectives_.reset();
  pirate_.reset();
  infection_.reset();
  weapon_master_.reset();
  racing_.reset();
  captain_.reset();
  zombie_pve_.reset();
  pending_pirate_wave_start_.reset();
  pending_soccer_ball_.reset();
  map_region_pending_.clear();
  map_assets_.clear();
  map_agents_ = {};
  loading_map_agents_.clear();
  npc_prestart_.clear();
  npc_prestart_bytes_ = 0U;
  latency_probes_.clear();
  normal_match_.reset();
  rotating_match_.reset();
  scene_.kind = 2U;
  auto result = MakeResult(RoomEvent::kReturnedToWaiting);
  AppendReturnToWaiting(result, {});
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::AbortFailedMatchStart(std::string_view notice) {
  if (room_->Phase() != RoomPhase::kPlaying || active_match_.has_value() ||
      combat_) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只有尚未安装战斗账本的 Playing 房间允许撤销失败开局");
  }
  auto returned = room_->ReturnToWaiting();
  if (const auto* error = std::get_if<application::ServiceError>(&returned)) {
    return *error;
  }
  if (std::get<RoomEvent>(returned) != RoomEvent::kReturnedToWaiting) {
    return MakeError(application::ErrorCode::kInternal,
                     "撤销失败开局没有恢复等待态");
  }
  scene_.kind = 2U;
  map_assets_.clear();
  latency_probes_.clear();
  normal_match_.reset();
  rotating_match_.reset();
  npc_prestart_.clear();
  npc_prestart_bytes_ = 0U;
  auto result = MakeResult(RoomEvent::kReturnedToWaiting);
  try {
    AppendReturnToWaiting(result, notice);
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "撤销开局后的房间消息无法编码");
  } catch (const std::logic_error&) {
    return MakeError(application::ErrorCode::kInternal,
                     "撤销开局后的房间快照不完整");
  }
  return result;
}

application::ServiceResult<bool> RoomCommandFlow::UpdatePlayerSnapshot(
    application::PlayerMenuSnapshot snapshot) {
  if (!snapshot.player_id.IsValid() ||
      !room_->GetMember(snapshot.player_id).has_value()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "资料快照玩家不在当前房间");
  }

  const auto player_id = snapshot.player_id;
  const bool replaced = players_.contains(player_id);
  auto original = players_.find(player_id);
  std::optional<application::PlayerMenuSnapshot> previous;
  if (original != players_.end()) {
    previous = original->second;
  }
  players_.insert_or_assign(player_id, std::move(snapshot));
  try {
    static_cast<void>(MemberUpdate(player_id));
  } catch (const protocol::ProtocolError&) {
    if (previous.has_value()) {
      players_.insert_or_assign(player_id, std::move(*previous));
    } else {
      players_.erase(player_id);
    }
    return MakeError(application::ErrorCode::kInternal,
                     "玩家资料快照不能编码为房间席位");
  }
  return replaced;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::AddMemberSnapshot(application::PlayerMenuSnapshot snapshot) {
  const auto player_id = snapshot.player_id;
  const auto joining_member = room_->GetMember(player_id);
  const bool late_observer = room_->IsHideSeek() &&
                             room_->Phase() == RoomPhase::kPlaying &&
                             joining_member && joining_member->IsObserver();
  if (!player_id.IsValid() ||
      (room_->Phase() != RoomPhase::kWaiting && !late_observer) ||
      !room_->GetMember(player_id).has_value() ||
      players_.contains(player_id)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "加入后的房间成员与资料缓存状态不一致");
  }

  const auto previous_occupants = scene_.occupants;
  const auto previous_observers = scene_.observers;
  players_.emplace(player_id, std::move(snapshot));
  scene_.occupants = static_cast<std::uint16_t>(room_->Participants().size());
  scene_.observers =
      static_cast<std::uint16_t>(room_->Members().size() - scene_.occupants);
  try {
    auto result = MakeResult(RoomEvent::kMemberJoined);
    CancelUserHostAuthorization(result);
    if (room_->UserHosted())
      result.deliveries.push_back(
          {player_id,
           {999U, protocol::EncodeUserHostDefaultSetting(
                      timing_.user_host_prize.minimum_amount,
                      timing_.user_host_prize.maximum_amount)}});
    result.deliveries.push_back(
        {.recipient = player_id,
         .message = {.opcode = 112U,
                     .body = protocol::EncodeSceneSessionResponse(
                         RoomSnapshot(player_id))}});

    const std::array peer_messages{
        MemberUpdate(player_id),
        OutboundMessage{
            .opcode = 501U,
            .body = protocol::EncodeRoomSceneUpdateResponse(scene_)}};
    for (const auto& member : room_->Members()) {
      if (member.player_id == player_id) {
        continue;
      }
      for (const auto& message : peer_messages) {
        result.deliveries.push_back(
            {.recipient = member.player_id, .message = message});
      }
    }
    return result;
  } catch (const protocol::ProtocolError&) {
    players_.erase(player_id);
    scene_.occupants = previous_occupants;
    scene_.observers = previous_observers;
    return MakeError(application::ErrorCode::kInternal,
                     "新成员资料不能编码为已验证房间协议");
  } catch (const std::logic_error&) {
    players_.erase(player_id);
    scene_.occupants = previous_occupants;
    scene_.observers = previous_observers;
    return MakeError(application::ErrorCode::kInternal,
                     "新成员加入后房间快照契约失效");
  }
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::RemoveMemberSnapshot(
    domain::PlayerId departed, const RoomDeparture& departure,
    std::string_view interrupted_notice,
    std::chrono::steady_clock::time_point now) {
  hide_seek_capabilities_.erase(departed);
  const bool observer_left = hide_seek_observer_loading_.erase(departed) != 0;
  if (!departure.room || departure.room.get() != room_.get() ||
      departure.room_removed || room_->Phase() == RoomPhase::kClosed ||
      room_->GetMember(departed).has_value() || !players_.contains(departed)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "离开后的房间成员与资料缓存状态不一致");
  }
  const auto host = room_->GetMember(room_->HostPlayerId());
  const auto host_player = players_.find(room_->HostPlayerId());
  if (!host.has_value() || host_player == players_.end()) {
    return MakeError(application::ErrorCode::kInternal,
                     "成员离开后缺少新房主资料");
  }

  std::vector<OutboundMessage> departure_updates;
  if (observer_left && hide_seek_)
    static_cast<void>(hide_seek_->Leave(
        departed.Value(), std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch())
                              .count()));
  std::optional<ExtremeRacingUpdate> departure_racing;
  std::optional<application::MatchSettlementCommand> interrupted;
  if (active_match_.has_value() && !active_match_->rewards_applied &&
      !active_match_->terminal_staged && !pending_settlement_) {
    PreserveCombatPerformance();
    const auto match_participant = active_match_->participants.find(departed);
    if (match_participant != active_match_->participants.end()) {
      match_participant->second.departed = true;
    }
    if (combat_) {
      if (zombie_pve_ && room_->Phase() == RoomPhase::kPlaying)
        departure_updates =
            zombie_pve_->MemberLeft(departed, room_->HostPlayerId(), *combat_);
      if (racing_ && room_->Phase() == RoomPhase::kPlaying) {
        combat_->SetHost(room_->HostPlayerId());
        departure_racing = racing_->Leave(departed, now);
      }
      static_cast<void>(combat_->RemoveParticipant(departed));
    }
    if (departure.start_interrupted) {
      auto prepared = BuildSettlementCommand(
          application::MatchCompletionKind::kInterrupted, now);
      if (const auto* error =
              std::get_if<application::ServiceError>(&prepared)) {
        return *error;
      }
      interrupted =
          std::get<application::MatchSettlementCommand>(std::move(prepared));
      combat_.reset();
      pending_network_movements_.clear();
    }
  } else {
    if (scene_.configuration.submode_id == 4U &&
        room_->Phase() == RoomPhase::kPlaying) {
      scene_.occupants =
          static_cast<std::uint16_t>(room_->Participants().size());
      scene_.field_40 = host_player->second.nickname;
      players_.erase(departed);
      return FinishMapBrowse();
    }
    // 加载或等待阶段没有可延续的终局账本；立即停用，避免旧对象继续成为合法目标。
    combat_.reset();
    pending_network_movements_.clear();
  }

  const auto previous_scene = scene_;
  scene_.occupants = static_cast<std::uint16_t>(room_->Participants().size());
  scene_.observers =
      static_cast<std::uint16_t>(room_->Members().size() - scene_.occupants);
  scene_.configuration.field_12 = room_->UserHosted() ? 1U : 0U;
  scene_.field_40 = host_player->second.nickname;
  if (departure.start_interrupted) {
    scene_.kind = 2U;
  }

  try {
    auto result =
        MakeResult(departure.start_interrupted ? RoomEvent::kReturnedToWaiting
                                               : RoomEvent::kMemberLeft);
    CancelUserHostAuthorization(result);
    if (departure.new_host) ResetUserHostPrize();
    if (interrupted.has_value()) {
      pending_settlement_ = std::move(interrupted);
      result.settlement_command = *pending_settlement_;
    }
    std::vector<OutboundMessage> messages{
        OutboundMessage{
            .opcode = 511U,
            .body = protocol::EncodeSceneMemberRemovedResponse(
                static_cast<std::uint8_t>(departure.departed_seat))},
        OutboundMessage{
            .opcode = 501U,
            .body = protocol::EncodeRoomSceneUpdateResponse(scene_)}};
    if (departure.new_host.has_value()) {
      messages.push_back(
          {.opcode = 502U,
           .body = protocol::EncodeRoomHostChangedResponse(host->seat)});
    }
    Broadcast(result, messages);
    Broadcast(result, departure_updates);
    if (departure.start_interrupted) {
      AppendReturnToWaiting(result, interrupted_notice);
    }

    players_.erase(departed);
    pending_network_movements_.erase(departed);
    map_assets_.erase(departed);
    network_endpoints_.erase(departed);
    network_setting_clients_.erase(departed);
    item_setting_clients_.erase(departed);
    room_setting_versions_.erase(departed);
    AppendNetworkState(result);
    AppendNetworkPeers(result);
    latency_probes_.clear();
    if (departure.start_interrupted) {
      map_assets_.clear();
      normal_match_.reset();
      rotating_match_.reset();
    }
    if (departure_racing)
      return ApplyRacingUpdate(std::move(result), std::move(*departure_racing),
                               now);
    if (hide_seek_ && room_->Phase() == RoomPhase::kPlaying) {
      // Departures change the mode roster immediately, even when no remaining
      // client is polling. Waiting for the next phase timer can leave a
      // departed final cat alive or allow a reconnect before its tombstone is
      // recorded.
      auto advanced = AdvanceHideSeek(now);
      if (const auto* error = std::get_if<application::ServiceError>(&advanced))
        return *error;
      auto current = std::get<RoomCommandResult>(std::move(advanced));
      if (current.event == RoomEvent::kNoChange)
        current.event = RoomEvent::kMemberLeft;
      current.deliveries.insert(
          current.deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
      return current;
    }
    if ((infection_ || zombie_pve_) && combat_ &&
        room_->Phase() == RoomPhase::kPlaying) {
      combat_->SetHost(room_->HostPlayerId());
      // Removing a mother or the final human can end the match immediately.
      // Otherwise discard departed respawn timers and retain the current game.
      auto advanced = AdvanceObjectives(now);
      if (const auto* error =
              std::get_if<application::ServiceError>(&advanced)) {
        return *error;
      }
      auto current = std::get<RoomCommandResult>(std::move(advanced));
      if (current.event == RoomEvent::kNoChange) {
        current.event = RoomEvent::kMemberLeft;
      }
      current.deliveries.insert(
          current.deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
      return current;
    }
    return result;
  } catch (const protocol::ProtocolError&) {
    scene_ = previous_scene;
    return MakeError(application::ErrorCode::kInternal,
                     "成员离开后房间消息不能编码为已验证协议");
  } catch (const std::logic_error&) {
    scene_ = previous_scene;
    return MakeError(application::ErrorCode::kInternal,
                     "成员离开后房间快照契约失效");
  }
}

application::ServiceResult<OutboundMessage>
RoomCommandFlow::MakeSceneSessionMessage(domain::PlayerId selected) const {
  if (!room_->GetMember(selected).has_value() || !players_.contains(selected)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在当前房间资料缓存中");
  }
  try {
    return OutboundMessage{
        .opcode = 112U,
        .body = protocol::EncodeSceneSessionResponse(RoomSnapshot(selected))};
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "房间初始快照不能编码为已验证协议");
  } catch (const std::logic_error&) {
    return MakeError(application::ErrorCode::kInternal, "房间初始快照契约失效");
  }
}

const protocol::SceneInfo& RoomCommandFlow::Scene() const noexcept {
  return scene_;
}

bool RoomCommandFlow::HasUnsettledMatch() const noexcept {
  return active_match_.has_value() && !pending_settlement_.has_value() &&
         !active_match_->rewards_applied && !active_match_->terminal_staged &&
         room_->Phase() != RoomPhase::kResults;
}

bool RoomCommandFlow::CanChangeCombatPreset(
    domain::PlayerId player) const noexcept {
  // D10870 的限制及 848380/D242B0 的特殊配装路径不使用普通预设同步。
  const auto mode = room_->ModeId();
  return room_->Phase() == RoomPhase::kPlaying && combat_ && mode != 8U &&
         mode != 15U && mode != 16U && mode != 20U && mode != 21U &&
         mode != 23U && mode != 24U && mode != 25U && mode != 26U &&
         mode != 27U && mode != 28U && combat_->CanChangePreset(player);
}

std::optional<application::UseItemCommand>
RoomCommandFlow::PrepareInstantRespawn(domain::PlayerId player,
                                       std::uint32_t object) const {
  if (room_->Phase() != RoomPhase::kPlaying || !active_match_ || !combat_ ||
      (weapon_master_ && weapon_master_->Finishing()) ||
      !combat_->CanInstantRespawn(player, object) || room_->ModeId() == 23U ||
      room_->ModeId() == 25U) {
    return std::nullopt;
  }
  return application::UseItemCommand{
      .player_id = player,
      .item_id = domain::ItemId{100443U},
      .operation = application::ItemUseOperation::kInstantRespawn,
      .match_id = active_match_->match_id,
      .life_generation = combat_->LifeGeneration(player).value()};
}

bool RoomCommandFlow::AuthorizeInstantRespawn(
    const application::UseItemCommand& command) {
  return room_->Phase() == RoomPhase::kPlaying && active_match_ && combat_ &&
         (!weapon_master_ || !weapon_master_->Finishing()) &&
         command.match_id == active_match_->match_id &&
         combat_->AuthorizeInstantRespawn(command.player_id,
                                          command.life_generation);
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::EquipmentChanged(
    application::PlayerMenuSnapshot snapshot) {
  const auto player = snapshot.player_id;
  if (CanChangeCombatPreset(player)) {
    const auto message = ItemRejection(snapshot);
    if (!message.empty())
      return MakeError(application::ErrorCode::kConflict, message);
    // 与开局仓储的原生默认槽一致；完整快照同时保留按页独立的水晶。
    std::array<std::optional<std::uint32_t>, 5U> weapons{2004U, 1004U, 7001U,
                                                         5033U, std::nullopt};
    constexpr std::array<std::string_view, 4U> slots{"primary", "secondary",
                                                     "melee", "grenade"};
    auto fighter = static_cast<std::uint32_t>(snapshot.initial_fighter);
    std::vector<std::uint32_t> equipment_ids;
    for (const auto& item : snapshot.items) {
      if (item.equipped &&
          item.item_id.Value() <= std::numeric_limits<std::uint32_t>::max())
        equipment_ids.push_back(
            static_cast<std::uint32_t>(item.item_id.Value()));
      if (item.equipped && item.slot_code == "character" &&
          item.item_id.Value() >= 300001U && item.item_id.Value() < 400000U)
        fighter = static_cast<std::uint32_t>(item.item_id.Value() - 300000U);
      const auto slot = std::ranges::find(slots, item.slot_code);
      if (!item.equipped || slot == slots.end()) {
        continue;
      }
      if (item.item_id.Value() <= 200000U || item.item_id.Value() >= 300000U) {
        return MakeError(application::ErrorCode::kInternal,
                         "预设武器无法映射原生模板");
      }
      weapons[static_cast<std::size_t>(slot - slots.begin())] =
          static_cast<std::uint32_t>(item.item_id.Value() - 200000U);
    }
    // D242B0 -> 851180 also applies Ethi's +100 when updating a respawn preset.
    if (fighter == 7U && weapons[3]) weapons[3] = *weapons[3] + 100U;
    for (auto& weapon : weapons)
      if (weapon) weapon = domain::ModeWeaponTemplate(room_->ModeId(), *weapon);
    if (domain::ModeRequiresSniperPrimary(room_->ModeId()) &&
        (!weapons[0] || !domain::IsNativeSniperWeapon(*weapons[0]))) {
      return MakeError(application::ErrorCode::kConflict,
                       "狙击模式需要装备有效的狙击主武器");
    }
    auto updated = UpdatePlayerSnapshot(std::move(snapshot));
    if (const auto* error = std::get_if<application::ServiceError>(&updated)) {
      return *error;
    }
    static_cast<void>(combat_->ChangePreset(player, weapons, fighter,
                                            std::move(equipment_ids)));
    auto result = MakeResult(RoomEvent::kNoChange);
    const OutboundMessage equipment{
        .opcode = 806U,
        .body = protocol::EncodeEquippedItemsResponse(players_.at(player))};
    for (const auto& peer : room_->Members()) {
      if (peer.player_id != player) {
        result.deliveries.push_back(
            {.recipient = peer.player_id, .message = equipment});
      }
    }
    return result;
  }
  if (room_->Phase() != RoomPhase::kWaiting) {
    return MakeResult(RoomEvent::kNoChange);
  }
  auto updated = UpdatePlayerSnapshot(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&updated)) {
    return *error;
  }
  auto ready = room_->SetReady(player, false);
  if (const auto* error = std::get_if<application::ServiceError>(&ready)) {
    return *error;
  }
  if (player == room_->HostPlayerId()) {
    scene_.field_40 = players_.at(player).nickname;
  }
  auto result = MakeResult(RoomEvent::kNoChange);
  CancelUserHostAuthorization(result);
  const auto member = MemberUpdate(player);
  Broadcast(result, std::span{&member, 1U});
  const OutboundMessage equipment{
      .opcode = 806U,
      .body = protocol::EncodeEquippedItemsResponse(players_.at(player))};
  for (const auto& peer : room_->Members()) {
    if (peer.player_id != player) {
      result.deliveries.push_back(
          {.recipient = peer.player_id, .message = equipment});
    }
  }
  return result;
}

RoomCommandResult RoomCommandFlow::MakeResult(RoomEvent event) const {
  return RoomCommandResult{.event = event,
                           .deliveries = {},
                           .active_timers = room_->ActiveTimers(),
                           .combat_reject_reason = std::nullopt,
                           .settlement_command = std::nullopt};
}

application::ServiceResult<application::MatchSettlementCommand>
RoomCommandFlow::BuildSettlementCommand(
    application::MatchCompletionKind completion,
    std::chrono::steady_clock::time_point now) const {
  if (!active_match_.has_value() || now < active_match_->started_at) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "终局缺少有效的开局身份或单调时钟");
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - active_match_->started_at);
  constexpr auto maximum_elapsed = std::chrono::seconds{3600};
  if (elapsed > maximum_elapsed) {
    elapsed = maximum_elapsed;
  }
  std::vector<domain::MatchResultParticipantInput> participants;
  participants.reserve(active_match_->participants.size());
  for (const auto& [player_id, frozen] : active_match_->participants) {
    auto participant = frozen;
    if (combat_) {
      const auto current = combat_->Performance(player_id);
      if (current.has_value()) {
        participant.performance = current.value();
      }
      participant.surviving =
          combat_->LifeState(player_id).has_value() &&
          combat_->LifeState(player_id) != CombatLifeState::kDefeated;
    }
    if (objectives_) {
      participant.performance.objective_points = objectives_->Score(player_id);
      participant.performance.loot_lucci = objectives_->BonusLucci(player_id);
    }
    if (crazy_racing_)
      participant.performance.objective_points =
          crazy_racing_->Goals(player_id);
    if (zombie_pve_)
      participant.performance.pve_units = zombie_pve_->ScoreUnits(player_id);
    if (weapon_master_)
      participant.performance.objective_points =
          weapon_master_->Progress(player_id).experience;
    if (racing_ && racing_->Phase() != ExtremeRacingPhase::kWaiting) {
      const auto progress = racing_->Progress(player_id);
      participant.performance.objective_points = static_cast<std::uint32_t>(
          std::max(0, progress.finish_points + progress.coin_points));
      participant.racing_tie_break = progress.tie_break;
      participant.departed = progress.departed;
    }
    if (infection_) participant.result_team = infection_->Team(player_id);
    participants.push_back(participant);
  }

  auto built = application::BuildMatchSettlementCommand(
      {.match_id = active_match_->match_id,
       .mode_id = room_->ModeId(),
       .elapsed_seconds = static_cast<std::uint32_t>(elapsed.count()),
       .completion_kind = completion,
       .score_model = active_match_->score_model,
       .participants = std::move(participants),
       .decided_team =
           crazy_racing_ ? std::optional<std::uint8_t>{crazy_racing_->Winner()}
           : zombie_pve_
               ? std::optional<std::uint8_t>{zombie_pve_->Winner(true)}
           : infection_ ? std::optional<std::uint8_t>{infection_->Winner(true)}
           : pirate_    ? std::optional<std::uint8_t>{pirate_->Winner() == 0U
                                                          ? std::uint8_t{1U}
                                                          : pirate_->Winner()}
           : (room_->ModeId() == 19U || room_->ModeId() == 11U) && objectives_
               ? std::optional{objectives_->Winner()}
               : std::nullopt});
  if (hide_seek_ &&
      completion == application::MatchCompletionKind::kCompleted) {
    if (auto* command =
            std::get_if<application::MatchSettlementCommand>(&built)) {
      if (command->participants.empty() ||
          hide_seek_->Match().Phase() != HideSeekPhase::kResults)
        return MakeError(application::ErrorCode::kInvalidState,
                         "躲猫猫尚无权威任务结果");
      const auto rows =
          hide_seek_->Match()
              .Snapshot(command->participants.front().player_id.Value())
              .results;
      for (auto& participant : command->participants) {
        const auto row = std::ranges::find(rows, participant.player_id.Value(),
                                           &HideSeekScore::player);
        if (row == rows.end())
          return MakeError(application::ErrorCode::kInternal,
                           "躲猫猫终局缺少开局成员");
        participant.outcome = row->task_success ? domain::MatchOutcome::kWin
                                                : domain::MatchOutcome::kLoss;
        participant.departed = row->departed;
      }
    }
  }
  return built;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::ApplyRacingUpdate(RoomCommandResult result,
                                   ExtremeRacingUpdate update,
                                   std::chrono::steady_clock::time_point now) {
  Broadcast(result, update.messages);
  for (auto& message : update.host_messages)
    result.deliveries.push_back(
        {.recipient = room_->HostPlayerId(), .message = std::move(message)});
  PreserveCombatPerformance();
  if (update.completion == ExtremeRacingCompletion::kFiveStages) {
    auto finished =
        FinishMatch(application::MatchCompletionKind::kCompleted, now);
    if (auto* value = std::get_if<RoomCommandResult>(&finished))
      value->deliveries.insert(
          value->deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
    return finished;
  }
  auto advanced = AdvanceObjectives(now);
  if (const auto* error = std::get_if<application::ServiceError>(&advanced))
    return *error;
  auto objective_result = std::get<RoomCommandResult>(std::move(advanced));
  if (objective_result.settlement_command.has_value()) {
    objective_result.deliveries.insert(
        objective_result.deliveries.begin(),
        std::make_move_iterator(result.deliveries.begin()),
        std::make_move_iterator(result.deliveries.end()));
    objective_result.combat_reject_reason = result.combat_reject_reason;
    return objective_result;
  }
  result.deliveries.insert(
      result.deliveries.end(),
      std::make_move_iterator(objective_result.deliveries.begin()),
      std::make_move_iterator(objective_result.deliveries.end()));
  result.active_timers = std::move(objective_result.active_timers);
  return result;
}

bool RoomCommandFlow::PreserveCombatPerformance() noexcept {
  if (!active_match_.has_value() || !combat_) {
    return false;
  }
  bool changed = false;
  for (auto& [player_id, frozen] : active_match_->participants) {
    const auto current = combat_->Performance(player_id);
    if (current.has_value()) {
      const auto& previous = frozen.performance;
      changed = changed || previous.bubbles != current->bubbles ||
                previous.pangs != current->pangs ||
                previous.deaths != current->deaths ||
                previous.helps != current->helps;
      frozen.performance = current.value();
      frozen.surviving =
          combat_->LifeState(player_id) != CombatLifeState::kDefeated;
    }
    if (objectives_) {
      frozen.performance.objective_points = objectives_->Score(player_id);
      frozen.performance.loot_lucci = objectives_->BonusLucci(player_id);
    }
    if (zombie_pve_)
      frozen.performance.pve_units = zombie_pve_->ScoreUnits(player_id);
    if (crazy_racing_) {
      const auto goals = crazy_racing_->Goals(player_id);
      changed = changed || frozen.performance.objective_points != goals;
      frozen.performance.objective_points = goals;
    }
  }
  if (racing_ && racing_->Phase() != ExtremeRacingPhase::kWaiting) {
    for (auto& [player_id, frozen] : active_match_->participants) {
      const auto progress = racing_->Progress(player_id);
      const auto points = static_cast<std::uint32_t>(
          std::max(0, progress.finish_points + progress.coin_points));
      changed = changed || frozen.performance.objective_points != points ||
                frozen.racing_tie_break != progress.tie_break ||
                frozen.departed != progress.departed;
      frozen.performance.objective_points = points;
      frozen.racing_tie_break = progress.tie_break;
      frozen.departed = progress.departed;
    }
  }
  if (weapon_master_) {
    for (auto& [player_id, frozen] : active_match_->participants) {
      const auto progress = weapon_master_->Progress(player_id);
      const auto previous =
          active_match_->score_players.at(player_id).weapon_master;
      changed = changed || !previous ||
                previous->experience != progress.experience ||
                previous->assists != progress.assists;
      frozen.performance.objective_points = progress.experience;
      active_match_->score_players.at(player_id).weapon_master = progress;
    }
  }
  return changed;
}

void RoomCommandFlow::AppendScore(RoomCommandResult& result,
                                  std::uint8_t winner) const {
  if (!active_match_.has_value()) {
    throw std::logic_error{"native score requires a frozen match roster"};
  }
  std::vector<protocol::NativeScorePlayer> rows;
  rows.reserve(active_match_->score_players.size());
  for (const auto& [player_id, identity] : active_match_->score_players) {
    auto row = identity;
    if (zombie_pve_) row.team = zombie_pve_->Team(player_id);
    if (infection_) row.team = infection_->Team(player_id) == 2U ? 34U : 1U;
    row.performance = active_match_->participants.at(player_id).performance;
    row.surviving = active_match_->participants.at(player_id).surviving &&
                    !active_match_->participants.at(player_id).departed;
    rows.push_back(std::move(row));
  }
  const std::array messages{OutboundMessage{
      .opcode = 729U,
      .body = protocol::EncodeNativeScoreResponse(
          rows, winner,
          (room_->ModeId() == 19U || room_->ModeId() == 11U) && objectives_
              ? std::optional{objectives_->RoundScores()}
              : std::nullopt)}};
  Broadcast(result, messages);
}

void RoomCommandFlow::AppendMatchResult(
    RoomCommandResult& result,
    const application::MatchSettlementCommand& command) {
  if (!active_match_.has_value()) {
    throw std::logic_error{"native final score requires a frozen match roster"};
  }
  std::uint8_t winner = 0U;
  if (!domain::IndividualMatchScoreModel(active_match_->score_model) &&
      command.completion_kind == application::MatchCompletionKind::kCompleted) {
    for (const auto& participant : command.participants) {
      if (participant.outcome == domain::MatchOutcome::kWin) {
        winner = participant.team_code;
        break;
      }
    }
  }
  if (pirate_) {
    winner = pirate_->Winner() == 0U ? 1U : pirate_->Winner();
  }
  if (infection_) winner = infection_->Winner(true) == 2U ? 34U : 1U;
  if (zombie_pve_) winner = zombie_pve_->Winner(true) == 2U ? 2U : 35U;
  const std::array stop{OutboundMessage{
      .opcode = 705U,
      .body = protocol::EncodeMatchStopResponse(
          winner, room_->ModeId(), infection_ && infection_->Winner() == 0U)}};
  active_match_->winner = winner;
  Broadcast(result, stop);
  if (pirate_) {
    active_match_->winner = pirate_->Winner() == 0U ? 1U : pirate_->Winner();
    const std::array end{
        OutboundMessage{.opcode = static_cast<std::uint16_t>(
                            active_match_->winner == 2U ? 906U : 907U),
                        .body = {}}};
    Broadcast(result, end);
  }
  room_->AwaitCommittedRewards();
  result.active_timers = room_->ActiveTimers();
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::ApplySettlementResult(
    const application::MatchSettlementResult& settlement,
    std::chrono::steady_clock::time_point now) {
  if (!active_match_.has_value() ||
      active_match_->match_id != settlement.match_id ||
      active_match_->rewards_applied || room_->Phase() != RoomPhase::kResults) {
    return RoomCommandResult{};
  }
  if (pending_settlement_.has_value()) {
    return PendingJournalError();
  }
  if (settlement.mode_id != room_->ModeId() ||
      settlement.players.size() != active_match_->participants.size()) {
    return MakeError(application::ErrorCode::kInternal,
                     "奖励回执与开局名单或模式不一致");
  }
  auto scores = active_match_->score_players;
  auto profiles = players_;
  std::set<domain::PlayerId> seen;
  for (const auto& player : settlement.players) {
    const auto score = scores.find(player.player_id);
    if (score == scores.end() || !seen.insert(player.player_id).second ||
        player.reward.progression.level == 0U ||
        player.reward.progression.level > 1000U) {
      return MakeError(application::ErrorCode::kInternal,
                       "奖励回执玩家或等级非法");
    }
    score->second.reward = player.reward;
    const auto& expected =
        active_match_->participants.at(player.player_id).performance;
    const auto& actual = player.performance;
    if (std::array{expected.bubbles, expected.pangs, expected.deaths,
                   expected.helps, expected.objective_points} !=
            std::array{actual.bubbles, actual.pangs, actual.deaths,
                       actual.helps, actual.objective_points} ||
        player.reward.progression.experience !=
            player.reward.after.experience ||
        player.reward.after.lucci < 0 || player.reward.after.experience < 0) {
      return MakeError(application::ErrorCode::kInternal,
                       "奖励回执统计或余额与终局契约不一致");
    }
    score->second.performance =
        active_match_->participants.at(player.player_id).performance;
    score->second.surviving =
        active_match_->participants.at(player.player_id).surviving &&
        !active_match_->participants.at(player.player_id).departed;
    const auto profile = profiles.find(player.player_id);
    if (profile != profiles.end()) {
      const auto& progress = player.reward.progression;
      profile->second.progress = {.level = progress.level,
                                  .experience = progress.experience,
                                  .level_base = progress.level_base,
                                  .level_next = progress.level_next};
      profile->second.wallets.lucci = player.reward.after.lucci;
    }
  }
  std::vector<RoomDelivery> prize_wallet_updates;
  if (settlement.user_host_prize) {
    const auto& prize = *settlement.user_host_prize;
    std::set<domain::PlayerId> expected_wallets{prize.sponsor_player_id};
    for (const auto& [player, participant] : active_match_->participants) {
      static_cast<void>(participant);
      expected_wallets.insert(player);
    }
    if (!prize.sponsor_player_id.IsValid() ||
        prize.wallets.size() != expected_wallets.size())
      return MakeError(application::ErrorCode::kInternal,
                       "主办奖金钱包回执名单不完整");
    for (const auto& value : prize.wallets) {
      const auto& wallets = value.wallets;
      if (expected_wallets.erase(value.player_id) != 1U || wallets.lucci < 0 ||
          wallets.lucci > domain::kNativeSignedDwordMaximum ||
          wallets.bubble_cash < 0 ||
          wallets.bubble_cash > domain::kNativeSignedDwordMaximum ||
          wallets.treasure_bubble < 0 ||
          wallets.treasure_bubble > domain::kNativeSignedDwordMaximum)
        return MakeError(application::ErrorCode::kInternal,
                         "主办奖金钱包回执身份或余额非法");
      const auto profile = profiles.find(value.player_id);
      if (profile == profiles.end() || !room_->GetMember(value.player_id))
        continue;
      // 729保留可重放的普通奖励；105随后同步含奖金的真实钱包，不能把历史after当成总余额。
      profile->second.wallets = wallets;
      prize_wallet_updates.push_back(
          {.recipient = value.player_id,
           .message = {.opcode = 105U,
                       .body = protocol::EncodePlayerProfileResponse(
                           profile->second)}});
    }
  }
  std::vector<protocol::NativeScorePlayer> rows;
  rows.reserve(scores.size());
  for (const auto& [player_id, score] : scores) {
    static_cast<void>(player_id);
    rows.push_back(score);
  }
  std::vector<RoomDelivery> contribution_notices;
  std::set<domain::PlayerId> contribution_players;
  for (const auto& contribution : settlement.clan_contributions) {
    const auto player =
        std::ranges::find(settlement.players, contribution.player_id,
                          &application::MatchSettlementPlayerResult::player_id);
    if (player == settlement.players.end() || contribution.clan_id == 0U ||
        contribution.clan_id == 99U || contribution.points > 60U ||
        !contribution_players.insert(contribution.player_id).second ||
        (contribution.points == 0U) !=
            (player->effective_outcome == domain::MatchOutcome::kLoss))
      return MakeError(application::ErrorCode::kInternal,
                       "战队贡献回执与正常参赛名单不一致");
    if (!room_->GetMember(contribution.player_id)) continue;
    const auto reason =
        player->effective_outcome == domain::MatchOutcome::kWin    ? 11U
        : player->effective_outcome == domain::MatchOutcome::kDraw ? 12U
                                                                   : 13U;
    contribution_notices.push_back(
        {.recipient = contribution.player_id,
         .message = {
             .opcode = 25U,
             .body = protocol::EncodeClanPointsNotice(
                 contribution.clan_id, contribution.points, reason, false)}});
  }
  const std::array messages{
      OutboundMessage{
          .opcode = 729U,
          .body = protocol::EncodeNativeScoreResponse(
              rows, active_match_->winner,
              (room_->ModeId() == 19U || room_->ModeId() == 11U) && objectives_
                  ? std::optional{objectives_->RoundScores()}
                  : std::nullopt)},
      OutboundMessage{.opcode = 732U, .body = {0U, 0U}}};
  room_->BeginCommittedResults(now);
  auto result = MakeResult(RoomEvent::kNoChange);
  Broadcast(result, messages);
  result.deliveries.insert(
      result.deliveries.end(),
      std::make_move_iterator(contribution_notices.begin()),
      std::make_move_iterator(contribution_notices.end()));
  result.deliveries.insert(
      result.deliveries.end(),
      std::make_move_iterator(prize_wallet_updates.begin()),
      std::make_move_iterator(prize_wallet_updates.end()));
  for (const auto player : settlement.couple_daily_gift_players) {
    if (room_->GetMember(player))
      result.deliveries.push_back(
          {.recipient = player,
           .message = {.opcode = 65039U, .body = {1U, 0U, 0U, 0U}}});
  }
  active_match_->score_players = std::move(scores);
  players_ = std::move(profiles);
  active_match_->rewards_applied = true;
  return result;
}

bool RoomCommandFlow::OrdinaryScoreTargetReached() const noexcept {
  // TimeAttack PvP keeps the native room/display target, but that cached20 is
  // not an additional victory condition. Captain/weapon-master and dedicated
  // football/star/coin objectives retain their own target contracts.
  const auto mode = room_->ModeId();
  if (mode == 1U || mode == 3U || mode == 12U || mode == 13U || mode == 14U) {
    return false;
  }
  if (!active_match_.has_value() || active_match_->score_target == 0U ||
      active_match_->score_target > 10'000U) {
    return false;
  }
  const auto target = static_cast<std::uint64_t>(active_match_->score_target);
  if (active_match_->score_model == domain::MatchScoreModel::kIndividualPangs) {
    return std::ranges::any_of(
        active_match_->participants, [target](const auto& entry) {
          return static_cast<std::uint64_t>(entry.second.performance.pangs) >=
                 target;
        });
  }
  if (active_match_->score_model != domain::MatchScoreModel::kTeamPangs) {
    return false;
  }
  std::array<std::uint64_t, 2U> team_scores{};
  for (const auto& [player_id, participant] : active_match_->participants) {
    static_cast<void>(player_id);
    if (participant.team_code == 1U || participant.team_code == 2U) {
      team_scores[participant.team_code - 1U] += participant.performance.pangs;
    }
  }
  return std::ranges::any_of(
      team_scores, [target](std::uint64_t score) { return score >= target; });
}

std::optional<application::PlayerMenuSnapshot> RoomCommandFlow::PlayerSnapshot(
    domain::PlayerId player) const {
  const auto found = players_.find(player);
  if (found == players_.end() || !room_->GetMember(player)) return std::nullopt;
  return found->second;
}

void RoomCommandFlow::Broadcast(
    RoomCommandResult& result,
    std::span<const OutboundMessage> messages) const {
  for (const auto& member : room_->Members()) {
    for (const auto& message : messages) {
      result.deliveries.push_back(
          {.recipient = member.player_id, .message = message});
    }
  }
}

OutboundMessage RoomCommandFlow::MemberUpdate(
    domain::PlayerId player_id) const {
  const auto member = room_->GetMember(player_id);
  const auto player = players_.find(player_id);
  if (!member.has_value() || player == players_.end()) {
    throw std::logic_error{"房间成员与资料快照失去同步"};
  }
  protocol::SceneSessionSlot slot{
      .slot = static_cast<std::uint8_t>(member->seat),
      .team = member->team,
      .occupied = true,
      .locked = false,
      .ready = member->ready,
      .player = player->second};
  return {.opcode = 503U,
          .body = protocol::EncodeSceneMemberUpdateResponse(slot)};
}

protocol::SceneSessionSnapshot RoomCommandFlow::RoomSnapshot(
    domain::PlayerId selected) const {
  const auto members = room_->Members();
  const auto selected_member = room_->GetMember(selected);
  const auto host_member = room_->GetMember(room_->HostPlayerId());
  if (!selected_member.has_value() || !host_member.has_value()) {
    throw std::logic_error{"房间快照缺少本地玩家或房主"};
  }

  const auto locked = room_->LockedSeats();
  std::vector<protocol::SceneSessionSlot> slots;
  slots.reserve(room_->SessionSlotCount());
  for (std::uint16_t seat = 0U; seat < room_->SessionSlotCount(); ++seat) {
    const auto member = MemberBySeat(members, seat);
    if (!member.has_value()) {
      slots.push_back({.slot = static_cast<std::uint8_t>(seat),
                       .team = 255U,
                       .occupied = false,
                       .locked = std::ranges::binary_search(locked, seat),
                       .ready = false,
                       .player = std::nullopt});
      continue;
    }
    const auto player = players_.find(member->player_id);
    if (player == players_.end()) {
      throw std::logic_error{"房间快照缺少成员资料"};
    }
    slots.push_back({.slot = static_cast<std::uint8_t>(seat),
                     .team = member->team,
                     .occupied = true,
                     .locked = false,
                     .ready = member->ready,
                     .player = player->second});
  }

  auto scene = scene_;
  scene.occupants = static_cast<std::uint16_t>(room_->Participants().size());
  scene.observers =
      static_cast<std::uint16_t>(members.size() - scene.occupants);
  return {.scene = std::move(scene),
          .host_slot = host_member->seat,
          .selected_slot = selected_member->seat,
          .slots = std::move(slots),
          .include_equipment_records = true};
}

void RoomCommandFlow::AppendReturnToWaiting(RoomCommandResult& result,
                                            std::string_view notice) {
  ResetUserHostPrize();
  for (const auto& member : room_->Members()) {
    const auto player = players_.find(member.player_id);
    if (player == players_.end()) {
      throw std::logic_error{"返回房间时缺少成员资料"};
    }
    result.deliveries.push_back(
        {.recipient = member.player_id,
         .message = {.opcode = 119U,
                     .body = protocol::EncodeLeaveRoomResponse()}});
    result.deliveries.push_back(
        {.recipient = member.player_id,
         .message = {
             .opcode = 105U,
             .body = protocol::EncodePlayerProfileResponse(player->second)}});
    result.deliveries.push_back(
        {.recipient = member.player_id,
         .message = {.opcode = 112U,
                     .body = protocol::EncodeSceneSessionResponse(
                         RoomSnapshot(member.player_id))}});
    if (!notice.empty()) {
      result.deliveries.push_back(
          {.recipient = member.player_id,
           .message = {.opcode = 109U,
                       .body = protocol::EncodeNoticeResponse(
                           protocol::Utf8ToUtf16(notice))}});
    }
  }
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::LevelIconChanged(
    application::PlayerMenuSnapshot snapshot) {
  const auto member = room_->GetMember(snapshot.player_id);
  if (room_->Phase() != RoomPhase::kWaiting || !member) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前阶段不能修改等级图标");
  }
  const OutboundMessage message{
      .opcode = 973U,
      .body = protocol::EncodeLevelIconResponse(
          {.slot = static_cast<std::uint8_t>(member->seat),
           .team = member->team,
           .occupied = true,
           .locked = false,
           .ready = member->ready,
           .player = snapshot})};
  auto updated = UpdatePlayerSnapshot(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&updated))
    return *error;
  auto result = MakeResult(RoomEvent::kNoChange);
  Broadcast(result, std::span{&message, 1U});
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::CoupleChanged(
    application::PlayerMenuSnapshot snapshot) {
  const auto member = room_->GetMember(snapshot.player_id);
  if (!member || !players_.contains(snapshot.player_id))
    return MakeResult(RoomEvent::kNoChange);
  auto& current = players_.at(snapshot.player_id);
  application::SyncNativeTitleEquipmentFlags(current, snapshot);
  current.couple = std::move(snapshot.couple);
  // 949 updates only relationship fields. A full 511/503 during combat can
  // rebuild the actor and disrupt a running match; neither readiness nor gear
  // changes.
  const OutboundMessage update{
      .opcode = 949U,
      .body = protocol::EncodeCoupleSlotResponse(
          {.slot = static_cast<std::uint8_t>(member->seat),
           .team = member->team,
           .occupied = true,
           .locked = false,
           .ready = member->ready,
           .player = current})};
  auto result = MakeResult(RoomEvent::kNoChange);
  Broadcast(result, std::span{&update, 1U});
  return result;
}

}  // namespace bf::runtime
