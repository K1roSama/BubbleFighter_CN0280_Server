#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_PREPARATION_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_PREPARATION_REPOSITORY_H_

/**
 * @file match_preparation_repository.h
 * @brief 定义开局发布版本与全员原生配装的 MySQL 一致性读取适配器。
 */

#include "features/battle/match_preparation.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 在一个 repeatable-read
 * 只读事务中解析目录版本、奖励版本和全员零号配装。
 * @note 原生武器映射沿用 build 752 已验证规则：武器 item_id 减
 * 200000；未装备栏使用
 *       客户端导出的四项固定默认物品。适配器不会信任客户端自报模板。
 */
class MySqlMatchPreparationReader final
    : public application::MatchPreparationReader {
 public:
  /** @brief 注入生命周期覆盖本对象的有界连接池。 */
  explicit MySqlMatchPreparationReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 读取唯一 published 目录/奖励版本及每名参与者的当前有效配装。 */
  [[nodiscard]] application::ServiceResult<application::PreparedMatch> Load(
      const application::MatchPreparationRequest& request) override;

 private:
  MySqlConnectionPool& pool_;  ///< 共享连接池，不由本对象拥有。
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MATCH_PREPARATION_REPOSITORY_H_
