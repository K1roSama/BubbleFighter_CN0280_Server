#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORKSHOP_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORKSHOP_REPOSITORY_H_

/** @file workshop_repository.h @brief 定义科学工坊 MySQL 单事务适配器。 */

#include "features/shop/workshop_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 串行化同一玩家并原子维护消耗、奖励、审计与工坊操作历史。 */
class MySqlWorkshopRepository final : public application::WorkshopRepository {
 public:
  /** @brief 注入生命周期覆盖全部调用的有界连接池。 */
  explicit MySqlWorkshopRepository(MySqlConnectionPool& pool) noexcept;

  /** @brief 执行一个已经过应用层规则校验的短事务。 */
  [[nodiscard]] application::ServiceResult<application::WorkshopResult> Execute(
      const application::WorkshopCommand& command) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORKSHOP_REPOSITORY_H_
