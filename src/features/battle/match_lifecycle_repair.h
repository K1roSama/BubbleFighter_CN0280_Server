#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_LIFECYCLE_REPAIR_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_LIFECYCLE_REPAIR_H_

/**
 * @file match_lifecycle_repair.h
 * @brief 定义开局提交后安装失败及进程重启遗留活动对局的数据库补偿用例。
 */

#include <chrono>
#include <cstddef>

#include "application/error.h"
#include "features/battle/match_id.h"

namespace bf::application {

/** @brief 一次启动恢复实际中断的数据库记录数量。 */
struct MatchLifecycleRecoveryReport final {
  std::size_t match_count{
      0U};  ///< 从 loading/playing 转为 interrupted 的对局数。
  std::size_t participant_count{
      0U};  ///< 同事务标记为 disconnected 的参与者数。
};

/**
 * @brief 对局生命周期补偿的持久化端口。
 * @note 两个操作都必须在单个短事务内同时更新 matches 与
 * match_participants；重复执行不得失败。
 */
class MatchLifecycleRepairRepository {
 public:
  virtual ~MatchLifecycleRepairRepository() = default;

  /** @brief
   * 与同UUID开局串行中断；interrupted或锁下确认没有提交的记录是幂等补偿成功。
   */
  [[nodiscard]] virtual std::optional<ServiceError> Interrupt(
      const domain::MatchId& match_id,
      std::chrono::system_clock::time_point interrupted_at) = 0;

  /**
   * @brief 在单实例服务开放监听前，中断上个进程遗留的全部 loading/playing
   * 对局。
   * @note
   * 本接口不适用于共享同一数据库的多活实例；多活部署必须先引入实例租约再调用。
   */
  [[nodiscard]] virtual ServiceResult<MatchLifecycleRecoveryReport>
  RecoverAbandoned(std::chrono::system_clock::time_point interrupted_at) = 0;
};

/** @brief 世界执行器与 composition root 使用的生命周期修复接口。 */
class MatchLifecycleRepairExecutor {
 public:
  virtual ~MatchLifecycleRepairExecutor() = default;

  /** @brief 记录“数据库开局已提交、内存安装失败”的终态，可按同一 match_id
   * 重试。 */
  [[nodiscard]] virtual std::optional<ServiceError> InterruptFailedStart(
      const domain::MatchId& match_id,
      std::chrono::system_clock::time_point interrupted_at) const = 0;

  /** @brief 在启动期间恢复进程崩溃遗留的活动记录。 */
  [[nodiscard]] virtual ServiceResult<MatchLifecycleRecoveryReport>
  RecoverAbandoned(
      std::chrono::system_clock::time_point interrupted_at) const = 0;
};

/**
 * @brief 校验时间与标识后，把生命周期修复交给数据库原子实现。
 * @note
 * 服务不在内存中猜测终局统计或发奖；被中断记录只用于审计，不进入正常结算。
 */
class MatchLifecycleRepairService final : public MatchLifecycleRepairExecutor {
 public:
  /** @brief 注入生命周期长于本服务的持久化端口。 */
  explicit MatchLifecycleRepairService(
      MatchLifecycleRepairRepository& repository) noexcept;

  /** @copydoc MatchLifecycleRepairExecutor::interrupt_failed_start */
  [[nodiscard]] std::optional<ServiceError> InterruptFailedStart(
      const domain::MatchId& match_id,
      std::chrono::system_clock::time_point interrupted_at) const override;

  /** @copydoc MatchLifecycleRepairExecutor::recover_abandoned */
  [[nodiscard]] ServiceResult<MatchLifecycleRecoveryReport> RecoverAbandoned(
      std::chrono::system_clock::time_point interrupted_at) const override;

 private:
  MatchLifecycleRepairRepository&
      repository_;  ///< 事务边界由 MySQL adapter 拥有。
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_LIFECYCLE_REPAIR_H_
