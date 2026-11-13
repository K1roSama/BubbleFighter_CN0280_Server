#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_KEY_MAP_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_KEY_MAP_STORE_H_

/** @file key_map_store.h
 *  @brief 原版按键映射的 MySQL 实现。
 */
#include "features/inventory/key_map.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 锁玩家行后完整替换按键表，连接重试不会产生重复行。 */
class MySqlKeyMapStore final : public application::KeyMapStore {
 public:
  /** @brief 注入由进程持有的有界连接池。 */
  explicit MySqlKeyMapStore(MySqlConnectionPool& pool) noexcept : pool_(pool) {}

  [[nodiscard]] application::ServiceResult<std::vector<application::KeyBinding>>
  Load(domain::PlayerId player) override;
  [[nodiscard]] application::ServiceResult<bool> Save(
      domain::PlayerId player,
      const application::KeyMapSettings& settings) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_KEY_MAP_STORE_H_
