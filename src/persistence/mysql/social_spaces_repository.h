#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_SPACES_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_SPACES_REPOSITORY_H_

/**
 * @file social_spaces_repository.h
 * @brief 好友分组、群邀请、成员和消息的 MySQL 事务实现。
 */
#include "features/social/social_spaces.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 关系、邀请和消息历史必须在同一事务验证 actor 授权后变更。 */
class MySqlSocialSpacesRepository final
    : public application::SocialSpacesStore {
 public:
  explicit MySqlSocialSpacesRepository(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  [[nodiscard]] application::ServiceResult<application::SocialSpacesChange>
  Execute(const application::SocialSpaceCommand&) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SOCIAL_SPACES_REPOSITORY_H_
