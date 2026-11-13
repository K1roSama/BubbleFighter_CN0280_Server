#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_START_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_START_H_

/**
 * @file match_start.h
 * @brief 定义正式开局前持久化对局身份、规则版本和全员快照的原子用例。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/match_id.h"
#include "features/battle/match_result.h"
#include "features/rooms/room_ownership.h"

namespace bf::application {

/** @brief 为每次新的对局开局生成不可预测、非零且进程内不重复的稳定 UUID。 */
class MatchIdSource {
 public:
  virtual ~MatchIdSource() = default;

  /**
   * @brief 返回一个新的对局
   * UUID；调用方须先保存该值，再以相同值重试不确定提交。
   * @note 正式实现必须使用系统 CSPRNG，不得使用房间号、时间戳或进程内计数器。
   */
  [[nodiscard]] virtual ServiceResult<domain::MatchId> Next() = 0;
};

/** @brief
 * 正式开局时冻结的一名参与者；席位和队伍在终局前不得被后来房间状态覆盖。 */
struct MatchStartParticipant final {
  domain::PlayerId player_id;   ///< 已认证且属于开局房间的玩家。
  std::uint8_t seat_index{0U};  ///< 开局零基稳定席位。
  std::uint8_t team_code{0U};   ///< 已确认普通模式只允许队伍 1 或 2。
};

/** @brief
 * 本服第一版主办比赛奖金规则；数值上下限由服务端配置，不能据此声称恢复了原服分配规则。
 */
struct UserHostPrizeTerms final {
  domain::PlayerId sponsor_player_id;  ///< 已认证的主办者，可为不参赛的观察者。
  std::uint32_t amount{0U};            ///< 本局含百分之十手续费的总扣款。
  std::uint32_t minimum_amount{1000U};    ///< 本局冻结的配置下限。
  std::uint32_t maximum_amount{100000U};  ///< 本局冻结的配置上限。
  std::uint32_t policy_version{
      1U};  ///< 本服规则一：未离场胜方均分，余数按玩家编号，平局或中断退款。
};

/**
 * @brief 可按同一 match_id 安全重试的开局持久化命令。
 * @note
 * started_at、参与者及两个配置版本必须在第一次提交前冻结；重试时不得重新读取活动房间。
 */
struct MatchStartCommand final {
  domain::MatchId match_id;       ///< 调用方预先生成并在重试间保持稳定的 UUID。
  domain::RoomId source_room_id;  ///< 进程内来源房间，仅用于追踪。
  std::uint32_t mode_id{0U};      ///< 已确认支持的正模式编号。
  std::int64_t configuration_version_id{0};  ///< 已发布地图/模式配置版本。
  std::int64_t reward_rules_version_id{0};   ///< 已发布奖励规则版本。
  domain::MatchScoreModel score_model;       ///< 普通团队或个人击爆计分规则。
  std::chrono::system_clock::time_point
      started_at;  ///< 开局 UTC 时间；数据库使用 timestamptz。
  std::uint32_t source_channel{
      0U};  ///< 服务端冻结的频道；零代表历史调用方未知，80 才可登记战队赛。
  std::vector<MatchStartParticipant>
      participants;  ///< 完整开局名单；服务会按 player_id 规范排序。
  std::optional<UserHostPrizeTerms>
      user_host_prize;  ///< 仅真实主办比赛设置；开局事务同时扣款和扣一张卡。
  std::optional<RoomOwnership>
      room_owner;  ///< 独立 DS 开局必须携带；本地兼容后端为空。
};

/** @brief 开局记录的占用结果；相同命令重试只返回既有记录，不重复插入参与者。 */
enum class MatchStartClaimState : std::uint8_t {
  kCreated,
  kExistingSame,
};

/** @brief 开局事务提交后的稳定结果。 */
struct MatchStartResult final {
  domain::MatchId match_id;           ///< 已持久化对局 UUID。
  MatchStartClaimState claim_state;   ///< 本次创建或同内容幂等重放。
  std::size_t participant_count{0U};  ///< 已冻结参与者数量。
};

/**
 * @brief 对局开局独占数据库事务端口。
 * @note adapter 必须在同一事务中写 matches 和全部 match_participants；同
 * match_id 不同内容返回 kConflict，相同规范内容返回
 * kExistingSame。析构未提交时必须回滚。
 */
class MatchStartTransaction {
 public:
  virtual ~MatchStartTransaction() = default;

  /** @brief 创建开局记录或验证已有记录与命令完全一致。 */
  [[nodiscard]] virtual ServiceResult<MatchStartClaimState> Persist(
      const MatchStartCommand& command) = 0;

  /** @brief 原子提交对局主记录和全部参与者；必须是最后一次调用。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 从数据库工作线程取得一个独占连接和短事务的工厂。 */
class MatchStartUnitOfWork {
 public:
  virtual ~MatchStartUnitOfWork() = default;

  /** @brief 开始事务；返回的空事务对象属于 adapter 契约错误。 */
  [[nodiscard]] virtual ServiceResult<std::unique_ptr<MatchStartTransaction>>
  Begin() = 0;
};

/** @brief 房间开局编排依赖的持久化接口。 */
class MatchStartExecutor {
 public:
  virtual ~MatchStartExecutor() = default;

  /** @brief 校验并原子持久化一局身份、规则版本和完整参与者快照。 */
  [[nodiscard]] virtual ServiceResult<MatchStartResult> Start(
      MatchStartCommand command) const = 0;
};

/**
 * @brief 实现“数据库提交成功后才允许房间安装 match_id”的开局用例。
 * @note
 * 本服务只在数据库工作线程执行；不会修改房间内存，也不会在事务中等待网络操作。
 */
class MatchStartService final : public MatchStartExecutor {
 public:
  /** @brief 注入事务工厂；其生命周期必须覆盖本服务。 */
  explicit MatchStartService(MatchStartUnitOfWork& unit_of_work) noexcept;

  /**
   * @brief 按 player_id 规范化参与者后，在一个事务中持久化整局。
   * @return
   * 成功时返回创建/重放状态；非法规则、重复玩家/席位或事务失败返回明确错误。
   */
  [[nodiscard]] ServiceResult<MatchStartResult> Start(
      MatchStartCommand command) const override;

 private:
  MatchStartUnitOfWork& unit_of_work_;  ///< 由数据库工作池线程提供的事务来源。
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_START_H_
