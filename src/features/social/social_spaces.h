#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SPACES_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SPACES_H_

/**
 * @file social_spaces.h
 * @brief 原版 NM 分组及多人通讯的持久化端口。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {
/** @brief 动作编号属于通讯扩展负载，不能当作原生协议 opcode。 */
enum class SocialSpaceAction : std::uint32_t {
  kSnapshot = 0,
  kCategoryCreate = 1,
  kCategoryRename = 2,
  kCategoryDelete = 3,
  kCategoryMove = 4,
  kCategoryRemove = 5,
  kChatCreate = 10,
  kChatInvite = 11,
  kChatAccept = 12,
  kChatDecline = 13,
  kChatLeave = 14,
  kChatMessage = 15,
  kChatHistory = 16
};

/** @brief actor 必须来自认证会话，目标和文本不能授予操作权限。 */
struct SocialSpaceCommand final {
  domain::PlayerId actor;
  SocialSpaceAction action{};
  std::uint32_t space{};
  std::uint32_t target{};
  std::u16string text;
};

/** @brief 好友分组是所有者的展示数据，不能替代真实好友关系校验。 */
struct FriendCategory final {
  std::uint32_t id{};
  std::string name;
  std::vector<domain::PlayerId> members;
};

/** @brief 昵称只是显示快照，授权使用稳定玩家身份。 */
struct ChatMember final {
  domain::PlayerId player;
  std::string nickname;
};

/** @brief 成员快照不替代发送消息时重新校验的持久化成员关系。 */
struct GroupChat final {
  std::uint32_t id{};
  std::vector<ChatMember> members;
};

/** @brief 待处理邀请不赋予成员身份或群聊发言权限。 */
struct GroupInvitation final {
  std::uint32_t chat{};
  ChatMember inviter;
};

/** @brief 各列表属于 player，不得作为其他玩家的授权快照复用。 */
struct SocialSpacesSnapshot final {
  domain::PlayerId player;
  std::vector<FriendCategory> categories;
  std::vector<GroupChat> chats;
  std::vector<GroupInvitation> invitations;
};

/** @brief recipients 必须由事务验证的成员关系产生，不能使用客户端广播名单。 */
struct GroupChatMessage final {
  std::uint64_t sequence{};
  std::uint32_t chat{};
  ChatMember sender;
  std::string text;
  std::vector<domain::PlayerId> recipients;
};

/** @brief 变更快照和消息只能在存储事务提交后发布。 */
struct SocialSpacesChange final {
  std::vector<SocialSpacesSnapshot> snapshots;
  std::vector<GroupChatMessage> messages;
};

/** @brief 同一事务必须验证 actor 权限并更新关系与历史。 */
class SocialSpacesStore {
 public:
  virtual ~SocialSpacesStore() = default;
  [[nodiscard]] virtual ServiceResult<SocialSpacesChange> Execute(
      const SocialSpaceCommand&) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SPACES_H_
