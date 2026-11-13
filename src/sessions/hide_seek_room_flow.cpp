#include <algorithm>
#include <iterator>
#include <stdexcept>

#include "features/rooms/room_command_flow.h"
#include "protocol/rooms/room_messages.h"

namespace bf::runtime {
namespace {
HideSeekTime Clock(std::chrono::steady_clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now.time_since_epoch())
      .count();
}

application::ServiceError Denied(const char* message) {
  return {.code = application::ErrorCode::kInvalidState,
          .safe_message = message};
}
}  // namespace

void RoomCommandFlow::AppendHideSeek(
    RoomCommandResult& result, std::vector<HideSeekDelivery> messages) const {
  for (auto& message : messages) {
    const domain::PlayerId player{message.recipient};
    if (room_->GetMember(player))
      result.deliveries.push_back(
          {player, {message.opcode, std::move(message.body)}});
  }
}

void RoomCommandFlow::SynchronizeHideSeekTeams(RoomCommandResult& result) {
  if (!hide_seek_ || room_->Phase() != RoomPhase::kPlaying) return;
  for (const auto& actor : hide_seek_->Match().Appearances()) {
    const domain::PlayerId player{actor.player};
    const auto member = room_->GetMember(player);
    if (actor.role != HideSeekRole::kHunter || !member || member->team == 2U)
      continue;
    const auto changed = room_->ConvertHideSeekMember(player);
    if (std::holds_alternative<application::ServiceError>(changed)) continue;
    if (combat_) {
      const auto row = combat_->actors_.find(player);
      if (row != combat_->actors_.end()) row->second.participant.team_code = 2U;
    }
    const OutboundMessage update{
        504U, protocol::EncodeRoomTeamChangedResponse({member->seat, 2U})};
    Broadcast(result, std::span{&update, 1U});
  }
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleHideSeek(
    domain::PlayerId player, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (!room_->IsHideSeek()) return Denied("该房间不是躲猫猫模式");
  try {
    const auto request =
        hide_seek_wire::decode_request(packet.body.data(), packet.body.size());
    if (request.command == hide_seek_wire::Command::Hello) {
      const auto member = room_->GetMember(player);
      const bool observer = room_->Phase() == RoomPhase::kPlaying && member &&
                            member->IsObserver() && hide_seek_;
      if (room_->Phase() != RoomPhase::kWaiting && !observer)
        return Denied("只能在等待房间协商躲猫猫版本");
      hide_seek_capabilities_[player] = request.value;
      hide_seek_wire::Writer response;
      response.word(hide_seek_wire::version);
      response.word(timing_.hide_seek_scene && timing_.hide_seek_scene->Valid()
                        ? hide_seek_wire::required_capabilities
                        : 0U);
      auto result = MakeResult(RoomEvent::kNoChange);
      result.deliveries.push_back(
          {player,
           {hide_seek_wire::capability_opcode, std::move(response.data)}});
      if (observer) {
        if ((request.value & hide_seek_wire::required_capabilities) !=
            hide_seek_wire::required_capabilities)
          return Denied("客户端尚未支持完整躲猫猫观战能力");
        // Repeated Hello must not restart loading or create another
        // subscription.
        if (!hide_seek_observer_loading_.contains(player)) {
          hide_seek_observer_loading_[player] = 0;
          result.deliveries.push_back(
              {player,
               {510U, protocol::EncodeStartSceneResponse(
                          {.map_key = scene_.configuration.map_key,
                           .mode_key = scene_.configuration.family_key,
                           .round_index = 0U,
                           .field_0 = 1})}});
        }
      }
      return result;
    }
    if (!hide_seek_ || room_->Phase() != RoomPhase::kPlaying)
      return Denied("躲猫猫回合尚未开始或已经结束");
    auto result = MakeResult(RoomEvent::kNoChange);
    AppendHideSeek(
        result, hide_seek_->Request(player.Value(), packet.body, Clock(now)));
    SynchronizeHideSeekTeams(result);
    if (hide_seek_->Match().Phase() != HideSeekPhase::kResults) {
      room_->SetObjectiveDeadline(std::chrono::steady_clock::time_point{
          std::chrono::milliseconds{hide_seek_->Match().NextDeadline()}});
      result.active_timers = room_->ActiveTimers();
      return result;
    }
    auto advanced = AdvanceHideSeek(now);
    if (auto* value = std::get_if<RoomCommandResult>(&advanced)) {
      value->deliveries.insert(
          value->deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
    }
    return advanced;
  } catch (const std::invalid_argument&) {
    return Denied("躲猫猫请求版本、回合、身份或负载无效");
  }
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::AdvanceHideSeek(
    std::chrono::steady_clock::time_point now) {
  auto result = MakeResult(RoomEvent::kNoChange);
  if (!hide_seek_ || room_->Phase() != RoomPhase::kPlaying) return result;
  for (const auto& actor : hide_seek_->Match().Appearances())
    if (actor.role != HideSeekRole::kDeparted &&
        !room_->GetMember(domain::PlayerId{actor.player}))
      AppendHideSeek(result, hide_seek_->Leave(actor.player, Clock(now)));
  AppendHideSeek(result, hide_seek_->Tick(Clock(now)));
  SynchronizeHideSeekTeams(result);
  if (hide_seek_->Match().Phase() == HideSeekPhase::kResults) {
    auto finished =
        FinishMatch(hide_seek_->Match().Winner() == HideSeekWinner::kAbandoned
                        ? application::MatchCompletionKind::kAbandoned
                        : application::MatchCompletionKind::kCompleted,
                    now);
    if (auto* value = std::get_if<RoomCommandResult>(&finished))
      value->deliveries.insert(
          value->deliveries.begin(),
          std::make_move_iterator(result.deliveries.begin()),
          std::make_move_iterator(result.deliveries.end()));
    return finished;
  }
  const auto due = hide_seek_->Match().NextDeadline();
  room_->SetObjectiveDeadline(
      std::chrono::steady_clock::time_point{std::chrono::milliseconds{due}});
  result.active_timers = room_->ActiveTimers();
  return result;
}
}  // namespace bf::runtime
