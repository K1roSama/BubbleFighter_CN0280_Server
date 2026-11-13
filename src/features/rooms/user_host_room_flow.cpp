#include <algorithm>
#include <utility>
#include <variant>

#include "features/rooms/room_command_flow.h"
#include "protocol/inventory/inventory_messages.h"

namespace bf::runtime {
namespace {
using Status = protocol::UserHostStartStatus;
using Action = protocol::UserHostSettingAction;

application::ServiceError Rejected(application::ErrorCode code,
                                   const char* text) {
  return {.code = code, .safe_message = text};
}

bool TeamMode(std::uint32_t mode) {
  const auto model = domain::GetMatchScoreModel(mode);
  return model && (*model == domain::MatchScoreModel::kTeamPangs ||
                   *model == domain::MatchScoreModel::kTeamObjectives ||
                   *model == domain::MatchScoreModel::kTeamElimination ||
                   *model == domain::MatchScoreModel::kTeamRounds);
}
}  // namespace

std::optional<application::UserHostPrizeTerms> RoomCommandFlow::UserHostPrize()
    const {
  return frozen_user_host_prize_;
}

Status RoomCommandFlow::UserHostStatus(domain::PlayerId actor) const {
  if (!room_->UserHosted() || !timing_.user_host_prize.enabled ||
      actor != room_->HostPlayerId() || room_->Phase() != RoomPhase::kWaiting ||
      scene_.configuration.submode_id == 4U ||
      (scene_.configuration.field_8 & 0x80U) != 0U)
    return Status::kCannotStart;
  if (!TeamMode(room_->ModeId())) return Status::kNotTeamMode;
  // Existing soccer registration requires a fighter host. An observer must not
  // enter a paid match whose ball cannot be registered; ordinary soccer is
  // unchanged.
  if (room_->ModeId() == 11U) return Status::kCannotStart;
  const auto members = room_->Participants();
  const auto first =
      std::ranges::count(members, std::uint8_t{1U}, &RoomMemberSnapshot::team);
  if (first == 0 || static_cast<std::size_t>(first) * 2U != members.size())
    return Status::kUnequalTeams;
  const auto player = players_.find(actor);
  if (player == players_.end()) return Status::kCannotStart;
  if (!std::ranges::any_of(player->second.items, [](const auto& item) {
        return item.item_id.Value() == 101164U && item.quantity > 0 &&
               !item.expires_at;
      }))
    return Status::kMissingTicket;
  const auto& rules = timing_.user_host_prize;
  if (user_host_amount_ < rules.minimum_amount ||
      user_host_amount_ > rules.maximum_amount ||
      player->second.wallets.lucci < user_host_amount_)
    return Status::kInvalidPrize;
  return Status::kAccepted;
}

bool RoomCommandFlow::UserHostRosterMatches() const {
  if (!user_host_authorization_) return false;
  const auto members = room_->Members();
  const auto& frozen = user_host_authorization_->roster;
  if (members.size() != frozen.size()) return false;
  for (std::size_t i = 0; i < members.size(); ++i)
    if (members[i].player_id != frozen[i].player_id ||
        members[i].seat != frozen[i].seat || members[i].team != frozen[i].team)
      return false;
  return true;
}

RoomCommandResult RoomCommandFlow::UserHostReply(domain::PlayerId actor,
                                                 Status status) const {
  auto result = MakeResult(RoomEvent::kNoChange);
  result.deliveries.push_back(
      {actor, {997U, protocol::EncodeUserHostStartResponse(status)}});
  return result;
}

void RoomCommandFlow::CancelUserHostAuthorization(RoomCommandResult& result) {
  if (!user_host_authorization_) return;
  user_host_authorization_.reset();
  // The native protocol has no invented cancellation action. A real failure
  // plus invalidated server authorization prevents a late 503 from starting a
  // new match.
  const OutboundMessage message{
      997U, protocol::EncodeUserHostStartResponse(Status::kCannotStart)};
  Broadcast(result, std::span{&message, 1U});
}

void RoomCommandFlow::ResetUserHostPrize() noexcept {
  user_host_amount_ = 0U;
  user_host_authorization_.reset();
  frozen_user_host_prize_.reset();
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleUserHost(
    domain::PlayerId actor, const protocol::Packet& packet,
    std::chrono::steady_clock::time_point now) {
  if (packet.opcode == 2028U) {
    protocol::ValidateUserHostStartRequest(packet.body);
    const auto status = UserHostStatus(actor);
    if (status != Status::kAccepted) return UserHostReply(actor, status);
    if (user_host_authorization_ &&
        now < user_host_authorization_->expires_at && UserHostRosterMatches())
      return MakeResult(RoomEvent::kNoChange);  // Replays must not restart
                                                // client five-second clocks.
    const auto& rules = timing_.user_host_prize;
    user_host_authorization_ =
        UserHostAuthorization{.terms = {.sponsor_player_id = actor,
                                        .amount = user_host_amount_,
                                        .minimum_amount = rules.minimum_amount,
                                        .maximum_amount = rules.maximum_amount,
                                        .policy_version = 1U},
                              .roster = room_->Members(),
                              .eligible_at = now + std::chrono::seconds{5},
                              .expires_at = now + std::chrono::seconds{30}};
    auto result = MakeResult(RoomEvent::kNoChange);
    const OutboundMessage message{
        997U, protocol::EncodeUserHostStartResponse(Status::kAccepted)};
    Broadcast(result, std::span{&message, 1U});
    return result;
  }

  const auto request = protocol::DecodeUserHostSettingRequest(packet.body);
  if (!room_->UserHosted() || !timing_.user_host_prize.enabled)
    return Rejected(application::ErrorCode::kInvalidState,
                    "当前房间不是已启用的主办奖金房间");
  const bool host = actor == room_->HostPlayerId();
  const bool query = request.action == Action::kQueryAsGuest ||
                     request.action == Action::kQueryAsHost;
  if ((!query || request.action == Action::kQueryAsHost) && !host)
    return Rejected(application::ErrorCode::kPermissionDenied,
                    "只有当前主办者可以设置奖金或请求主办者状态");
  if (!query && room_->Phase() != RoomPhase::kWaiting)
    return Rejected(application::ErrorCode::kInvalidState,
                    "奖金已冻结，不能在开局或结果阶段修改");
  auto result = MakeResult(RoomEvent::kNoChange);
  if (!query && user_host_authorization_) {
    // Editing explicitly revokes the pending start; repeated identical
    // confirmations remain read-only so native text-change/confirm replays
    // cannot charge anything.
    if (request.action == Action::kSetAmount &&
        request.amount == user_host_amount_)
      return MakeResult(RoomEvent::kNoChange);
    CancelUserHostAuthorization(result);
  }
  const auto& rules = timing_.user_host_prize;
  if (request.action == Action::kSetAmount) {
    if (request.amount < rules.minimum_amount ||
        request.amount > rules.maximum_amount ||
        players_.at(actor).wallets.lucci < request.amount)
      return Rejected(application::ErrorCode::kInvalidInput,
                      "奖金金额超出配置范围或主办者余额不足");
    user_host_amount_ = request.amount;
  }
  const OutboundMessage message{998U, protocol::EncodeUserHostSettingResponse(
                                          {.action = request.action,
                                           .amount = user_host_amount_,
                                           .minimum = rules.minimum_amount,
                                           .maximum = rules.maximum_amount})};
  if (query)
    result.deliveries.push_back({actor, message});
  else
    Broadcast(result, std::span{&message, 1U});
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::AssetsCommitted(
    application::PlayerMenuSnapshot snapshot) {
  const auto player = snapshot.player_id;
  if (!room_->GetMember(player)) return MakeResult(RoomEvent::kNoChange);
  // Encode before replacing the cache: a malformed database result has no
  // partial effect.
  const auto profile = protocol::EncodePlayerProfileResponse(snapshot);
  const auto inventory = protocol::EncodeInventorySnapshotResponse(snapshot);
  auto updated = UpdatePlayerSnapshot(std::move(snapshot));
  if (const auto* error = std::get_if<application::ServiceError>(&updated))
    return *error;
  auto result = MakeResult(RoomEvent::kNoChange);
  result.deliveries.push_back({player, {105U, profile}});
  result.deliveries.push_back({player, {802U, inventory}});
  return result;
}
}  // namespace bf::runtime
