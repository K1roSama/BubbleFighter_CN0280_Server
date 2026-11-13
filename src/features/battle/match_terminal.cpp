#include "features/battle/match_terminal.h"

#include <string>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

[[nodiscard]] ServiceError InvalidTerminal(std::string message) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::move(message),
                      .correlation_id = {},
                      .is_retryable = false};
}

[[nodiscard]] bool ValidCompletion(MatchCompletionKind completion) noexcept {
  return completion == MatchCompletionKind::kCompleted ||
         completion == MatchCompletionKind::kInterrupted ||
         completion == MatchCompletionKind::kAbandoned;
}

}  // namespace

ServiceResult<MatchSettlementCommand> BuildMatchSettlementCommand(
    const MatchTerminalFacts& facts) {
  if (facts.mode_id == 0U || facts.elapsed_seconds > 3600U ||
      !ValidCompletion(facts.completion_kind)) {
    return InvalidTerminal("对局终局模式、时长或完成类型无效");
  }

  const bool racing = facts.mode_id == 24U;
  for (const auto& participant : facts.participants) {
    if (participant.racing_tie_break.has_value() != racing)
      return InvalidTerminal(
          "极限竞速终局缺少同分值或非竞速局带有竞速排序字段");
  }
  const auto calculated = domain::CalculateMatchResult(
      {.score_model = facts.score_model,
       .completed = facts.completion_kind == MatchCompletionKind::kCompleted,
       .participants = facts.participants,
       .decided_team = facts.decided_team});
  const auto* result = std::get_if<domain::MatchResult>(&calculated);
  if (result == nullptr) {
    return InvalidTerminal("对局终局成员、席位、队伍或计分模型无效");
  }

  MatchSettlementCommand command{.match_id = facts.match_id,
                                 .mode_id = facts.mode_id,
                                 .elapsed_seconds = facts.elapsed_seconds,
                                 .completion_kind = facts.completion_kind,
                                 .participants = {}};
  command.participants.reserve(result->participants.size());
  for (const auto& participant : result->participants) {
    command.participants.push_back({.player_id = participant.player_id,
                                    .seat_index = participant.seat_index,
                                    .team_code = participant.team_code,
                                    .performance = participant.performance,
                                    .outcome = participant.outcome,
                                    .rank = participant.rank,
                                    .departed = participant.departed});
  }
  return command;
}

}  // namespace bf::application
