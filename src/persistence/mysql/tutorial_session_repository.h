#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TUTORIAL_SESSION_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TUTORIAL_SESSION_REPOSITORY_H_

/**
 * @file tutorial_session_repository.h
 * @brief MySQL原103客户端报告账本，不访问正式比赛或奖励表。
 */

#include "features/practice/tutorial_session.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 以认证玩家行锁和不可变UUID实现一次开始/终止事务的教程适配器。 */
class MySqlTutorialSessionRepository final
    : public application::TutorialSessionStore {
 public:
  explicit MySqlTutorialSessionRepository(MySqlConnectionPool& pool) noexcept;
  [[nodiscard]] application::ServiceResult<bool> Begin(
      const application::TutorialSessionStart& command) override;
  [[nodiscard]] application::ServiceResult<bool> Finish(
      const application::TutorialSessionFinish& command) override;

 private:
  [[nodiscard]] application::ServiceResult<bool> BeginSinglePlay(
      const application::TutorialSessionStart& command);
  [[nodiscard]] application::ServiceResult<bool> FinishSinglePlay(
      const application::TutorialSessionFinish& command);
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_TUTORIAL_SESSION_REPOSITORY_H_
