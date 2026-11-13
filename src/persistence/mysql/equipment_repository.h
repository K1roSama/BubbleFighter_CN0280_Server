#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_EQUIPMENT_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_EQUIPMENT_REPOSITORY_H_

/**
 * @file equipment_repository.h
 * @brief 定义玩家换装用例的 MySQL 事务适配器。
 */

#include "features/inventory/equipment_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 从有界连接池创建锁定库存并修改零号配装页的短事务。 */
class MySqlChangeEquipmentUnitOfWork final
    : public application::ChangeEquipmentUnitOfWork {
 public:
  /** @brief 注入生命周期长于本对象及其活动事务的连接池。 */
  explicit MySqlChangeEquipmentUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取独占连接并开始换装事务。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::ChangeEquipmentTransaction>>
  Begin() override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_EQUIPMENT_REPOSITORY_H_
