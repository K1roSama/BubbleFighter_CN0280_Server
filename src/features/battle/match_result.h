#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_RESULT_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_RESULT_H_

/**
 * @file match_result.h
 * @brief 定义普通击爆计分对局的终局胜负、并列名次和结算参与者纯规则。
 */

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "application/identifier.h"
#include "features/battle/match_reward.h"

namespace bf::domain {

/**
 * @brief 已恢复流程的明确计分模型；目标模式不得套用普通击爆胜负。
 */
enum class MatchScoreModel : std::uint8_t {
  kTeamPangs,
  kIndividualPangs,
  kTeamObjectives,
  kIndividualObjectives,
  kTeamElimination,
  kIndividualElimination,
  kTeamRounds,
  kCooperative,
  kInfection,
};

/** @brief 返回旧服务已恢复目标规则的模式计分类型；未知目标不开放。 */
[[nodiscard]] std::optional<MatchScoreModel> GetMatchScoreModel(
    std::uint32_t mode_id) noexcept;

/** @brief 判断枚举是否为明确支持的计分类型。 */
[[nodiscard]] bool ValidMatchScoreModel(MatchScoreModel model) noexcept;

/** @brief 判断模式是否按每个玩家独立计分，与原生名义队伍无关。 */
[[nodiscard]] bool IndividualMatchScoreModel(MatchScoreModel model) noexcept;

/** @brief 判断模式是否允许已恢复的普通死亡/复活握手。 */
[[nodiscard]] bool OrdinaryMatchRespawn(std::uint32_t mode_id) noexcept;

/** @brief 原生 5F3BC0 排序字段；分数来源和同分值单位仍是本服竞速策略。 */
struct ExtremeRacingOrder final {
  bool departed{false};          ///< 原生标签4；离场者排在所有留场者之后。
  std::int64_t total_points{0};  ///< 累计两项分数之和，比较不做有符号取反。
  std::int32_t tie_break{0};  ///< 原生标签6；当前本服使用累计用时/未完成惩罚。
  std::uint16_t seat{0U};     ///< 开局冻结席位，同分同时间的确定性最后比较项。
};

/** @brief 局内榜和终局共用比较器：离场排后、总分降序、同分值及冻结席位升序。 */
[[nodiscard]] bool BetterExtremeRacingOrder(
    const ExtremeRacingOrder& lhs, const ExtremeRacingOrder& rhs) noexcept;

/** @brief 终局计算使用的一名开局参与者及已确认统计。 */
struct MatchResultParticipantInput final {
  PlayerId player_id;            ///< 开局冻结的玩家编号。
  std::uint8_t seat_index{0U};   ///< 开局稳定席位；同局唯一。
  std::uint8_t team_code{0U};    ///< 团队模型只允许 1/2，个人模型仍保留原值。
  MatchPerformance performance;  ///< 由运行时确认的累计统计。
  bool departed{
      false};  ///< 是否在正常终局前离场；奖励层据此强制按失败且不发奖。
  bool surviving{true};  ///< 生存目标使用确认生命周期，泡封者仍算存活。
  std::optional<std::uint8_t>
      result_team;  ///< 感染终局阵营 1/2；不改写持久化开局队伍。
  std::optional<std::int32_t>
      racing_tie_break;  ///< 模式24冻结同分值；必须全员提供，含离场者。
};

/** @brief 一名参与者的权威胜负、并列奖励名次与原统计。 */
struct MatchResultParticipant final {
  PlayerId player_id;            ///< 与输入一致的玩家编号。
  std::uint8_t seat_index{0U};   ///< 与输入一致的稳定席位。
  std::uint8_t team_code{0U};    ///< 与输入一致的开局队伍。
  MatchPerformance performance;  ///< 未经客户端奖励值污染的终局统计。
  MatchOutcome outcome{MatchOutcome::kDraw};  ///< 终局规则计算的胜、平或负。
  std::uint32_t rank{
      1U};  ///< 普通表现并列共享名次；竞速按原生席位顺序给唯一名次。
  bool departed{false};  ///< 供可靠结算应用离场政策。
};

/** @brief 一局普通击爆计分终局的不可变输入。 */
struct MatchResultInput final {
  MatchScoreModel score_model{
      MatchScoreModel::kTeamPangs};  ///< 团队或个人击爆目标。
  bool completed{true};              ///< 中断/放弃时所有人记平且不决定胜方。
  std::vector<MatchResultParticipantInput>
      participants;  ///< 所有开局玩家，含离场者。
  std::optional<std::uint8_t>
      decided_team;  ///< 爆破回合/合作由对应状态机提供，0 为平局。
};

/** @brief 终局规则违反成员、席位、队伍或容量约束时的稳定错误。 */
enum class MatchResultError : std::uint8_t {
  kInvalidScoreModel,
  kNoParticipants,
  kTooManyParticipants,
  kInvalidPlayer,
  kDuplicatePlayer,
  kDuplicateSeat,
  kInvalidTeam,
};

/** @brief 普通击爆计分终局结果；参与者按 PlayerId 升序，便于数据库锁顺序稳定。
 */
struct MatchResult final {
  std::uint8_t winning_team{
      0U};  ///< 团队唯一胜方为 1/2；平局、个人局或中断为 0。
  std::vector<MatchResultParticipant>
      participants;  ///< 按 PlayerId 升序的完整结果。
};

/**
 * @brief 依据确认统计计算团队/个人击爆胜负及表现名次。
 * @note
 * 名次比较顺序与旧服务一致：击爆、泡封、救援、较少死亡；相同表现共享奖励名次。
 *       个人胜负只看击爆目标，不能因泡封或席位顺序改变谁获胜。
 *       极限竞速全员提供同分值后使用独立比较器，正常终局第一名获胜，离场者不获胜。
 */
[[nodiscard]] std::variant<MatchResult, MatchResultError> CalculateMatchResult(
    const MatchResultInput& input);

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_RESULT_H_
