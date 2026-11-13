#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PURCHASE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PURCHASE_REPOSITORY_H_

/**
 * @file purchase_repository.h
 * @brief 定义商城购买用例的 MySQL 单事务适配器。
 */

#include "features/shop/purchase_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从有界连接池创建购买事务。
 * @note
 * 每个事务按玩家父记录串行化，并原子提交操作占位、钱包、库存、订单和审计流水。
 */
class MySqlPurchaseUnitOfWork final : public application::PurchaseUnitOfWork {
 public:
  /** @brief 注入生命周期长于本对象及其活动事务的连接池。 */
  explicit MySqlPurchaseUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取独占连接并开始购买事务。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::PurchaseTransaction>>
  Begin() override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PURCHASE_REPOSITORY_H_
