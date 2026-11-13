#ifndef BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_LOBBY_MINIGAME_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_LOBBY_MINIGAME_H_

/** @file lobby_minigame.h
 *  @brief 广场小游戏保留原生线格式；原服私有价格和奖池不能从客户端推断。
 *
 *  The client recovers the whole protocol and presentation; the original server
 * kept prices, odds and reward pools. Those live in `lobby_minigame.*` server
 * configuration keys and default to free play without item rewards. Charges and
 * rewards are only applied through the store in one MySQL transaction.
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "protocol/lobby/lobby_minigame_messages.h"

namespace bf::runtime {

/** @brief 奖励只允许数量叠加或永久单实例模板，权重不是客户端提供的概率。 */
struct LobbyMinigameReward final {
  std::uint32_t item_id{0U};
  std::uint32_t weight{0U};
};

/** @brief 价格和概率是服务端策略，config 必须保持 S2C1500 section 3
 * 的原生线布局。 */
struct LobbyMinigameRules final {
  protocol::LobbyMinigameConfig config{
      .basketball = {.enabled = true,
                     .cost_lucci = 0U,
                     .gauge_lead_ms = 1000U,
                     .hit_window_ms = 400U,
                     .round_end_ms = 11000U},
      .fishing = {.enabled = true,
                  .cost_lucci = 0U,
                  .alternate_cost_lucci = 0U,
                  .cast_ms = 1500U,
                  .bite_min_seconds = 3U,
                  .bite_max_seconds = 8U,
                  .hook_window_seconds = 2U,
                  .premium_rod_item = 0U,
                  .free_pass_item = 0U},
      .fortune = {.enabled = true, .daily_cost_lucci = 0U}};
  std::uint16_t fishing_daily_limit{
      0U};  ///< 0 = unlimited; reaching it answers code 4.
  std::uint32_t fishing_catch_percent{
      0U};  ///< Chance a hooked cast yields a reward.
  std::vector<LobbyMinigameReward>
      fishing_rewards;  ///< Weighted pool for a successful catch.
  std::uint32_t clean_shot_percent{
      0U};  ///< Chance a hit becomes a clean shot (result 12).
  std::vector<LobbyMinigameReward> clean_shot_rewards;
  std::vector<LobbyMinigameReward>
      fortune_lucky_items;  ///< Non-empty enables the paid result 10.
};

/** @brief 当日记录以服务器自然日为准，不能接受客户端重置次数。 */
struct LobbyMinigameRecord final {
  std::uint16_t fishing_count{0U};
  std::optional<std::uint64_t> fortune_paper;
  std::int64_t lucci{0};
  std::int64_t premium_rod_quantity{0};  ///< 当前有效高级鱼竿次数。
  std::int64_t free_pass_quantity{0};    ///< 当前有效免费券数量。
};

/** @brief 按原67E930选择鱼竿和用券，价格及次数均来自服务端快照。 */
[[nodiscard]] protocol::LobbyFishingTerms SelectFishingTerms(
    const protocol::LobbyMinigameConfig::Fishing& config,
    const LobbyMinigameRecord& record) noexcept;

/** @brief 余额必须来自已提交事务，不能按客户端回显费用自行推算。 */
struct LobbyMinigameCharge final {
  std::uint16_t fishing_count{0U};
  std::int64_t lucci_after{0};
  std::int64_t paid{0};
};

/** @brief 重复算命请求返回已存纸签；created 为 false 时不得重复扣费。 */
struct LobbyFortuneOutcome final {
  bool created{false};
  std::uint64_t paper{0U};
  std::int64_t lucci_after{0};
};

/** @brief 费用、奖励和按服务器自然日记账的记录必须在同一事务内提交。 */
class LobbyMinigameStore {
 public:
  virtual ~LobbyMinigameStore() = default;
  [[nodiscard]] virtual application::ServiceResult<LobbyMinigameRecord> Load(
      domain::PlayerId player,
      const protocol::LobbyMinigameConfig::Fishing& config) = 0;
  /** @brief
   * 锁内复核计费、扣券/鱼竿或金币，发奖并增加当日次数；任何失败全部回滚。 */
  [[nodiscard]] virtual application::ServiceResult<LobbyMinigameCharge>
  CompleteFishing(domain::PlayerId player,
                  const protocol::LobbyMinigameConfig::Fishing& config,
                  const protocol::LobbyFishingTerms& terms,
                  std::uint16_t daily_limit,
                  const std::vector<std::uint32_t>& rewards) = 0;
  [[nodiscard]] virtual application::ServiceResult<LobbyMinigameCharge>
  ShootBasketball(domain::PlayerId player, std::int64_t cost,
                  std::optional<std::uint32_t> reward) = 0;
  /** @brief Store today's paper once; later calls return the stored paper
   * without charging. */
  [[nodiscard]] virtual application::ServiceResult<LobbyFortuneOutcome> Fortune(
      domain::PlayerId player, std::uint64_t paper, std::int64_t cost,
      std::optional<std::uint32_t> lucky_item) = 0;
};

/** @brief 当前连接已确认的计费条件；未完成的抛竿不产生持久消费。 */
struct LobbyFishingSession final {
  std::optional<protocol::LobbyFishingTerms> active_terms;
};

/** @brief 业务失败保留原生 S2C571 结果，存储失败才升级为服务错误。 */
class LobbyMinigameService final {
 public:
  using Random = std::function<std::uint32_t(
      std::uint32_t bound)>;  ///< Uniform in [0, bound).
  LobbyMinigameService(LobbyMinigameStore& store, LobbyMinigameRules rules,
                       Random random);

  [[nodiscard]] const LobbyMinigameRules& Rules() const noexcept {
    return rules_;
  }

  [[nodiscard]] application::ServiceResult<LobbyMinigameRecord> Record(
      domain::PlayerId player) {
    return store_.Load(player, rules_.config.fishing);
  }

  [[nodiscard]] application::ServiceResult<std::vector<std::uint8_t>> Handle(
      domain::PlayerId player, const protocol::LobbyMinigameRequest& request,
      LobbyFishingSession& session);

 private:
  [[nodiscard]] std::optional<std::uint32_t> Draw(
      const std::vector<LobbyMinigameReward>& pool);
  LobbyMinigameStore& store_;
  LobbyMinigameRules rules_;
  Random random_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_LOBBY_MINIGAME_H_
