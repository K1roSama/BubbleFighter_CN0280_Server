#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TREASURE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TREASURE_REPOSITORY_H_

/**
 * @file treasure_repository.h
 * @brief 定义泡泡抽取和领取的 MySQL 原子事务适配器。
 */

#include "features/shop/treasure_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief
 * 在玩家父行上串行化抽取/领取，并同事务维护钱包、库存、流水和固定抽取结果。
 * @note 本仓储从 bubble_draws 的 JSON
 * 快照恢复旧结果，绝不按当前配置重新随机或重建奖励。
 */
class MySqlTreasureRepository final : public application::TreasureRepository {
 public:
  /** @brief 注入生命周期长于本仓储活动调用的有界连接池。 */
  explicit MySqlTreasureRepository(MySqlConnectionPool& pool) noexcept;

  /** @brief 在 repeatable-read 快照中读取钱包、十一种针和唯一 pending 抽取。 */
  [[nodiscard]] application::ServiceResult<application::TreasureStateSnapshot>
  LoadState(domain::PlayerId player_id,
            const application::BubbleServiceRules& rules,
            std::chrono::system_clock::time_point now) override;

  /** @brief 原子消耗并创建抽取；玩家已有 pending 时返回原结果且不重复消耗。 */
  [[nodiscard]] application::ServiceResult<
      std::pair<application::TreasureDrawSnapshot, bool>>
  Open(const application::TreasureOpenPersistenceCommand& command) override;

  /** @brief 原子发放冻结奖励并保存二选一下标和格位；重复领取明确拒绝。 */
  [[nodiscard]] application::ServiceResult<application::TreasureClaimResult>
  Claim(const application::TreasureClaimPersistenceCommand& command) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TREASURE_REPOSITORY_H_
