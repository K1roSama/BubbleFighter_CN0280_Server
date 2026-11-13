#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_REPOSITORY_H_

/**
 * @file match_repository.h
 * @brief 定义对局开局、终局日志和可靠结算的 MySQL 适配器。
 */

#include "features/battle/match_lifecycle_repair.h"
#include "features/battle/match_settlement.h"
#include "features/battle/match_start.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从有界连接池创建对局开局事务。
 * @note 一次事务原子写入 matches 和完整参与者集合；相同 match_id
 * 只接受逐字段相同的重放。
 */
class MySqlMatchStartUnitOfWork final
    : public application::MatchStartUnitOfWork {
 public:
  /** @brief 注入生命周期长于本对象及其活动事务的连接池。 */
  explicit MySqlMatchStartUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取独占连接并开始一个可回滚的开局事务。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::MatchStartTransaction>>
  Begin() override;

 private:
  MySqlConnectionPool& pool_;  ///< 共享有界连接池，不由本对象拥有。
};

/**
 * @brief 原子中断开局安装失败或上次单实例进程遗留的活动对局。
 * @note
 * 指定对局的重复补偿是幂等的；启动恢复必须在监听端口开放前执行，避免误伤本进程新局。
 */
class MySqlMatchLifecycleRepairRepository final
    : public application::MatchLifecycleRepairRepository {
 public:
  /** @brief 注入生命周期长于本对象的连接池。 */
  explicit MySqlMatchLifecycleRepairRepository(
      MySqlConnectionPool& pool) noexcept;

  /** @brief 同事务中断指定 matches 行和仍为 joined 的参与者。 */
  [[nodiscard]] std::optional<application::ServiceError> Interrupt(
      const domain::MatchId& match_id,
      std::chrono::system_clock::time_point interrupted_at) override;

  /** @brief 锁定并中断启动前遗留的全部 loading/playing 行。 */
  [[nodiscard]] application::ServiceResult<
      application::MatchLifecycleRecoveryReport>
  RecoverAbandoned(
      std::chrono::system_clock::time_point interrupted_at) override;

 private:
  MySqlConnectionPool& pool_;  ///< 共享有界连接池，不由本对象拥有。
};

/**
 * @brief 将不可变终局命令先保存到 MySQL，并按稳定顺序恢复未完成命令。
 * @note stage
 * 自身提交独立短事务；成功回执后房间即可释放终局内存。恢复扫描不占用结算行锁，
 *       因此仍是至少一次投递，最终幂等性由结算事务保证。
 */
class MySqlMatchSettlementJournal final
    : public application::MatchSettlementJournal {
 public:
  /** @brief 注入生命周期长于本对象的连接池。 */
  explicit MySqlMatchSettlementJournal(MySqlConnectionPool& pool) noexcept;

  /** @brief 原子验证开局快照并保存终局；冲突终局绝不覆盖已保存内容。 */
  [[nodiscard]] application::ServiceResult<
      application::MatchSettlementStageReceipt>
  Stage(const application::MatchSettlementCommand& command) override;

  /** @brief 按创建时间和 match_id 读取待处理、可重试或遗留处理中命令。 */
  [[nodiscard]] application::ServiceResult<
      std::vector<application::MatchSettlementCommand>>
  LoadRecoverable(std::size_t limit) override;

 private:
  MySqlConnectionPool& pool_;  ///< 共享有界连接池，不由本对象拥有。
};

/**
 * @brief 从有界连接池创建可靠对局结算事务。
 * @note 每个事务按 match、settlement、player_id
 * 升序锁定数据，并把统计、进度、钱包、审计、
 *       参与者结果和成功快照一次提交；异常析构会全部回滚到可恢复状态。
 */
class MySqlMatchSettlementUnitOfWork final
    : public application::MatchSettlementUnitOfWork {
 public:
  /** @brief 注入生命周期长于本对象及其活动事务的连接池。 */
  explicit MySqlMatchSettlementUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取独占连接并开始一个可回滚的结算事务。 */
  [[nodiscard]]
  application::ServiceResult<
      std::unique_ptr<application::MatchSettlementTransaction>> Begin()
      override;

 private:
  MySqlConnectionPool& pool_;  ///< 共享有界连接池，不由本对象拥有。
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_REPOSITORY_H_
