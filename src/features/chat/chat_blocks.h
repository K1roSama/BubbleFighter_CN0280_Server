#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHAT_CHAT_BLOCKS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHAT_CHAT_BLOCKS_H_

/** @file chat_blocks.h
 *  @brief 新版独立聊天屏蔽名单；不修改好友关系或好友黑名单。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {
/** @brief 聊天屏蔽名单中的一项：被屏蔽玩家及其当前昵称。 */
struct ChatBlockEntry final {
  std::uint32_t player_id{0U};
  std::string nickname;
};

/** @brief 客户端请求屏蔽/解除屏蔽的目标；可按玩家身份或仅按昵称指定。 */
struct ChatBlockTarget final {
  // 1302: 0x3a 带玩家身份；0xba 只有昵称。所有者只取已认证 session。
  std::uint32_t player_id{0U};
  std::string nickname;
};

/** @brief 聊天屏蔽名单持久化端口；所有者始终来自已认证会话。 */
class ChatBlockStore {
 public:
  virtual ~ChatBlockStore() = default;
  [[nodiscard]] virtual ServiceResult<std::vector<ChatBlockEntry>> Load(
      domain::PlayerId owner) = 0;
  [[nodiscard]] virtual ServiceResult<ChatBlockEntry> Change(
      domain::PlayerId owner, const ChatBlockTarget& target, bool blocked) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHAT_CHAT_BLOCKS_H_
