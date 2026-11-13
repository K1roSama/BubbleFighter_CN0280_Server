#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_REWARD_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_REWARD_H_

/**
 * @file match_reward.h
 * @brief 定义对局奖励资格、倍率、等级进度和资产封顶的纯领域规则。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace bf::domain {

inline constexpr std::int64_t kNativeSignedDwordMaximum =
    2'147'483'647LL;  ///< 旧客户端有符号 DWORD 上限。

/** @brief 终局中一名玩家的胜负结果。 */
enum class MatchOutcome : std::uint8_t {
  kWin,
  kDraw,
  kLoss,
};

/** @brief 奖励配置可引用的十种计分项倍率。 */
struct RewardRateLine final {
  std::int32_t finish{0};  ///< 完成有效对局的固定倍率。
  std::int32_t win{0};     ///< 胜利固定倍率。
  std::int32_t draw{0};    ///< 平局固定倍率。
  std::int32_t loss{0};    ///< 失败固定倍率。
  std::int32_t bubble{0};  ///< 每次泡封倍率。
  std::int32_t pang{0};    ///< 每次击爆倍率。
  std::int32_t help{0};    ///< 每次救援倍率。
  std::int32_t first{0};   ///< 第一名固定倍率。
  std::int32_t second{0};  ///< 第二名固定倍率。
  std::int32_t third{0};   ///< 第三名固定倍率。
};

/** @brief 单一模式只覆盖明确发布的倍率，空值沿用默认配置。 */
struct RewardRateOverride final {
  std::optional<std::int32_t> finish;  ///< 可选完成倍率。
  std::optional<std::int32_t> win;     ///< 可选胜利倍率。
  std::optional<std::int32_t> draw;    ///< 可选平局倍率。
  std::optional<std::int32_t> loss;    ///< 可选失败倍率。
  std::optional<std::int32_t> bubble;  ///< 可选泡封倍率。
  std::optional<std::int32_t> pang;    ///< 可选击爆倍率。
  std::optional<std::int32_t> help;    ///< 可选救援倍率。
  std::optional<std::int32_t> first;   ///< 可选第一名倍率。
  std::optional<std::int32_t> second;  ///< 可选第二名倍率。
  std::optional<std::int32_t> third;   ///< 可选第三名倍率。
};

/** @brief 一个模式对经验、金币和评级的稀疏倍率覆盖。 */
struct ModeRewardOverride final {
  std::optional<RewardRateOverride> experience;  ///< 经验倍率覆盖。
  std::optional<RewardRateOverride> lucci;       ///< 金币倍率覆盖。
  std::optional<RewardRateOverride> rating;      ///< 评级倍率覆盖。
};

/** @brief 一个正模式编号及其稀疏奖励覆盖；发布快照内必须按 mode_id 严格升序。
 */
struct ModeRewardRule final {
  std::uint32_t mode_id{0U};    ///< 大于零的客户端模式编号。
  ModeRewardOverride override;  ///< 该模式覆盖，未设置字段沿用默认倍率。
};

/**
 * @brief 一份不可变发布的奖励规则快照。
 * @note 当前字段精确对应旧 `rewards.json` version
 * 1；加载器必须在发布缓存中保留配置版本摘要。
 */
struct MatchRewardPolicy final {
  std::uint32_t version{1U};  ///< 结构版本，当前只支持 1。
  std::uint32_t minimum_seconds{
      0};  ///< 取得奖励所需最短有效时长，范围 0～3600。
  std::uint32_t minimum_players{1};  ///< 取得奖励所需开局玩家数，范围 1～36。
  std::uint32_t level_step{1};     ///< 三角等级阈值的基础步长，范围 1～100000。
  std::uint32_t maximum_level{1};  ///< 原生资料可表达的最大等级，范围 1～1000。
  RewardRateLine experience;       ///< 默认经验倍率；各项只能非负。
  RewardRateLine lucci;            ///< 默认金币倍率；各项只能非负。
  RewardRateLine rating;           ///< 默认评级倍率；允许 -10000～10000。
  std::vector<ModeRewardRule> modes;  ///< 按 mode_id 严格升序的稀疏覆盖。
};

/** @brief 奖励规则或终局输入违反固定约束时的稳定错误。 */
enum class MatchRewardError : std::uint8_t {
  kUnsupportedVersion,
  kInvalidPolicyLimit,
  kInvalidRate,
  kInvalidMode,
  kInvalidMatchInput,
  kInvalidCurrentProgress,
};

