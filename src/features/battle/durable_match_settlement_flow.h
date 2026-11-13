#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_DURABLE_MATCH_SETTLEMENT_FLOW_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_DURABLE_MATCH_SETTLEMENT_FLOW_H_

/**
 * @file durable_match_settlement_flow.h
 * @brief 定义终局快照先持久化、再异步投递及启动恢复的运行时编排。
 */

#include <cstddef>
#include <cstdint>
#include <optional>

#include "application/error.h"
#include "application/event_logger.h"
#include "features/battle/match_settlement.h"
#include "features/battle/match_settlement_dispatcher.h"

namespace bf::runtime {

/** @brief 一份已耐久保存的终局命令当前是否进入进程内调度器。 */
enum class DurableMatchSettlementDisposition : std::uint8_t {
  kScheduled,             ///< 已被有界调度器接收并承诺在正常停机前执行。
  kPersistedForRecovery,  ///< 调度器过载或未运行；命令仍在 MySQL
                          ///< 日志中等待重投。
};

/** @brief 实时终局提交的耐久保存与进程内调度结果。 */
struct DurableMatchSettlementSubmission final {
  /** @brief 数据库耐久保存回执。 */
  application::MatchSettlementStageReceipt stage;

  /** @brief 命令当前已进入调度器，或仅留待恢复扫描处理。 */
  DurableMatchSettlementDisposition disposition{
      DurableMatchSettlementDisposition::kScheduled};

  /** @brief 未进入调度器时的安全原因；已调度时为空。 */
  std::optional<application::ServiceError> scheduling_error;
};

/** @brief 一次恢复扫描读取、调度和留待后续扫描的数量。 */
struct MatchSettlementRecoveryReport final {
  /** @brief 本次从耐久日志读取的命令数。 */
  std::size_t loaded{0U};

  /** @brief 本次被调度器接收的命令数。 */
  std::size_t scheduled{0U};

  /** @brief 因首个调度错误而仍留在日志中的命令数。 */
  std::size_t deferred{0U};

  /** @brief 首个调度错误；无延后时为空。 */
  std::optional<application::ServiceError> scheduling_error;
};

/**
 * @brief 强制终局命令先写耐久日志，再按至少一次语义交给结算调度器。
 * @note 对象不拥有 journal、dispatcher 或
 * logger；三者必须比本对象存活更久。所有方法应由 composition root
 * 的单一恢复/世界执行器调用，数据库端口自身仍须允许对应调用并发。
 * @note stage
 * 成功后即使调度器过载也返回成功值而不是伪装成未保存错误；调用方可安全销毁房间，
 *       恢复扫描会从 MySQL
 * 取得同一拥有型命令。扫描可能重复投递，结算必须保持幂等。
 */
class DurableMatchSettlementFlow final {
 public:
  /** @brief 注入耐久日志、进程内调度边界和日志端口。 */
  DurableMatchSettlementFlow(application::MatchSettlementJournal& journal,
                             MatchSettlementSubmissionSink& dispatcher,
                             application::EventLogger& logger) noexcept;

  /**
   * @brief 校验并耐久保存实时终局，然后尝试投递到进程内有界队列。
   * @return 保存失败返回
   * ServiceError；保存成功后始终返回拥有明确调度状态的结果。
   */
  [[nodiscard]] application::ServiceResult<DurableMatchSettlementSubmission>
  Submit(const application::MatchSettlementCommand& command);

  /**
   * @brief 从耐久日志读取并按稳定顺序重投一批未完成结算。
   * @param limit 本批上限，必须为 1～4096。
   * @return 日志读取/契约错误返回 ServiceError；调度过载则返回含 deferred
   * 的正常报告。
   */
  [[nodiscard]] application::ServiceResult<MatchSettlementRecoveryReport>
  Recover(std::size_t limit);

 private:
  application::MatchSettlementJournal& journal_;  ///< MySQL 终局日志端口。
  MatchSettlementSubmissionSink& dispatcher_;     ///< 进程内有界调度端口。
  application::EventLogger& logger_;              ///< 脱敏运行日志端口。
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_DURABLE_MATCH_SETTLEMENT_FLOW_H_
