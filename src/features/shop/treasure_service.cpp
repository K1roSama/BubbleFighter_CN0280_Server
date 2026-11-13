#include "features/shop/treasure_service.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError InvalidInput(std::string message) {
  return {.code = ErrorCode::kInvalidInput, .safe_message = std::move(message)};
}

ServiceError ContractError(std::string message) {
  return {.code = ErrorCode::kInternal,
          .safe_message = "泡泡抽取事务返回无效状态：" + std::move(message)};
}

bool ValidPositions(const std::array<std::uint32_t, 2U>& positions) noexcept {
  return positions[0] <= 4U && positions[1] <= 4U &&
         positions[0] != positions[1];
}

bool ValidDraw(const TreasureDrawSnapshot& draw, domain::PlayerId player_id) {
  if (draw.draw_id == 0U || draw.player_id != player_id ||
      !draw.needle_item_id.IsValid() || !ValidPositions(draw.positions) ||
      draw.selection_duration <= std::chrono::seconds::zero() ||
      draw.rules_version_id == 0U) {
    return false;
  }
  std::set<std::uint64_t> stocks;
  return std::ranges::all_of(draw.rewards, [&](const auto& reward) {
    return reward.stock_id.IsValid() && reward.item_id.IsValid() &&
           reward.quantity > 0U &&
           stocks.insert(reward.stock_id.Value()).second;
  });
}

ServiceResult<PublishedServiceRules> LoadRules(ServiceRulesLoader& loader) {
  auto rules = loader.Load();
  if (const auto* error = std::get_if<ServiceError>(&rules)) {
    return *error;
  }
  return std::get<PublishedServiceRules>(std::move(rules));
}

}  // namespace

TreasureService::TreasureService(const SystemClock& clock,
                                 ServiceRulesLoader& rules,
                                 TreasureRandomSource& random,
                                 TreasureRepository& repository) noexcept
    : clock_(clock), rules_(rules), random_(random), repository_(repository) {}

ServiceResult<TreasureStateSnapshot> TreasureService::State(
    domain::PlayerId player_id) const {
  if (!player_id.IsValid()) {
    return InvalidInput("玩家标识无效");
  }
  auto rules_result = LoadRules(rules_);
  if (const auto* error = std::get_if<ServiceError>(&rules_result)) {
    return *error;
  }
  const auto& rules = std::get<PublishedServiceRules>(rules_result);
  auto state_result =
      repository_.LoadState(player_id, rules.bubbles, clock_.Now());
  if (const auto* error = std::get_if<ServiceError>(&state_result)) {
    return *error;
  }
  auto snapshot = std::get<TreasureStateSnapshot>(std::move(state_result));
  if (snapshot.player_id != player_id ||
      snapshot.needle_counts.size() != rules.bubbles.needles.size()) {
    return ContractError("状态归属或魔法针数量不一致");
  }
  for (std::size_t index = 0U; index < snapshot.needle_counts.size(); ++index) {
    if (snapshot.needle_counts[index].item_id !=
        rules.bubbles.needles[index].item_id) {
      return ContractError("状态魔法针顺序与发布规则不一致");
    }
  }
  if (snapshot.pending_draw.has_value() &&
      !ValidDraw(*snapshot.pending_draw, player_id)) {
    return ContractError("待领取抽取快照无效");
  }
  return snapshot;
}

ServiceResult<std::pair<TreasureDrawSnapshot, bool>> TreasureService::Open(
    domain::OperationId operation_id, domain::PlayerId player_id,
    domain::ItemId needle_item_id, std::uint32_t primary_position,
    std::span<const std::uint32_t> positions) const {
  if (!player_id.IsValid() || !needle_item_id.IsValid() ||
      primary_position > 4U || positions.size() > 5U) {
    return InvalidInput("魔法针或泡泡位置无效");
  }
  auto rules_result = LoadRules(rules_);
  if (const auto* error = std::get_if<ServiceError>(&rules_result)) {
    return *error;
  }
  const auto& rules = std::get<PublishedServiceRules>(rules_result);
  const auto needle = std::ranges::find(rules.bubbles.needles, needle_item_id,
                                        &BubbleNeedleRule::item_id);
  if (needle == rules.bubbles.needles.end()) {
    return InvalidInput("魔法针未在发布规则中启用");
  }

  std::array<std::uint32_t, 2U> selected_positions{};
  if (positions.empty()) {
    selected_positions = {primary_position, (primary_position + 1U) % 5U};
  } else if (positions.size() == selected_positions.size()) {
    std::ranges::copy(positions, selected_positions.begin());
  } else {
    return InvalidInput("请选择两个不同的泡泡");
  }
  if (!ValidPositions(selected_positions)) {
    return InvalidInput("请选择两个不同的泡泡");
  }

  auto indices_result =
      random_.SampleWithoutReplacement(rules.bubbles.reward_pool.size(), 5U);
  if (const auto* error = std::get_if<ServiceError>(&indices_result)) {
    return *error;
  }
  const auto indices =
      std::get<std::vector<std::size_t>>(std::move(indices_result));
  if (indices.size() != 5U) {
    return ContractError("随机源没有返回五个索引");
  }
  std::array<ServiceRewardRule, 5U> rewards{};
  std::set<std::size_t> unique_indices;
  for (std::size_t index = 0U; index < rewards.size(); ++index) {
    if (indices[index] >= rules.bubbles.reward_pool.size() ||
        !unique_indices.insert(indices[index]).second) {
      return ContractError("随机源返回越界或重复索引");
    }
    rewards[index] = rules.bubbles.reward_pool[indices[index]];
  }
  auto result =
      repository_.Open({.operation_id = operation_id,
                        .player_id = player_id,
                        .rules_version_id = rules.configuration_version_id,
                        .needle = *needle,
                        .positions = selected_positions,
                        .rewards = rewards,
                        .selection_duration = rules.bubbles.selection_duration,
                        .now = clock_.Now()});
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  const auto& opened = std::get<std::pair<TreasureDrawSnapshot, bool>>(result);
  if (!ValidDraw(opened.first, player_id)) {
    return ContractError("抽取结果快照无效");
  }
  return result;
}

ServiceResult<TreasureClaimResult> TreasureService::Claim(
    domain::OperationId operation_id, domain::PlayerId player_id,
    std::uint32_t choice, std::uint32_t position) const {
  if (!player_id.IsValid() || choice > 1U || position > 4U) {
    return InvalidInput("泡泡奖励选择无效");
  }
  auto result = repository_.Claim({.operation_id = operation_id,
                                   .player_id = player_id,
                                   .choice = choice,
                                   .position = position,
                                   .now = clock_.Now()});
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  const auto& claimed = std::get<TreasureClaimResult>(result);
  if (claimed.player_id != player_id || claimed.draw_id == 0U ||
      claimed.position != position || !claimed.granted_item_id.IsValid() ||
      !std::ranges::all_of(
          claimed.stocks, [](const auto kStock) { return kStock.IsValid(); })) {
    return ContractError("领取结果与命令不一致");
  }
  return result;
}

}  // namespace bf::application
