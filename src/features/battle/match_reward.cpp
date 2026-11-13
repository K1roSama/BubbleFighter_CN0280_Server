#include "features/battle/match_reward.h"

#include <algorithm>
#include <array>
#include <limits>

namespace bf::domain {
namespace {

using RateMember = std::int32_t RewardRateLine::*;
using OverrideMember = std::optional<std::int32_t> RewardRateOverride::*;

constexpr std::array<std::pair<RateMember, OverrideMember>, 10U> kRateMembers{{
    {&RewardRateLine::finish, &RewardRateOverride::finish},
    {&RewardRateLine::win, &RewardRateOverride::win},
    {&RewardRateLine::draw, &RewardRateOverride::draw},
    {&RewardRateLine::loss, &RewardRateOverride::loss},
    {&RewardRateLine::bubble, &RewardRateOverride::bubble},
    {&RewardRateLine::pang, &RewardRateOverride::pang},
    {&RewardRateLine::help, &RewardRateOverride::help},
    {&RewardRateLine::first, &RewardRateOverride::first},
    {&RewardRateLine::second, &RewardRateOverride::second},
    {&RewardRateLine::third, &RewardRateOverride::third},
}};

constexpr std::array<RateMember, 10U> kPlainRateMembers{
    &RewardRateLine::finish, &RewardRateLine::win,    &RewardRateLine::draw,
    &RewardRateLine::loss,   &RewardRateLine::bubble, &RewardRateLine::pang,
    &RewardRateLine::help,   &RewardRateLine::first,  &RewardRateLine::second,
    &RewardRateLine::third,
};

bool ValidRateLine(const RewardRateLine& rates, bool allow_negative) noexcept {
  return std::ranges::all_of(kPlainRateMembers, [&](RateMember member) {
    const auto value = rates.*member;
    const auto minimum = allow_negative ? -10'000 : 0;
    return value >= minimum && value <= 10'000;
  });
}

bool ValidOverride(const RewardRateOverride& rates,
                   bool allow_negative) noexcept {
  return std::ranges::all_of(kRateMembers, [&](const auto& members) {
    const auto& value = rates.*members.second;
    const auto minimum = allow_negative ? -10'000 : 0;
    return !value.has_value() || (*value >= minimum && *value <= 10'000);
  });
}

RewardRateLine MergeRates(RewardRateLine result,
                          const RewardRateOverride& overrides) noexcept {
  for (const auto& [rate_member, override_member] : kRateMembers) {
    const auto& value = overrides.*override_member;
    if (value.has_value()) {
      result.*rate_member = *value;
    }
  }
  return result;
}

std::int64_t Threshold(const MatchRewardPolicy& policy,
                       std::uint32_t level) noexcept {
  const auto value = static_cast<std::int64_t>(level);
  return static_cast<std::int64_t>(policy.level_step) * (value - 1) * value / 2;
}

std::int64_t SaturatedReward(const RewardRateLine& rates,
                             const MatchRewardInput& input,
                             MatchOutcome effective_outcome) noexcept {
  const std::int64_t finish = 1;
  const std::int64_t win = effective_outcome == MatchOutcome::kWin ? 1 : 0;
  const std::int64_t draw = effective_outcome == MatchOutcome::kDraw ? 1 : 0;
  const std::int64_t loss = effective_outcome == MatchOutcome::kLoss ? 1 : 0;
  const std::int64_t first = input.rank == 1U ? 1 : 0;
  const std::int64_t second = input.rank == 2U ? 1 : 0;
  const std::int64_t third = input.rank == 3U ? 1 : 0;
  const auto total =
      static_cast<std::int64_t>(rates.finish) * finish +
      static_cast<std::int64_t>(rates.win) * win +
      static_cast<std::int64_t>(rates.draw) * draw +
      static_cast<std::int64_t>(rates.loss) * loss +
      static_cast<std::int64_t>(rates.bubble) * input.performance.bubbles +
      static_cast<std::int64_t>(rates.pang) * input.performance.pangs +
      static_cast<std::int64_t>(rates.help) * input.performance.helps +
      static_cast<std::int64_t>(rates.first) * first +
      static_cast<std::int64_t>(rates.second) * second +
      static_cast<std::int64_t>(rates.third) * third;
  return std::clamp(total, -kNativeSignedDwordMaximum,
                    kNativeSignedDwordMaximum);
}

std::int64_t AddAndClamp(std::int64_t before, std::int64_t delta,
                         std::int64_t minimum, std::int64_t maximum) noexcept {
  if (delta > 0 && before > maximum - delta) {
    return maximum;
  }
  if (delta < 0 && before < minimum - delta) {
    return minimum;
  }
  return std::clamp(before + delta, minimum, maximum);
}

}  // namespace

std::optional<MatchRewardError> ValidateMatchRewardPolicy(
    const MatchRewardPolicy& policy) noexcept {
  if (policy.version != 1U) {
    return MatchRewardError::kUnsupportedVersion;
  }
  if (policy.minimum_seconds > 3'600U || policy.minimum_players == 0U ||
      policy.minimum_players > 36U || policy.level_step == 0U ||
      policy.level_step > 100'000U || policy.maximum_level == 0U ||
      policy.maximum_level > 1'000U ||
      Threshold(policy, policy.maximum_level + 1U) >
          kNativeSignedDwordMaximum) {
    return MatchRewardError::kInvalidPolicyLimit;
  }
  if (!ValidRateLine(policy.experience, false) ||
      !ValidRateLine(policy.lucci, false) ||
      !ValidRateLine(policy.rating, true)) {
    return MatchRewardError::kInvalidRate;
  }
  std::uint32_t previous_mode_id = 0U;
  for (const auto& rule : policy.modes) {
    if (rule.mode_id == 0U || rule.mode_id <= previous_mode_id) {
      return MatchRewardError::kInvalidMode;
    }
    previous_mode_id = rule.mode_id;
    const auto& mode = rule.override;
    if ((mode.experience.has_value() &&
         !ValidOverride(*mode.experience, false)) ||
        (mode.lucci.has_value() && !ValidOverride(*mode.lucci, false)) ||
        (mode.rating.has_value() && !ValidOverride(*mode.rating, true))) {
      return MatchRewardError::kInvalidRate;
    }
  }
  return std::nullopt;
}

std::variant<MatchRewardAward, MatchRewardError> CalculateMatchReward(
    const MatchRewardPolicy& policy, const MatchRewardInput& input) noexcept {
  if (const auto error = ValidateMatchRewardPolicy(policy)) {
    return *error;
  }
  if (input.mode_id == 0U || input.elapsed_seconds > 3'600U ||
      input.starting_players == 0U || input.performance.loot_lucci > 10'000U ||
      input.starting_players > 36U || input.rank == 0U ||
      input.rank > input.starting_players) {
    return MatchRewardError::kInvalidMatchInput;
  }

  auto experience_rates = policy.experience;
  auto lucci_rates = policy.lucci;
  auto rating_rates = policy.rating;
  const auto mode =
      std::ranges::find(policy.modes, input.mode_id, &ModeRewardRule::mode_id);
  if (mode != policy.modes.end()) {
    if (mode->override.experience.has_value()) {
      experience_rates =
          MergeRates(experience_rates, *mode->override.experience);
    }
    if (mode->override.lucci.has_value()) {
      lucci_rates = MergeRates(lucci_rates, *mode->override.lucci);
    }
    if (mode->override.rating.has_value()) {
      rating_rates = MergeRates(rating_rates, *mode->override.rating);
    }
  }

  const auto effective_outcome =
      input.departed ? MatchOutcome::kLoss : input.outcome;
  const bool eligible = input.completed && !input.departed &&
                        input.elapsed_seconds >= policy.minimum_seconds &&
                        input.starting_players >= policy.minimum_players;
  if (!eligible) {
    // Locally selected pirate rule: a normal finish cashes out verified picked
    // coins even in a solo/short match. Base XP/lucci/rating keep their policy.
    const bool pirate_loot =
        input.mode_id == 101U && input.completed && !input.departed;
    return MatchRewardAward{
        .eligible = false,
        .lucci = pirate_loot ? input.performance.loot_lucci : 0U,
        .effective_outcome = effective_outcome};
  }
  return MatchRewardAward{
      .eligible = true,
      .experience = SaturatedReward(experience_rates, input, effective_outcome),
      .lucci = std::min<std::int64_t>(
          kNativeSignedDwordMaximum,
          SaturatedReward(lucci_rates, input, effective_outcome) +
              input.performance.loot_lucci),
      .rating = SaturatedReward(rating_rates, input, effective_outcome),
      .effective_outcome = effective_outcome,
  };
}

std::variant<RewardProgressSnapshot, MatchRewardError> CalculateRewardProgress(
    const MatchRewardPolicy& policy, std::int64_t experience) noexcept {
  if (const auto error = ValidateMatchRewardPolicy(policy)) {
    return *error;
  }
  const auto maximum_experience =
      Threshold(policy, policy.maximum_level + 1U) - 1;
  if (experience < 0 || experience > maximum_experience) {
    return MatchRewardError::kInvalidCurrentProgress;
  }
  std::uint32_t level = 1U;
  while (level < policy.maximum_level &&
         experience >= Threshold(policy, level + 1U)) {
    ++level;
  }
  return RewardProgressSnapshot{.experience = experience,
                                .level = level,
                                .level_base = Threshold(policy, level),
                                .level_next = Threshold(policy, level + 1U)};
}

std::variant<AppliedMatchReward, MatchRewardError> ApplyMatchReward(
    const MatchRewardPolicy& policy, const CurrentRewardBalances& before,
    const MatchRewardAward& award) noexcept {
  if (const auto error = ValidateMatchRewardPolicy(policy)) {
    return *error;
  }
  const auto maximum_experience =
      Threshold(policy, policy.maximum_level + 1U) - 1;
  if (before.experience < 0 || before.experience > maximum_experience ||
      before.lucci < 0 || before.lucci > kNativeSignedDwordMaximum ||
      before.rating < 0 || before.rating > kNativeSignedDwordMaximum ||
      award.experience < 0 || award.lucci < 0 ||
      award.experience > kNativeSignedDwordMaximum ||
      award.lucci > kNativeSignedDwordMaximum ||
      award.rating < -kNativeSignedDwordMaximum ||
      award.rating > kNativeSignedDwordMaximum) {
    return MatchRewardError::kInvalidCurrentProgress;
  }

  CurrentRewardBalances after{
      .experience = AddAndClamp(before.experience, award.experience,
                                before.experience, maximum_experience),
      .lucci = AddAndClamp(before.lucci, award.lucci, before.lucci,
                           kNativeSignedDwordMaximum),
      .rating = AddAndClamp(before.rating, award.rating, 0,
                            kNativeSignedDwordMaximum),
  };
  auto progress_result = CalculateRewardProgress(policy, after.experience);
  if (const auto* error = std::get_if<MatchRewardError>(&progress_result)) {
    return *error;
  }
  auto applied = award;
  applied.experience = after.experience - before.experience;
  applied.lucci = after.lucci - before.lucci;
  applied.rating = after.rating - before.rating;
  return AppliedMatchReward{
      .before = before,
      .after = after,
      .applied_award = applied,
      .progression = std::get<RewardProgressSnapshot>(progress_result)};
}

}  // namespace bf::domain
