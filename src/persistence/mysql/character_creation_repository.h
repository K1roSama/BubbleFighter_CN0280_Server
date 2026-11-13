#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHARACTER_CREATION_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHARACTER_CREATION_REPOSITORY_H_

/**
 * @file character_creation_repository.h
 * @brief 定义首次角色创建用例的 MySQL 事务适配器。
 */

#include "features/characters/character_creation.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从有界连接池创建角色创建事务。
 * @note 每次 `begin` 独占一个连接；事务对象销毁但未提交时由 MySQL 自动回滚。
 */
class MySqlCharacterCreationUnitOfWork final
    : public application::CharacterCreationUnitOfWork {
 public:
  /** @brief 注入生命周期长于本对象及其活动事务的连接池。 */
  explicit MySqlCharacterCreationUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取连接并开始一个原子建角事务。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::CharacterCreationTransaction>>
  Begin() override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHARACTER_CREATION_REPOSITORY_H_
