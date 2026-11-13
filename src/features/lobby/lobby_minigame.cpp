#include "features/lobby/lobby_minigame.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace bf::runtime {
namespace {

// Reply codes recovered from the client handlers (66D210, 66DD20/66E480,
// 66DF00, 66EB30).
constexpr std::uint32_t kBasketballBroken =
    0U;  // "농구 골대가 망가져서 공이 미끄러졌습니다"
constexpr std::uint32_t kBasketballPoor =
    2U;  // "루찌가 부족하여 게임을 할 수 없습니다"
constexpr std::uint32_t kBasketballMiss = 10U;       // animation 0
constexpr std::uint32_t kBasketballHit = 11U;        // animation 1
constexpr std::uint32_t kBasketballCleanShot = 12U;  // animation 2 plus item
constexpr std::uint32_t kFishingFrozen = 0U;  // pond frozen, bobber bounced
constexpr std::uint32_t kFishingNoFish = 2U;  // "물고기를 건지지 못했습니다"
constexpr std::uint32_t kFishingOverfished = 4U;  // daily limit
constexpr std::uint32_t kFishingPoor = 8U;
constexpr std::uint32_t kFishingEscaped = 9U;
constexpr std::uint32_t kFishingNoItem = 10U;
constexpr std::uint32_t kFishingOneItem = 11U;
constexpr std::uint32_t kFishingStarted =
    20U;  // 66DD20 starts the float for >= 20
constexpr std::uint32_t kFortuneClosed = 0U;  // "운세가 열리지 않았습니다"
constexpr std::uint32_t kFortunePoor = 2U;
constexpr std::uint32_t kFortuneLucky = 10U;
constexpr std::uint32_t kFortuneShown = 11U;

std::uint32_t ClampLucci(std::int64_t value) {
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
      value, 0, std::numeric_limits<std::int32_t>::max()));
}

bool Insufficient(const application::ServiceError& error) {
  return error.code == application::ErrorCode::kInsufficientBalance;
}

}  // namespace

protocol::LobbyFishingTerms SelectFishingTerms(
    const protocol::LobbyMinigameConfig::Fishing& config,
    const LobbyMinigameRecord& record) noexcept {
  // 67E930: with exactly one coupon, use the ordinary rod even when a premium
  // rod exists.
  const bool premium =
      config.premium_rod_item != 0U && record.premium_rod_quantity > 0 &&
      (config.free_pass_item == 0U || record.free_pass_quantity != 1);
  return {
      .premium_rod = premium,
      .use_free_pass = config.free_pass_item != 0U &&
                       record.free_pass_quantity >= (premium ? 2 : 1),
      .base_cost = premium ? config.alternate_cost_lucci : config.cost_lucci,
      .completed_count = record.fishing_count};
}

LobbyMinigameService::LobbyMinigameService(LobbyMinigameStore& store,
                                           LobbyMinigameRules rules,
                                           Random random)
    : store_(store), rules_(std::move(rules)), random_(std::move(random)) {}

