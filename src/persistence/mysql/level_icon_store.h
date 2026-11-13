#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LEVEL_ICON_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LEVEL_ICON_STORE_H_

/** @file level_icon_store.h
 *  @brief MySQL 等级图标选择事务适配器。
 */
#include "features/characters/level_icon.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 对已建角玩家保存经服务器身份校验的图标偏好。 */
class MySqlLevelIconStore final : public application::LevelIconStore {
 public:
  /** @brief 借用共享连接池。 */
  explicit MySqlLevelIconStore(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  /** @brief 事务内验证家族并幂等更新偏好。 */
  [[nodiscard]] application::ServiceResult<bool> Save(
      domain::PlayerId player,
      const application::LevelIconSelection& selection) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LEVEL_ICON_STORE_H_
