#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_ACCESS_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_ACCESS_REPOSITORY_H_

/**
 * @file world_access_repository.h
 * @brief 定义世界战队权限与双向聊天屏蔽的 MySQL 只读适配器。
 */

#include "features/authentication/world_access.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 在一个 MySQL 快照中读取玩家存在性、战队归属和双向屏蔽。 */
class MySqlWorldAccessReader final : public application::WorldAccessReader {
 public:
  /** @brief 注入生命周期覆盖本 reader 的有界连接池。 */
  explicit MySqlWorldAccessReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 屏蔽结果使用双方并集并按玩家 ID 排序去重。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::WorldAccessSnapshot>>
  Load(domain::PlayerId player_id) override;
  [[nodiscard]] std::optional<application::ServiceError> ActivateDevice(
      domain::PlayerId player, std::string_view code) override;

 private:
  MySqlConnectionPool& pool_;  ///< 每次读取只借用一条连接。
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_ACCESS_REPOSITORY_H_
