#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_REPOSITORY_H_

/**
 * @file social_repository.h
 * @brief 定义好友、屏蔽、私聊事务及一致性查询的 MySQL 适配器。
 */

#include "features/social/social_query.h"
#include "features/social/social_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 为社交写用例提供短事务，并为名单/历史提供 repeatable-read 只读快照。
 * @note 写事务按 player_id 升序锁双方资料；私聊正文只进入参数化
 * SQL，不写普通日志。
 *       对象自身不持有连接，可由有界数据库工作池中的多个线程共享。
 */
class MySqlSocialRepository final : public application::SocialUnitOfWork,
                                    public application::SocialSnapshotReader {
 public:
  /** @brief 注入生命周期长于本对象的连接池。 */
  explicit MySqlSocialRepository(MySqlConnectionPool& pool) noexcept;

  /** @brief 取得独占连接并创建未提交时自动回滚的社交写事务。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::SocialTransaction>>
  Begin() override;

  /** @brief 在同一只读快照中合并好友、申请、屏蔽和可选账号搜索。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::SocialOverviewSnapshot>>
  LoadOverview(const application::SocialOverviewRequest& request) override;

  /** @brief 在同一只读快照中复核关系并读取双方最近私聊。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::SocialConversationSnapshot>>
  LoadHistory(const application::SocialHistoryRequest& request) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_REPOSITORY_H_
