#include "features/battle/match_settlement.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError InvalidCommand(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInvalidInput,
      .safe_message = std::string{"对局终局快照非法："} + std::string{detail}};
}

ServiceError ContractError(std::string_view detail) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::string{"结算事务返回无效状态："} +
                                      std::string{detail}};
}

ServiceError RewardError(domain::MatchRewardError error) {
  switch (error) {
    case domain::MatchRewardError::kUnsupportedVersion:
      return ContractError("奖励规则版本不受支持");
    case domain::MatchRewardError::kInvalidPolicyLimit:
      return ContractError("奖励规则上限非法");
    case domain::MatchRewardError::kInvalidRate:
      return ContractError("奖励倍率非法");
    case domain::MatchRewardError::kInvalidMode:
      return ContractError("奖励规则含非法模式");
    case domain::MatchRewardError::kInvalidMatchInput:
      return InvalidCommand("排名、人数或时长越界");
    case domain::MatchRewardError::kInvalidCurrentProgress:
      return ContractError("玩家当前进度或事务余额非法");
  }
  return ContractError("未知奖励规则错误");
}

[[nodiscard]] bool ValidCompletionKind(MatchCompletionKind value) noexcept {
  switch (value) {
    case MatchCompletionKind::kCompleted:
    case MatchCompletionKind::kInterrupted:
    case MatchCompletionKind::kAbandoned:
      return true;
  }
  return false;
}

[[nodiscard]] bool ValidOutcome(domain::MatchOutcome value) noexcept {
  switch (value) {
    case domain::MatchOutcome::kWin:
    case domain::MatchOutcome::kDraw:
    case domain::MatchOutcome::kLoss:
      return true;
  }
  return false;
}

}  // namespace

std::optional<ServiceError> ValidateMatchSettlementCommand(
    const MatchSettlementCommand& command) {
  if (command.room_owner && (command.room_owner->owner_epoch == 0U ||
                             command.room_owner->owner_epoch >
                                 static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max())))
    return InvalidCommand("invalid room ownership epoch");
  if (command.mode_id == 0U || command.elapsed_seconds > 3'600U ||
      command.participants.empty() || command.participants.size() > 36U ||
      !ValidCompletionKind(command.completion_kind)) {
    return InvalidCommand("模式、时长或参与者数量越界");
  }
  std::set<domain::PlayerId> player_ids;
  std::set<std::uint8_t> seats;
  for (const auto& participant : command.participants) {
    if (!participant.player_id.IsValid() || participant.rank == 0U ||
        participant.rank > command.participants.size() ||
        participant.team_code < 1U ||
        participant.team_code >
            ((command.mode_id == 13U || command.mode_id == 23U) ? 16U : 2U) ||
        !ValidOutcome(participant.outcome)) {
      return InvalidCommand("玩家标识、队伍、胜负或排名越界");
    }
    const auto unit_total = std::accumulate(
        participant.performance.pve_units.begin(),
        participant.performance.pve_units.end(), std::uint64_t{0U});
    if ((command.mode_id != 16U && unit_total != 0U) ||
        unit_total > participant.performance.pangs)
      return InvalidCommand("PVE类别次数不属于该模式或超过已确认击爆数");
    if (!player_ids.insert(participant.player_id).second ||
        !seats.insert(participant.seat_index).second) {
      return InvalidCommand("玩家或席位重复");
    }
  }
  return std::nullopt;
}

