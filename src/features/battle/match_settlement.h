#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_H_

/**
 * @file match_settlement.h
 * @brief 定义对局终局快照、奖励计算和可靠单事务结算用例。
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/match_id.h"
#include "features/battle/match_reward.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/rooms/room_ownership.h"

namespace bf::application {

/** @brief 对局终止原因的持久语义；中断和放弃保留统计但不发奖励。 */
enum class MatchCompletionKind : std::uint8_t {
  kCompleted,
  kInterrupted,
  kAbandoned,
};

/** @brief 开局参与者在终局时冻结的席位、统计、胜负和离场状态。 */
struct MatchSettlementParticipant final {
  domain::PlayerId player_id;            ///< 开局冻结的玩家编号。
  std::uint8_t seat_index{0U};           ///< 开局稳定席位。
  std::uint8_t team_code{0U};            ///< 开局冻结队伍代码。
  domain::MatchPerformance performance;  ///< 服务端确认的累计统计。
  domain::MatchOutcome outcome{
      domain::MatchOutcome::kDraw};  ///< 未离场时的权威胜负。
  std::uint32_t rank{1U};            ///< 奖励名次；并列允许同值。
  bool departed{false};              ///< 是否在终局前离开或断线。
};

/**
 * @brief 一局只能提交一次的不可变终局命令。
 * @note participants 必须包含所有开局玩家，包括离场者；重试同 match_id
 * 时全部字段必须相同。
 */
struct MatchSettlementCommand final {
  domain::MatchId match_id;           ///< 对局稳定 UUID。
  std::uint32_t mode_id{0U};          ///< 开局冻结的正模式编号。
  std::uint32_t elapsed_seconds{0U};  ///< 0～3600 秒的终局时长。
  MatchCompletionKind completion_kind{
      MatchCompletionKind::kCompleted};  ///< 终局类型。
  std::vector<MatchSettlementParticipant>
      participants;  ///< 所有开局参与者终局快照。
  std::optional<RoomOwnership>
      room_owner;  ///< DS 首次提交必须匹配开局及当前有效租约。
};

/** @brief 终局快照写入耐久日志后的幂等状态。 */
enum class MatchSettlementStageState : std::uint8_t {
  kCreated,            ///< 首次保存该 match_id 的不可变终局快照。
  kExistingIdentical,  ///< 已存在逐字段相同的快照，可继续按至少一次语义投递。
};

/** @brief 耐久日志确认保存的对局身份和幂等状态。 */
struct MatchSettlementStageReceipt final {
  domain::MatchId match_id;  ///< 已持久化的稳定对局 UUID。
  MatchSettlementStageState state{
      MatchSettlementStageState::kCreated};  ///< 保存结果。
};

/**
 * @brief 在数据库调度前保存不可变终局快照并提供崩溃恢复扫描的端口。
 * @note MySQL adapter 必须在独立短事务中按 match_id 幂等保存完整命令；同 ID
 * 不同内容返回 kConflict。load_recoverable 必须按 created_at/match_id
 * 稳定排序，只返回 pending、处理中孤儿
 *       和可重试失败记录，不得返回成功或终止失败记录。
 * @note 恢复是至少一次投递，扫描和实时提交可能返回同一命令；最终结算仍由
 * match_id 唯一约束和 MatchSettlementService 的重放校验保证不重复发奖。
 */
class MatchSettlementJournal {
 public:
  virtual ~MatchSettlementJournal() = default;

  /** @brief 原子保存终局快照，成功返回首次创建或相同内容重放。 */
  [[nodiscard]] virtual ServiceResult<MatchSettlementStageReceipt> Stage(
      const MatchSettlementCommand& command) = 0;

  /** @brief 读取至多 limit 条可恢复命令；返回值不得超过 limit 且 match_id
   * 不得重复。 */
  [[nodiscard]] virtual ServiceResult<std::vector<MatchSettlementCommand>>
  LoadRecoverable(std::size_t limit) = 0;
};

/**
 * @brief 校验可持久化和结算的终局命令公共不变量。
 * @return
 * 合法时为空；模式、时长、终止类型、人数、玩家、席位、队伍、胜负或排名非法时
 *         返回安全错误。
 */
[[nodiscard]] std::optional<ServiceError> ValidateMatchSettlementCommand(
    const MatchSettlementCommand& command);

/** @brief 单名玩家提交后的统计与实际奖励结果。 */
struct MatchSettlementPlayerResult final {
  domain::PlayerId player_id;            ///< 结果所属玩家。
  domain::MatchPerformance performance;  ///< 已累计到模式统计的终局值。
  domain::MatchOutcome effective_outcome{
      domain::MatchOutcome::kDraw};   ///< 离场修正后的胜负。
  std::uint32_t rank{1U};             ///< 采用的奖励名次。
  domain::AppliedMatchReward reward;  ///< 实际前后值、增量和等级。
};