std::optional<std::uint32_t> LobbyMinigameService::Draw(
    const std::vector<LobbyMinigameReward>& pool) {
  std::uint64_t total = 0U;
  for (const auto& reward : pool) total += reward.weight;
  if (total == 0U || total > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  auto pick = random_(static_cast<std::uint32_t>(total));
  for (const auto& reward : pool) {
    if (pick < reward.weight) return reward.item_id;
    pick -= reward.weight;
  }
  return std::nullopt;
}

application::ServiceResult<std::vector<std::uint8_t>>
LobbyMinigameService::Handle(domain::PlayerId player,
                             const protocol::LobbyMinigameRequest& request,
                             LobbyFishingSession& session) {
  const auto& config = rules_.config;
  switch (request.kind) {
    case protocol::LobbyMinigameRequestKind::kShootBasketball: {
      if (!config.basketball.enabled)
        return protocol::EncodeBasketballReply(kBasketballBroken, 0U, 0U);
      // The client decides the hit from its own gauge window (67F0C0); the
      // server owns the price and whether a hit becomes a rewarded clean shot.
      std::optional<std::uint32_t> reward;
      if (request.hit && !rules_.clean_shot_rewards.empty() &&
          random_(100U) < rules_.clean_shot_percent)
        reward = Draw(rules_.clean_shot_rewards);
      const auto result = !request.hit ? kBasketballMiss
                          : reward     ? kBasketballCleanShot
                                       : kBasketballHit;
      auto charged =
          store_.ShootBasketball(player, config.basketball.cost_lucci, reward);
      if (const auto* error = std::get_if<application::ServiceError>(&charged))
        return Insufficient(*error)
                   ? protocol::EncodeBasketballReply(kBasketballPoor, 0U, 0U)
                   : application::ServiceResult<std::vector<std::uint8_t>>{
                         *error};
      const auto& value = std::get<LobbyMinigameCharge>(charged);
      return protocol::EncodeBasketballReply(
          result, ClampLucci(value.lucci_after), reward.value_or(0U));
    }
    case protocol::LobbyMinigameRequestKind::kStartFishing: {
      session.active_terms.reset();
      if (!config.fishing.enabled)
        return protocol::EncodeStartFishingReply(kFishingFrozen);
      auto loaded = store_.Load(player, config.fishing);
      if (const auto* error = std::get_if<application::ServiceError>(&loaded))
        return *error;
      const auto& record = std::get<LobbyMinigameRecord>(loaded);
      if (record.fishing_count == std::numeric_limits<std::uint16_t>::max() ||
          (rules_.fishing_daily_limit != 0U &&
           record.fishing_count >= rules_.fishing_daily_limit))
        return protocol::EncodeStartFishingReply(kFishingOverfished);
      const auto terms = SelectFishingTerms(config.fishing, record);
      if (request.fishing != terms)
        return protocol::EncodeStartFishingReply(
            5U);  // Original cancellation, no charge.
      const auto price = terms.use_free_pass
                             ? 0
                             : static_cast<std::int64_t>(terms.base_cost) *
                                   (record.fishing_count + 1);
      if (record.lucci < price ||
          price > std::numeric_limits<std::int32_t>::max())
        return protocol::EncodeStartFishingReply(kFishingPoor);
      session.active_terms = terms;
      return protocol::EncodeStartFishingReply(kFishingStarted);
    }
    case protocol::LobbyMinigameRequestKind::kLiftFishingRod: {
      const auto active = std::exchange(session.active_terms, std::nullopt);
      if (!active || *active != request.fishing)
        return protocol::EncodeLiftFishingReply(kFishingNoFish, 0, 0U, 0U, {});
      if (!request.hooked)
        return protocol::EncodeLiftFishingReply(kFishingEscaped, 0, 0U, 0U, {});
      std::vector<std::uint32_t> rewards;
      // Preserve the configured server reward policy for each catch. The
      // original premium rod permits two results; no original private odds are
      // inferred from its description.
      for (unsigned catch_index = 0U;
           catch_index < (active->premium_rod ? 2U : 1U); ++catch_index) {
        if (!rules_.fishing_rewards.empty() &&
            random_(100U) < rules_.fishing_catch_percent) {
          if (const auto item = Draw(rules_.fishing_rewards))
            rewards.push_back(*item);
        }
      }
      auto charged = store_.CompleteFishing(
          player, config.fishing, *active, rules_.fishing_daily_limit, rewards);
      if (const auto* error =
              std::get_if<application::ServiceError>(&charged)) {
        if (Insufficient(*error))
          return protocol::EncodeLiftFishingReply(kFishingPoor, 0, 0U, 0U, {});
        if (error->code == application::ErrorCode::kConflict)
          return protocol::EncodeLiftFishingReply(kFishingOverfished, 0, 0U, 0U,
                                                  {});
        if (error->code == application::ErrorCode::kInvalidState)
          return protocol::EncodeLiftFishingReply(kFishingNoFish, 0, 0U, 0U,
                                                  {});
        return *error;
      }
      const auto& value = std::get<LobbyMinigameCharge>(charged);
      return protocol::EncodeLiftFishingReply(
          rewards.empty()        ? kFishingNoItem
          : rewards.size() == 2U ? 12U
                                 : kFishingOneItem,
          static_cast<std::int32_t>(value.paid), ClampLucci(value.lucci_after),
          value.fishing_count, rewards);
    }
    case protocol::LobbyMinigameRequestKind::kFortune: {
      if (!config.fortune.enabled)
        return protocol::EncodeFortuneReply(kFortuneClosed, 0U, 0U, 0U);
      // Result 10 always presents a lucky item (66EB30), so the daily charge
      // only exists together with a configured lucky-item pool; otherwise the
      // paper is shown (11).
      const auto lucky = Draw(rules_.fortune_lucky_items);
      const std::int64_t cost = lucky ? config.fortune.daily_cost_lucci : 0;
      auto outcome = store_.Fortune(player, request.fortune_paper, cost, lucky);
      if (const auto* error = std::get_if<application::ServiceError>(&outcome))
        return Insufficient(*error)
                   ? protocol::EncodeFortuneReply(kFortunePoor, 0U, 0U, 0U)
                   : application::ServiceResult<std::vector<std::uint8_t>>{
                         *error};
      const auto& value = std::get<LobbyFortuneOutcome>(outcome);
      if (value.created && lucky)
        return protocol::EncodeFortuneReply(
            kFortuneLucky, value.paper, ClampLucci(value.lucci_after), *lucky);
      return protocol::EncodeFortuneReply(kFortuneShown, value.paper, 0U, 0U);
    }
  }
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = "未知广场小游戏请求"};
}

}  // namespace bf::runtime