namespace {

bool SameBalances(const domain::CurrentRewardBalances& left,
                  const domain::CurrentRewardBalances& right) noexcept {
  return left.experience == right.experience && left.lucci == right.lucci &&
         left.rating == right.rating;
}

bool SamePerformance(const domain::MatchPerformance& left,
                     const domain::MatchPerformance& right) noexcept {
  return left.bubbles == right.bubbles && left.pangs == right.pangs &&
         left.deaths == right.deaths && left.helps == right.helps &&
         left.objective_points == right.objective_points &&
         left.loot_lucci == right.loot_lucci &&
         left.pve_units == right.pve_units;
}

bool SameProgress(const domain::RewardProgressSnapshot& left,
                  const domain::RewardProgressSnapshot& right) noexcept {
  return left.experience == right.experience && left.level == right.level &&
         left.level_base == right.level_base &&
         left.level_next == right.level_next;
}

bool SameAward(const domain::MatchRewardAward& left,
               const domain::MatchRewardAward& right) noexcept {
  return left.eligible == right.eligible &&
         left.experience == right.experience && left.lucci == right.lucci &&
         left.rating == right.rating &&
         left.effective_outcome == right.effective_outcome;
}

bool SameApplied(const domain::AppliedMatchReward& left,
                 const domain::AppliedMatchReward& right) noexcept {
  return SameBalances(left.before, right.before) &&
         SameBalances(left.after, right.after) &&
         SameAward(left.applied_award, right.applied_award) &&
         SameProgress(left.progression, right.progression);
}

ServiceResult<MatchSettlementPlayerResult> CalculatePlayer(
    const domain::MatchRewardPolicy& policy,
    const MatchSettlementCommand& command,
    const MatchSettlementParticipant& participant,
    const domain::CurrentRewardBalances& before, bool couple_bonus) {
  const domain::MatchRewardInput input{
      .mode_id = command.mode_id,
      .performance = participant.performance,
      .outcome = participant.outcome,
      .rank = participant.rank,
      .elapsed_seconds = command.elapsed_seconds,
      .starting_players =
          static_cast<std::uint32_t>(command.participants.size()),
      .departed = participant.departed,
      .completed = command.completion_kind == MatchCompletionKind::kCompleted,
  };
  const auto award_result = domain::CalculateMatchReward(policy, input);
  if (const auto* error =
          std::get_if<domain::MatchRewardError>(&award_result)) {
    return RewardError(*error);
  }
  auto award = std::get<domain::MatchRewardAward>(award_result);
  if (couple_bonus && award.eligible) {
    // Original diamond/LOVE ring: +50% earned XP and lucci. Picked pirate
    // coins and rating are not battle-reward bonuses. Integer fractions floor.
    award.experience = std::min(domain::kNativeSignedDwordMaximum,
                                award.experience + award.experience / 2);
    const auto base_lucci = std::max<std::int64_t>(
        0, award.lucci - participant.performance.loot_lucci);
    award.lucci = std::min(domain::kNativeSignedDwordMaximum,
                           award.lucci + base_lucci / 2);
  }
  const auto applied_result = domain::ApplyMatchReward(policy, before, award);
  if (const auto* error =
          std::get_if<domain::MatchRewardError>(&applied_result)) {
    return RewardError(*error);
  }
  return MatchSettlementPlayerResult{
      .player_id = participant.player_id,
      .performance = participant.performance,
      .effective_outcome = award.effective_outcome,
      .rank = participant.rank,
      .reward = std::get<domain::AppliedMatchReward>(applied_result),
  };
}

std::vector<const MatchSettlementParticipant*> SortedParticipants(
    const MatchSettlementCommand& command) {
  std::vector<const MatchSettlementParticipant*> result;
  result.reserve(command.participants.size());
  for (const auto& participant : command.participants) {
    result.push_back(&participant);
  }
  std::ranges::sort(result, {}, [](const auto* participant) {
    return participant->player_id;
  });
  return result;
}

ServiceResult<MatchSettlementResult> RecomputeResult(
    const MatchSettlementCommand& command,
    const domain::MatchRewardPolicy& policy,
    std::span<const LockedRewardBalances> balances,
    std::span<const domain::PlayerId> bonuses) {
  const auto participants = SortedParticipants(command);
  if (balances.size() != participants.size()) {
    return ContractError("锁定余额数量与参与者不一致");
  }
  MatchSettlementResult result{.match_id = command.match_id,
                               .mode_id = command.mode_id,
                               .elapsed_seconds = command.elapsed_seconds,
                               .completion_kind = command.completion_kind};
  result.players.reserve(participants.size());
  for (std::size_t index = 0; index < participants.size(); ++index) {
    if (balances[index].player_id != participants[index]->player_id) {
      return ContractError("锁定余额顺序或玩家标识与请求不一致");
    }
    auto player_result = CalculatePlayer(
        policy, command, *participants[index], balances[index].balances,
        std::ranges::find(bonuses, participants[index]->player_id) !=
            bonuses.end());
    if (const auto* error = std::get_if<ServiceError>(&player_result)) {
      return *error;
    }
    result.players.push_back(
        std::get<MatchSettlementPlayerResult>(std::move(player_result)));
  }
  return result;
}

bool SameResult(const MatchSettlementResult& left,
                const MatchSettlementResult& right) noexcept {
  if (!(left.match_id == right.match_id) || left.mode_id != right.mode_id ||
      left.elapsed_seconds != right.elapsed_seconds ||
      left.completion_kind != right.completion_kind ||
      left.players.size() != right.players.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.players.size(); ++index) {
    const auto& a = left.players[index];
    const auto& b = right.players[index];
    if (a.player_id != b.player_id ||
        !SamePerformance(a.performance, b.performance) ||
        a.effective_outcome != b.effective_outcome || a.rank != b.rank ||
        !SameApplied(a.reward, b.reward)) {
      return false;
    }
  }
  return true;
}

ServiceResult<MatchSettlementResult> ValidateReplay(
    const MatchSettlementCommand& command, const MatchSettlementClaim& claim) {
  if (!claim.completed_result.has_value()) {
    return ContractError("成功重放缺少历史结果");
  }
  const auto& replay = *claim.completed_result;
  std::vector<LockedRewardBalances> before;
  before.reserve(replay.players.size());
  for (const auto& player : replay.players) {
    before.push_back(LockedRewardBalances{.player_id = player.player_id,
                                          .balances = player.reward.before});
  }
  auto expected_result = RecomputeResult(command, claim.reward_policy, before,
                                         claim.couple_bonus_players);
  if (const auto* error = std::get_if<ServiceError>(&expected_result)) {
    return *error;
  }
  if (!SameResult(std::get<MatchSettlementResult>(expected_result), replay)) {
    return ContractError("历史结果与终局快照或奖励规则不一致");
  }
  return replay;
}

std::optional<ServiceError> ValidatePrizeReceipt(
    const MatchSettlementCommand& command, const UserHostPrizeReceipt& prize) {
  if (!prize.sponsor_player_id.IsValid() || prize.policy_version != 1U ||
      prize.amount < 1 || prize.amount > domain::kNativeSignedDwordMaximum ||
      prize.fee_amount != prize.amount / 10) {
    return ContractError("主办奖金身份、策略或金额非法");
  }
  std::set<domain::PlayerId> winners;
  std::set<std::uint8_t> teams;
  std::set<domain::PlayerId> wallet_players{prize.sponsor_player_id};
  for (const auto& participant : command.participants) {
    wallet_players.insert(participant.player_id);
    if (command.completion_kind == MatchCompletionKind::kCompleted &&
        !participant.departed &&
        participant.outcome == domain::MatchOutcome::kWin) {
      winners.insert(participant.player_id);
      teams.insert(participant.team_code);
    }
  }
  if (teams.size() > 1U || prize.refunded != winners.empty()) {
    return ContractError("主办奖金关闭状态与终局胜负不一致");
  }
  const auto recipients =
      prize.refunded ? std::set{prize.sponsor_player_id} : winners;
  if (prize.claims.size() != recipients.size() ||
      prize.wallets.size() != wallet_players.size()) {
    return ContractError("主办奖金债权或钱包名单不完整");
  }
  const auto net =
      prize.refunded ? prize.amount : prize.amount - prize.fee_amount;
  const auto count = static_cast<std::int64_t>(recipients.size());
  auto remainder = net % count;
  auto recipient = recipients.begin();
  for (const auto& claim : prize.claims) {
    const auto expected = net / count + (remainder > 0 ? 1 : 0);
    if (remainder > 0) --remainder;
    if (claim.player_id != *recipient++ || claim.amount != expected ||
        claim.credited_amount < 0 || claim.credited_amount > claim.amount ||
        claim.ticket_due != prize.refunded ||
        (claim.ticket_credited && !claim.ticket_due)) {
      return ContractError("主办奖金债权不符合已冻结分配策略");
    }
  }
  auto wallet_player = wallet_players.begin();
  for (const auto& receipt : prize.wallets) {
    const auto& wallets = receipt.wallets;
    if (receipt.player_id != *wallet_player++ || wallets.lucci < 0 ||
        wallets.bubble_cash < 0 || wallets.treasure_bubble < 0 ||
        wallets.lucci > domain::kNativeSignedDwordMaximum ||
        wallets.bubble_cash > domain::kNativeSignedDwordMaximum ||
        wallets.treasure_bubble > domain::kNativeSignedDwordMaximum) {
      return ContractError("主办奖金钱包身份、顺序或余额非法");
    }
  }
  return std::nullopt;
}

std::optional<ServiceError> ReadSupplementalReceipts(
    MatchSettlementTransaction& transaction,
    const MatchSettlementCommand& command, MatchSettlementResult& result) {
  auto contributions = transaction.ClanContributions();
  if (const auto* error = std::get_if<ServiceError>(&contributions))
    return *error;
  auto prize = transaction.UserHostPrize();
  if (const auto* error = std::get_if<ServiceError>(&prize)) return *error;
  auto receipt =
      std::get<std::optional<UserHostPrizeReceipt>>(std::move(prize));
  if (receipt) {
    if (const auto error = ValidatePrizeReceipt(command, *receipt))
      return error;
  }
  result.clan_contributions =
      std::get<std::vector<MatchClanContribution>>(std::move(contributions));
  result.user_host_prize = std::move(receipt);
  auto gifts = transaction.CoupleDailyGifts();
  if (const auto* error = std::get_if<ServiceError>(&gifts)) return *error;
  std::set<domain::PlayerId> seen;
  for (const auto player : std::get<std::vector<domain::PlayerId>>(gifts)) {
    if (!seen.insert(player).second ||
        !std::ranges::any_of(result.players, [&](const auto& p) {
          return p.player_id == player && p.reward.applied_award.eligible;
        }))
      return ContractError("情侣赠品回执玩家与有效结算不符");
  }
  result.couple_daily_gift_players =
      std::get<std::vector<domain::PlayerId>>(std::move(gifts));
  return std::nullopt;
}

}  // namespace