/** @brief 开局冻结战队与本局已提交贡献，用于原生比赛积分提示。 */
struct MatchClanContribution final {
  domain::PlayerId player_id;  ///< 已正常完成本局的参与者。
  std::uint32_t clan_id{0U};   ///< 开局冻结的普通战队。
  std::uint32_t points{0U};    ///< 已提交积分；失败方为零。
};

/** @brief 本局一名玩家的真实奖金或退款债权；累计领取量不会混入普通战斗奖励。 */
struct UserHostPrizeClaimReceipt final {
  domain::PlayerId player_id;  ///< 领取者，退款只能是开局冻结的主办者。
  std::int64_t amount{0};      ///< 本局不可变的应付总额。
  std::int64_t credited_amount{
      0};                  ///< 已实际入账总额；余额上限下允许小于应付总额。
  bool ticket_due{false};  ///< 退款还应返还一张101164。
  bool ticket_credited{false};  ///< 应退卡已实际进入库存。

  /** @brief 比较债权和真实领取进度。 */
  bool operator==(const UserHostPrizeClaimReceipt&) const = default;
};

/** @brief 奖金处理后同一数据库快照中的钱包；它不是可再次累加的奖励增量。 */
struct UserHostPrizeWalletReceipt final {
  domain::PlayerId player_id;  ///< 参赛者或开局冻结主办者。
  PlayerWalletView
      wallets;  ///< 含其他待领奖金在本次事务中实际释放后的三钱包余额。
};

/**
 * @brief 已关闭主办奖金的独立业务回执。
 * @note
 * amount/fee/claims.amount来源于耐久账本；credited_amount、ticket_credited和wallets是
 *
 * 本次读取时的状态，重放须重新读取，不能用历史战斗奖励after覆盖当前余额。

 */
struct UserHostPrizeReceipt final {
  domain::PlayerId sponsor_player_id;  ///< 开局冻结的出资者，可不在参赛名单中。
  std::int64_t amount{0};              ///< 开局真实扣除的总金币。
  std::int64_t fee_amount{0};          ///< 预定手续费；退款时不扣留。
  std::uint32_t policy_version{1U};    ///< 本服明确版本化的分配策略。
  bool refunded{false};                ///< 真为退回总额和卡，假为胜方净奖金。
  std::vector<UserHostPrizeClaimReceipt>
      claims;  ///< 按玩家编号升序的本局债权。
  std::vector<UserHostPrizeWalletReceipt>
      wallets;  ///< 全部参赛者与主办者并集，按编号升序。
};

/** @brief 普通奖励可确定性重放；附属业务状态在同一事务中重新读取。 */
struct MatchSettlementResult final {
  domain::MatchId match_id;           ///< 与命令一致的对局 UUID。
  std::uint32_t mode_id{0U};          ///< 已结算模式。
  std::uint32_t elapsed_seconds{0U};  ///< 已累计到模式统计的时长。
  MatchCompletionKind completion_kind{
      MatchCompletionKind::kCompleted};  ///< 已持久化终局类型。
  std::vector<MatchSettlementPlayerResult>
      players;  ///< 按 player_id 升序的玩家结果。
  std::vector<MatchClanContribution>
      clan_contributions;  ///< 从耐久战队流水读取的提示数据。
  std::optional<UserHostPrizeReceipt>
      user_host_prize;  ///< 真实托管和债权回执，普通比赛为空。
  std::vector<domain::PlayerId>
      couple_daily_gift_players;  ///< 本局真实入库的每日赠品。
};

/** @brief 结算占用状态；已成功的同快照重试直接返回原结果。 */
enum class MatchSettlementClaimState : std::uint8_t {
  kNew,
  kCompletedReplay,
};

/** @brief 锁定对局/结算行后返回的规则快照和可选历史结果。 */
struct MatchSettlementClaim final {
  MatchSettlementClaimState state{
      MatchSettlementClaimState::kNew};     ///< 新执行或成功重放。
  domain::MatchRewardPolicy reward_policy;  ///< 开局冻结的奖励规则。
  std::optional<MatchSettlementResult> completed_result;  ///< 仅成功重放存在。
  std::vector<domain::PlayerId>
      couple_bonus_players;  ///< 开局冻结且双方正常完成的加成名单。
};

/** @brief 写入一名玩家统计、进度、金币和奖励流水所需的完整值。 */
struct MatchSettlementPlayerWrite final {
  domain::MatchId match_id;            ///< 所属对局 UUID。
  std::uint32_t mode_id{0U};           ///< 模式统计维度。
  std::uint32_t elapsed_seconds{0U};   ///< 本局累计时长。
  MatchSettlementPlayerResult result;  ///< 实际奖励和终局统计。
};

