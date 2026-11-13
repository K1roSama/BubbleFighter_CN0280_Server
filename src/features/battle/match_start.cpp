#include "features/battle/match_start.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError InvalidStart(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInvalidInput,
      .safe_message = std::string{"对局开局快照非法："} + std::string{detail}};
}

ServiceError ContractError(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInternal,
      .safe_message =
          std::string{"对局开局事务返回无效状态："} + std::string{detail}};
}

std::optional<ServiceError> Validate(const MatchStartCommand& command) {
  if (command.room_owner &&
      (command.room_owner->owner_epoch == 0U ||
       command.room_owner->owner_epoch > static_cast<std::uint64_t>(INT64_MAX)))
    return InvalidStart("房间所有权代次非法");
  if (!command.source_room_id.IsValid() || command.mode_id == 0U ||
      command.configuration_version_id <= 0 ||
      command.reward_rules_version_id <= 0 || command.participants.empty() ||
      command.participants.size() > 36U) {
    return InvalidStart("房间、模式、配置版本或参与者数量越界");
  }
  if (!domain::ValidMatchScoreModel(command.score_model)) {
    return InvalidStart("计分模型尚未恢复");
  }

  std::set<domain::PlayerId> player_ids;
  std::set<std::uint8_t> seats;
  for (const auto& participant : command.participants) {
    if (!participant.player_id.IsValid() ||
        ((command.mode_id == 13U || command.mode_id == 23U)
             ? (participant.team_code < 1U || participant.team_code > 16U)
             : (participant.team_code != 1U && participant.team_code != 2U))) {
      return InvalidStart("玩家标识或队伍非法");
    }
    if (!player_ids.insert(participant.player_id).second ||
        !seats.insert(participant.seat_index).second) {
      return InvalidStart("玩家或席位重复");
    }
  }
  if (command.user_host_prize) {
    const auto& prize = *command.user_host_prize;
    const bool team_mode =
        command.score_model == domain::MatchScoreModel::kTeamPangs ||
        command.score_model == domain::MatchScoreModel::kTeamObjectives ||
        command.score_model == domain::MatchScoreModel::kTeamElimination ||
        command.score_model == domain::MatchScoreModel::kTeamRounds;
    const auto first_team =
        std::ranges::count(command.participants, std::uint8_t{1U},
                           &MatchStartParticipant::team_code);
    if (!prize.sponsor_player_id.IsValid() || prize.policy_version != 1U ||
        !team_mode || prize.minimum_amount == 0U ||
        prize.minimum_amount > prize.maximum_amount ||
        prize.maximum_amount > domain::kNativeSignedDwordMaximum ||
        prize.amount < prize.minimum_amount ||
        prize.amount > prize.maximum_amount || first_team == 0 ||
        static_cast<std::size_t>(first_team) * 2U !=
            command.participants.size())
      return InvalidStart("主办比赛须使用有效奖金规则和人数相等的两队");
  }
  return std::nullopt;
}

}  // namespace

MatchStartService::MatchStartService(
    MatchStartUnitOfWork& unit_of_work) noexcept
    : unit_of_work_(unit_of_work) {}

ServiceResult<MatchStartResult> MatchStartService::Start(
    MatchStartCommand command) const {
  if (const auto error = Validate(command)) {
    return *error;
  }
  std::ranges::sort(command.participants, {},
                    &MatchStartParticipant::player_id);

  auto begun = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begun)) {
    return *error;
  }
  auto transaction =
      std::get<std::unique_ptr<MatchStartTransaction>>(std::move(begun));
  if (!transaction) {
    return ContractError("事务工厂返回空对象");
  }

  auto persisted = transaction->Persist(command);
  if (const auto* error = std::get_if<ServiceError>(&persisted)) {
    return *error;
  }
  const auto claim_state = std::get<MatchStartClaimState>(persisted);
  if (claim_state != MatchStartClaimState::kCreated &&
      claim_state != MatchStartClaimState::kExistingSame) {
    return ContractError("开局占用状态未知");
  }
  if (const auto error = transaction->Commit()) {
    return *error;
  }
  return MatchStartResult{.match_id = command.match_id,
                          .claim_state = claim_state,
                          .participant_count = command.participants.size()};
}

}  // namespace bf::application