MatchSettlementService::MatchSettlementService(
    MatchSettlementUnitOfWork& unit_of_work) noexcept
    : unit_of_work_(unit_of_work) {}

ServiceResult<MatchSettlementResult> MatchSettlementService::Settle(
    const MatchSettlementCommand& command) const {
  if (const auto error = ValidateMatchSettlementCommand(command)) {
    return *error;
  }
  auto begin_result = unit_of_work_.Begin();
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<MatchSettlementTransaction>>(
      std::move(begin_result));
  if (!transaction) {
    return ContractError("事务工厂返回空对象");
  }

  auto claim_result = transaction->ClaimSettlement(command);
  if (const auto* error = std::get_if<ServiceError>(&claim_result)) {
    return *error;
  }
  const auto claim = std::get<MatchSettlementClaim>(std::move(claim_result));
  if (const auto policy_error =
          domain::ValidateMatchRewardPolicy(claim.reward_policy)) {
    return RewardError(*policy_error);
  }
  std::set<domain::PlayerId> bonus_seen;
  for (const auto player : claim.couple_bonus_players) {
    if (!bonus_seen.insert(player).second ||
        !std::ranges::any_of(command.participants,
                             [&](const auto& p) {
                               return p.player_id == player && !p.departed;
                             }) ||
        command.completion_kind != MatchCompletionKind::kCompleted)
      return ContractError("情侣加成名单与终局参与者不符");
  }
  if (claim.state == MatchSettlementClaimState::kCompletedReplay) {
    auto replay = ValidateReplay(command, claim);
    if (std::holds_alternative<ServiceError>(replay)) {
      return replay;
    }
    if (const auto error = ReadSupplementalReceipts(
            *transaction, command, std::get<MatchSettlementResult>(replay)))
      return *error;
    if (auto error = transaction->Commit()) {
      return *error;
    }
    return replay;
  }
  if (claim.state != MatchSettlementClaimState::kNew) {
    return ContractError("结算占用状态未知");
  }
  if (claim.completed_result.has_value()) {
    return ContractError("新结算携带历史结果");
  }

  const auto participants = SortedParticipants(command);
  std::vector<domain::PlayerId> player_ids;
  player_ids.reserve(participants.size());
  for (const auto* participant : participants) {
    player_ids.push_back(participant->player_id);
  }
  auto balances_result = transaction->LockRewardBalances(player_ids);
  if (const auto* error = std::get_if<ServiceError>(&balances_result)) {
    return *error;
  }
  const auto balances =
      std::get<std::vector<LockedRewardBalances>>(std::move(balances_result));
  auto result_value = RecomputeResult(command, claim.reward_policy, balances,
                                      claim.couple_bonus_players);
  if (const auto* error = std::get_if<ServiceError>(&result_value)) {
    return *error;
  }
  auto result = std::get<MatchSettlementResult>(std::move(result_value));
  for (const auto& player : result.players) {
    const MatchSettlementPlayerWrite write{
        .match_id = command.match_id,
        .mode_id = command.mode_id,
        .elapsed_seconds = command.elapsed_seconds,
        .result = player};
    if (auto error = transaction->ApplyPlayerSettlement(write)) {
      return *error;
    }
  }
  if (auto error = transaction->CompleteSettlement(result)) {
    return *error;
  }
  if (const auto error =
          ReadSupplementalReceipts(*transaction, command, result))
    return *error;
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return result;
}

}  // namespace bf::application