/** @brief 按锁顺序返回的玩家标识与当前进度，防止 adapter 错位发奖。 */
struct LockedRewardBalances final {
  domain::PlayerId player_id;  ///< 被锁定的玩家；必须与请求同位置一致。
  domain::CurrentRewardBalances
      balances;  ///< 同一事务内读取的经验、金币和评级。
};

/**
 * @brief 可靠结算独占的数据库事务端口。
 * @note adapter 必须锁定 match/settlement 行，并按传入的 player_id
 * 升序锁资料、钱包和进度； 任一方法失败后对象析构必须回滚，禁止部分发布奖励。
 */
class MatchSettlementTransaction {
 public:
  virtual ~MatchSettlementTransaction() = default;

  /**
   * @brief 占用待结算对局或读取已成功结果。
   * @note 同 match_id 的不同终局快照必须返回 kConflict；处理中/可重试失败由
   * adapter 串行恢复。
   */
  [[nodiscard]] virtual ServiceResult<MatchSettlementClaim> ClaimSettlement(
      const MatchSettlementCommand& command) = 0;

  /** @brief 按严格升序锁定所有参与者当前经验、金币和评级并以同顺序返回。 */
  [[nodiscard]] virtual ServiceResult<std::vector<LockedRewardBalances>>
  LockRewardBalances(std::span<const domain::PlayerId> player_ids) = 0;

  /** @brief 原子写一名玩家的模式统计、进度、金币及非零金币奖励流水。 */
  [[nodiscard]] virtual std::optional<ServiceError> ApplyPlayerSettlement(
      const MatchSettlementPlayerWrite& write) = 0;

  /** @brief 将结算和对局标为成功并保存不含秘密的结果快照。 */
  [[nodiscard]] virtual std::optional<ServiceError> CompleteSettlement(
      const MatchSettlementResult& result) = 0;

  /** @brief 成功标记后、提交前读取本局真实战队贡献；旧实现可不提供。 */
  [[nodiscard]] virtual ServiceResult<std::vector<MatchClanContribution>>
  ClanContributions() {
    return std::vector<MatchClanContribution>{};
  }

  /** @brief 成功标记后或重放时读取真实主办奖金及当前钱包；不重新领取或发放。 */
  [[nodiscard]] virtual ServiceResult<std::optional<UserHostPrizeReceipt>>
  UserHostPrize() {
    return std::optional<UserHostPrizeReceipt>{};
  }

  /** @brief 读取本局已入库的每日情侣赠品，重放只读。 */
  [[nodiscard]] virtual ServiceResult<std::vector<domain::PlayerId>>
  CoupleDailyGifts() {
    return std::vector<domain::PlayerId>{};
  }

  /** @brief 原子提交全部玩家统计、奖励及结算状态；必须是最后一次调用。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/**
 * @brief 结算事务工厂，由数据库工作线程上的 MySQL adapter 实现。
 * @note 多工作线程共享 MatchSettlementService 时 begin
 * 可能并发调用；实现必须线程安全，并让
 *       每个成功返回的事务独占一个连接直到提交或析构回滚。
 */
class MatchSettlementUnitOfWork {
 public:
  virtual ~MatchSettlementUnitOfWork() = default;

  /** @brief 开始一个独占连接的短事务；未提交析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<
      std::unique_ptr<MatchSettlementTransaction>>
  Begin() = 0;
};

/**
 * @brief 房间终局编排依赖的可靠结算接口。
 * @note
 * 调度器可从多个数据库工作线程并发调用；实现若不支持并发，必须在装配时只配置一个
 *       结算工作线程，而不能依赖网络/房间线程替它串行化。
 */
class MatchSettlementExecutor {
 public:
  virtual ~MatchSettlementExecutor() = default;

  /** @brief 原子结算或重放一份不可变终局快照。 */
  [[nodiscard]] virtual ServiceResult<MatchSettlementResult> Settle(
      const MatchSettlementCommand& command) const = 0;
};

/** @brief 在一个事务中按稳定玩家顺序应用统计和奖励的结算服务。 */
class MatchSettlementService final : public MatchSettlementExecutor {
 public:
  /** @brief 注入结算事务工厂；其生命周期必须覆盖本服务。 */
  explicit MatchSettlementService(
      MatchSettlementUnitOfWork& unit_of_work) noexcept;

  /** @brief 校验终局、占用 match_id、计算实际奖励并一次提交全部玩家。 */
  [[nodiscard]] ServiceResult<MatchSettlementResult> Settle(
      const MatchSettlementCommand& command) const override;

 private:
  MatchSettlementUnitOfWork& unit_of_work_;  ///< 独占事务来源。
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_H_
