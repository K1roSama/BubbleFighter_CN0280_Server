#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PLAYER_MENU_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PLAYER_MENU_REPOSITORY_H_

/**
 * @file player_menu_repository.h
 * @brief 定义登录后菜单一致性快照的 MySQL 读取适配器。
 */

#include "features/lobby/player_menu_snapshot.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 在一个
 * repeatable-read、只读事务中装配玩家资料、成长、钱包、库存和配装。
 * @note 查询使用调用方提供的同一 UTC 时刻过滤期限，避免多表读取发生时间撕裂。
 */
class MySqlPlayerMenuSnapshotReader final
    : public application::PlayerMenuSnapshotReader {
 public:
  /** @brief 注入生命周期长于本 reader 的连接池。 */
  explicit MySqlPlayerMenuSnapshotReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 读取一个已建角玩家的完整菜单快照；不存在返回空值。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::PlayerMenuSnapshot>>
  Load(domain::PlayerId player_id,
       std::chrono::system_clock::time_point now) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_PLAYER_MENU_REPOSITORY_H_