/** @brief 终局中服务端确认的累计统计；未知指标不得猜测填入。 */
struct MatchPerformance final {
  std::uint32_t bubbles{0U};  ///< 确认泡封次数。
  std::uint32_t pangs{0U};    ///< 确认击爆次数。
  std::uint32_t deaths{0U};   ///< 确认死亡次数；当前不直接产生奖励。
  std::uint32_t helps{0U};    ///< 确认救援次数。
  std::uint32_t loot_lucci{
      0U};  ///< 认证装备产生且实际拾取的额外金币；可靠结算一次入账。
  std::uint32_t objective_points{
      0U};  ///< 模式目标分；当前仅累计统计，不擅自折算奖励。
  std::array<std::uint32_t, 16U>
      pve_units{};  ///< 原生PVE类别次数，不能当作恢复出的加权分值。
};

/** @brief 计算一名玩家奖励所需的不可变终局事实。 */
struct MatchRewardInput final {
  std::uint32_t mode_id{0U};                  ///< 已发布且大于零的模式编号。
  MatchPerformance performance;               ///< 服务端确认的终局统计。
  MatchOutcome outcome{MatchOutcome::kDraw};  ///< 未离场时的权威胜负。
  std::uint32_t rank{1U};                     ///< 奖励排名；并列可共享名次。
  std::uint32_t elapsed_seconds{0U};          ///< 已夹紧到 0～3600 的对局时长。
  std::uint32_t starting_players{0U};         ///< 开局冻结玩家数，范围 1～36。
  bool departed{false};  ///< 是否在终局前离场；离场者不获奖励且统计记负。
  bool completed{true};  ///< interrupted/abandoned 为 false，不发奖励。
};

/** @brief 规则计算出的三种奖励增量和最终采用的胜负。 */
struct MatchRewardAward final {
  bool eligible{false};        ///< 是否满足完成、未离场、时长和人数条件。
  std::int64_t experience{0};  ///< 经验候选增量，非负且不超过原生上限。
  std::int64_t lucci{0};       ///< 金币候选增量，非负且不超过原生上限。
  std::int64_t rating{0};      ///< 评级候选增量，可为负并限制在原生范围。
  MatchOutcome effective_outcome{
      MatchOutcome::kDraw};  ///< 离场时强制为失败的统计结果。
};

/** @brief 资料响应需要的等级和相邻阈值快照。 */
struct RewardProgressSnapshot final {
  std::int64_t experience{0};  ///< 当前累计经验。
  std::uint32_t level{1U};     ///< 由阈值唯一推导的等级。
  std::int64_t level_base{0};  ///< 当前等级起始经验。
  std::int64_t level_next{1};  ///< 下一等级阈值；最大等级仍保留上界。
};

/** @brief 结算事务锁定玩家后读取的当前进度和金币。 */
struct CurrentRewardBalances final {
  std::int64_t experience{0};  ///< 结算前累计经验。
  std::int64_t lucci{0};       ///< 结算前金币。
  std::int64_t rating{0};      ///< 结算前评级。
};

/** @brief 将候选奖励应用封顶规则后的真实前后值及实际增量。 */
struct AppliedMatchReward final {
  CurrentRewardBalances before;        ///< 锁定读取的提交前值。
  CurrentRewardBalances after;         ///< 应在同一事务写入的提交后值。
  MatchRewardAward applied_award;      ///< 已按上限收窄的真实增量。
  RewardProgressSnapshot progression;  ///< 与 after.experience 一致的等级快照。
};

/** @brief 验证 version 1 奖励快照的范围、倍率和等级阈值。 */
[[nodiscard]] std::optional<MatchRewardError> ValidateMatchRewardPolicy(
    const MatchRewardPolicy& policy) noexcept;

/** @brief 按默认倍率及模式稀疏覆盖计算一名玩家的候选奖励。 */
[[nodiscard]] std::variant<MatchRewardAward, MatchRewardError>
CalculateMatchReward(const MatchRewardPolicy& policy,
                     const MatchRewardInput& input) noexcept;

/** @brief 从累计经验确定等级及相邻三角阈值；策略或经验非法时返回错误。 */
[[nodiscard]] std::variant<RewardProgressSnapshot, MatchRewardError>
CalculateRewardProgress(const MatchRewardPolicy& policy,
                        std::int64_t experience) noexcept;

/** @brief 将候选奖励应用到锁定值，按旧客户端范围截断并返回实际增量。 */
[[nodiscard]] std::variant<AppliedMatchReward, MatchRewardError>
ApplyMatchReward(const MatchRewardPolicy& policy,
                 const CurrentRewardBalances& before,
                 const MatchRewardAward& award) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_REWARD_H_
