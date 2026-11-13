#include "features/battle/match_result.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <tuple>

namespace bf::domain {
namespace {

constexpr std::size_t kMaximumMatchParticipants = 36U;

[[nodiscard]] bool BetterPerformance(const MatchResultParticipantInput& lhs,
                                     const MatchResultParticipantInput& rhs,
                                     MatchScoreModel model) noexcept {
  if (lhs.racing_tie_break && rhs.racing_tie_break) {
    return BetterExtremeRacingOrder(
        {lhs.departed, lhs.performance.objective_points, *lhs.racing_tie_break,
         lhs.seat_index},
        {rhs.departed, rhs.performance.objective_points, *rhs.racing_tie_break,
         rhs.seat_index});
  }
  const bool objective = model == MatchScoreModel::kTeamObjectives ||
                         model == MatchScoreModel::kIndividualObjectives;
  if (model == MatchScoreModel::kIndividualElimination &&
      (lhs.surviving && !lhs.departed) != (rhs.surviving && !rhs.departed)) {
    return lhs.surviving && !lhs.departed;
  }
  const auto& left = lhs.performance;
  const auto& right = rhs.performance;
  const std::array left_positive{objective ? left.objective_points : left.pangs,
                                 left.bubbles, left.helps};
  const std::array right_positive{
      objective ? right.objective_points : right.pangs, right.bubbles,
      right.helps};
  if (left_positive != right_positive) {
    return left_positive > right_positive;
  }
  return left.deaths < right.deaths;
}

}  // namespace

bool BetterExtremeRacingOrder(const ExtremeRacingOrder& lhs,
                              const ExtremeRacingOrder& rhs) noexcept {
  // Do not negate signed totals: comparing them separately also avoids
  // INT64_MIN overflow.
  if (lhs.departed != rhs.departed) return !lhs.departed;
  if (lhs.total_points != rhs.total_points)
    return lhs.total_points > rhs.total_points;
  return std::tuple{lhs.tie_break, lhs.seat} <
         std::tuple{rhs.tie_break, rhs.seat};
}

std::optional<MatchScoreModel> GetMatchScoreModel(
    std::uint32_t mode_id) noexcept {
  switch (mode_id) {
    case 1U:
    case 3U:
    case 12U:
    case 14U:
    // Native 7FD310 uses the ordinary SniperModeMgr for both 12 and 18.
    // B4C590/8B8660 permit 18 to respawn; it is not Area32 elimination.
    case 18U:
    case 27U:
    // Captain: red/blue team deathmatch with one transformed captain per team.
    case 20U:
      return MatchScoreModel::kTeamPangs;
    case 13U:
      return MatchScoreModel::kIndividualPangs;
    case 5U:
    case 7U:
    // ExtremeRacing: finishing places plus picked coins over five stages.
    case 24U:
    case 26U:
      return MatchScoreModel::kIndividualObjectives;
    case 6U:
    case 21U:
      return MatchScoreModel::kTeamObjectives;
    case 25U:
      return MatchScoreModel::kTeamElimination;
    case 23U:
      return MatchScoreModel::kIndividualElimination;
    case 11U:
    case 19U:
      return MatchScoreModel::kTeamRounds;
    case 101U:
    case 102U:
    case 16U:
      return MatchScoreModel::kCooperative;
    case 8U:
    case 28U:
      return MatchScoreModel::kInfection;
    default:
      return std::nullopt;
  }
}

bool ValidMatchScoreModel(MatchScoreModel model) noexcept {
  return model >= MatchScoreModel::kTeamPangs &&
         model <= MatchScoreModel::kInfection;
}

bool IndividualMatchScoreModel(MatchScoreModel model) noexcept {
  return model == MatchScoreModel::kIndividualPangs ||
         model == MatchScoreModel::kIndividualObjectives ||
         model == MatchScoreModel::kIndividualElimination;
}

bool OrdinaryMatchRespawn(std::uint32_t mode_id) noexcept {
  // Pirate players use the native 704 -> 707 -> 32926 handshake. Cooperative
  // scoring does not disable their respawn; host-owned NPCs are gated
  // separately.
  if (mode_id == 11U || mode_id == 101U || mode_id == 102U) {
    return true;
  }
  const auto model = GetMatchScoreModel(mode_id);
  return model.has_value() && *model != MatchScoreModel::kTeamRounds &&
         *model != MatchScoreModel::kCooperative &&
         *model != MatchScoreModel::kInfection &&
         *model != MatchScoreModel::kTeamElimination &&
         *model != MatchScoreModel::kIndividualElimination;
}

std::variant<MatchResult, MatchResultError> CalculateMatchResult(
    const MatchResultInput& input) {
  if (!ValidMatchScoreModel(input.score_model) ||
      (input.decided_team.has_value() && *input.decided_team > 2U) ||
      (input.completed &&
       (input.score_model == MatchScoreModel::kTeamRounds ||
        input.score_model == MatchScoreModel::kCooperative ||
        input.score_model == MatchScoreModel::kInfection) &&
       !input.decided_team.has_value())) {
    return MatchResultError::kInvalidScoreModel;
  }
  if (input.participants.empty()) {
    return MatchResultError::kNoParticipants;
  }
  if (input.participants.size() > kMaximumMatchParticipants) {
    return MatchResultError::kTooManyParticipants;
  }

  const bool racing =
      std::ranges::any_of(input.participants, [](const auto& participant) {
        return participant.racing_tie_break.has_value();
      });
  if (racing &&
      (input.score_model != MatchScoreModel::kIndividualObjectives ||
       !std::ranges::all_of(input.participants, [](const auto& participant) {
         return participant.racing_tie_break &&
                *participant.racing_tie_break >= 0;
       }))) {
    return MatchResultError::kInvalidScoreModel;
  }
  std::set<PlayerId> players;
  std::set<std::uint8_t> seats;
  for (const auto& participant : input.participants) {
    if (input.score_model == MatchScoreModel::kInfection &&
        (!participant.result_team ||
         (*participant.result_team != 1U && *participant.result_team != 2U))) {
      return MatchResultError::kInvalidTeam;
    }
    if (!participant.player_id.IsValid()) {
      return MatchResultError::kInvalidPlayer;
    }
    if (!players.insert(participant.player_id).second) {
      return MatchResultError::kDuplicatePlayer;
    }
    if (!seats.insert(participant.seat_index).second) {
      return MatchResultError::kDuplicateSeat;
    }
    if (!IndividualMatchScoreModel(input.score_model) &&
        participant.team_code != 1U && participant.team_code != 2U) {
      return MatchResultError::kInvalidTeam;
    }
  }

  MatchResult result;
  result.participants.reserve(input.participants.size());
  for (const auto& participant : input.participants) {
    const auto rank = static_cast<std::uint32_t>(
        1U + std::ranges::count_if(input.participants, [&](const auto& other) {
          return BetterPerformance(other, participant, input.score_model);
        }));
    result.participants.push_back({.player_id = participant.player_id,
                                   .seat_index = participant.seat_index,
                                   .team_code = participant.team_code,
                                   .performance = participant.performance,
                                   .outcome = MatchOutcome::kDraw,
                                   .rank = rank,
                                   .departed = participant.departed});
  }

  if (input.completed) {
    if (!IndividualMatchScoreModel(input.score_model)) {
      std::array<std::uint64_t, 2U> scores{};
      for (const auto& participant : input.participants) {
        scores[participant.team_code - 1U] +=
            input.score_model == MatchScoreModel::kTeamObjectives
                ? participant.performance.objective_points
            : input.score_model == MatchScoreModel::kTeamElimination
                ? static_cast<std::uint32_t>(participant.surviving &&
                                             !participant.departed)
                : participant.performance.pangs;
      }
      if (input.score_model == MatchScoreModel::kTeamObjectives &&
          input.decided_team) {
        result.winning_team = *input.decided_team;
      } else if (input.score_model == MatchScoreModel::kTeamElimination) {
        result.winning_team = (scores[0] > 0U) != (scores[1] > 0U)
                                  ? (scores[0] > 0U ? 1U : 2U)
                                  : 0U;
      } else if (input.score_model == MatchScoreModel::kTeamRounds ||
                 input.score_model == MatchScoreModel::kCooperative ||
                 input.score_model == MatchScoreModel::kInfection) {
        result.winning_team = *input.decided_team;
      } else if (scores[0] != scores[1]) {
        result.winning_team = scores[0] > scores[1] ? 1U : 2U;
      }
      if (result.winning_team != 0U) {
        for (auto& participant : result.participants) {
          const auto source =
              std::ranges::find(input.participants, participant.player_id,
                                &MatchResultParticipantInput::player_id);
          const auto final_team =
              input.score_model == MatchScoreModel::kInfection
                  ? *source->result_team
                  : participant.team_code;
          participant.outcome = final_team == result.winning_team
                                    ? MatchOutcome::kWin
                                    : MatchOutcome::kLoss;
        }
      }
    } else if (racing) {
      // Native standings have a stable seat fallback, so even equal score/time
      // has one first place. Departed participants can never win a completed
      // race.
      for (auto& participant : result.participants)
        participant.outcome = participant.rank == 1U && !participant.departed
                                  ? MatchOutcome::kWin
                                  : MatchOutcome::kLoss;
    } else if (input.score_model == MatchScoreModel::kIndividualElimination) {
      const auto alive =
          std::ranges::count_if(input.participants, [](const auto& player) {
            return player.surviving && !player.departed;
          });
      for (auto& player : result.participants) {
        const auto source =
            std::ranges::find(input.participants, player.player_id,
                              &MatchResultParticipantInput::player_id);
        player.outcome =
            source->surviving && !source->departed
                ? (alive == 1 ? MatchOutcome::kWin : MatchOutcome::kDraw)
                : MatchOutcome::kLoss;
      }
    } else {
      const auto score = [model = input.score_model](const auto& player) {
        return model == MatchScoreModel::kIndividualObjectives
                   ? player.performance.objective_points
                   : player.performance.pangs;
      };
      const auto maximum =
          std::ranges::max_element(input.participants, {}, score);
      const auto maximum_pangs = score(*maximum);
      const auto leader_count = std::ranges::count_if(
          input.participants, [&](const auto& participant) {
            return score(participant) == maximum_pangs;
          });
      for (auto& participant : result.participants) {
        if (score(participant) == maximum_pangs) {
          participant.outcome =
              leader_count == 1 ? MatchOutcome::kWin : MatchOutcome::kDraw;
        } else {
          participant.outcome = MatchOutcome::kLoss;
        }
      }
    }
  }

  std::ranges::sort(result.participants, {},
                    &MatchResultParticipant::player_id);
  return result;
}

}  // namespace bf::domain
