#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHAT_BLOCK_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHAT_BLOCK_STORE_H_

/** @file chat_block_store.h
 *  @brief 以玩家锁串行化 50 人上限与独立聊天名单持久化。
 */
#include "features/chat/chat_blocks.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 基于 MySQL 的聊天屏蔽名单仓储。 */
class MySqlChatBlockStore final : public application::ChatBlockStore {
 public:
  explicit MySqlChatBlockStore(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  [[nodiscard]] application::ServiceResult<
      std::vector<application::ChatBlockEntry>>
  Load(domain::PlayerId owner) override;
  [[nodiscard]] application::ServiceResult<application::ChatBlockEntry> Change(
      domain::PlayerId owner, const application::ChatBlockTarget& target,
      bool blocked) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CHAT_BLOCK_STORE_H_
