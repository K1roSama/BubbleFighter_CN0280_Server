#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_QUERY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_QUERY_H_

/**
 * @file social_query.h
 * @brief 定义好友名单、申请和最近私聊的一致性只读用例。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/chat/user_message.h"
#include "features/social/social_target.h"

namespace bf::application {

inline constexpr std::uint32_t kMaximumPrivateHistoryMessages =
    50U;  ///< 旧服务每次返回的最近私聊上限。

/** @brief 名单中一名对端相对于当前玩家的关系状态。 */
enum class SocialRosterState : std::uint8_t {
  kSearchResult = 0U,
  kFriend = 1U,
  kIncomingRequest = 2U,
  kOutgoingRequest = 3U,
  kBlocked = 4U,
};

/** @brief 好友名单或搜索结果中的权威账号记录；在线状态由运行时补充。 */
struct SocialRosterRecord final {
  domain::PlayerId player_id;  ///< 对端玩家标识。
  std::string account_name;    ///< 数据库保留大小写的展示账号名。
  SocialRosterState state{
      SocialRosterState::kSearchResult};  ///< 当前关系状态。
};

/** @brief 一条发给当前玩家、可在原生通讯面板弹出的好友申请。 */
struct SocialInvitationRecord final {
  domain::PlayerId sender_player_id;  ///< 申请发起玩家。
  std::string sender_account_name;    ///< 发起者权威展示账号名。
  std::optional<domain::UserMessage> request_message;  ///< 可选申请附言。
};

/** @brief 同一只读快照中的合并名单和有序收到申请。 */
struct SocialOverviewSnapshot final {
  domain::PlayerId owner_player_id;  ///< 快照所属玩家。
  std::vector<SocialRosterRecord>
      roster;  ///< 持久关系按玩家 ID 升序，搜索项可在末尾。
  std::vector<SocialInvitationRecord>
      invitations;  ///< 收到申请按创建时间、发送者排序。
};

/** @brief 最近私聊中的一条持久化记录。 */
struct SocialHistoryRecord final {
  domain::MessageId message_id;       ///< 单调历史游标。
  domain::PlayerId sender_player_id;  ///< 实际发言者。
  domain::PlayerId target_player_id;  ///< 实际接收者。
  domain::UserMessage message;        ///< 已验证正文。
};

/** @brief 与一个指定账号的最近会话及双方权威显示身份。 */
struct SocialConversationSnapshot final {
  domain::PlayerId actor_player_id;  ///< 发起查询的玩家。
  std::string actor_account_name;    ///< 发起者数据库展示账号名。
  domain::PlayerId peer_player_id;   ///< 查询的会话对端。
  std::string peer_account_name;     ///< 对端数据库展示账号名。
  bool relationship_allowed{false};  ///< 读取时双方仍为好友且均未屏蔽。
  std::vector<SocialHistoryRecord>
      messages;  ///< 最后 50 条，按 message_id 严格升序。
};

/** @brief 名单读取参数；search_account 仅用于把无关系搜索结果附在名单末尾。 */
struct SocialOverviewRequest final {
  domain::PlayerId player_id;                          ///< 当前认证玩家。
  std::optional<domain::SocialTarget> search_account;  ///< 可选目标账号。
};

/** @brief 历史读取参数；reader 必须在同一快照复核好友与双向屏蔽。 */
struct SocialHistoryRequest final {
  domain::PlayerId player_id;           ///< 当前认证玩家。
  domain::SocialTarget target_account;  ///< 对端登录账号。
  std::uint32_t limit{
      kMaximumPrivateHistoryMessages};  ///< 固定上界，供 SQL LIMIT 参数化。
};

/**
 * @brief 社交只读数据库端口。
 * @note 每次调用必须使用一个只读一致性事务；overview
 * 不得在多次查询间混入关系变化，history
 *       必须在读取消息的同一快照中复核好友和任一方向屏蔽。
 */
class SocialSnapshotReader {
 public:
  virtual ~SocialSnapshotReader() = default;

  /** @brief 读取名单、收到申请及可选搜索项；玩家不存在返回空值。 */
  [[nodiscard]] virtual ServiceResult<std::optional<SocialOverviewSnapshot>>
  LoadOverview(const SocialOverviewRequest& request) = 0;

  /** @brief 读取最近会话；目标账号不存在返回空值，关系事实由结果显式给出。 */
  [[nodiscard]] virtual ServiceResult<std::optional<SocialConversationSnapshot>>
  LoadHistory(const SocialHistoryRequest& request) = 0;
};

/** @brief 社交运行时依赖的、已完成数据库契约校验的只读接口。 */
class SocialSnapshotLoader {
 public:
  virtual ~SocialSnapshotLoader() = default;

  /** @brief 返回可直接映射到 roster/invitation 协议的权威快照。 */
  [[nodiscard]] virtual ServiceResult<SocialOverviewSnapshot> LoadOverview(
      const SocialOverviewRequest& request) const = 0;

  /** @brief 返回最多 50 条可直接按时间顺序显示的好友私聊。 */
  [[nodiscard]] virtual ServiceResult<SocialConversationSnapshot> LoadHistory(
      const SocialHistoryRequest& request) const = 0;
};

/** @brief 校验社交 reader 的身份、排序、容量和关系快照，再交给协议运行时。 */
class SocialQueryService final : public SocialSnapshotLoader {
 public:
  /** @brief 注入只读数据库端口；其生命周期必须覆盖本服务。 */
  explicit SocialQueryService(SocialSnapshotReader& reader) noexcept;

  /** @brief 读取并校验名单快照。 */
  [[nodiscard]] ServiceResult<SocialOverviewSnapshot> LoadOverview(
      const SocialOverviewRequest& request) const override;

  /** @brief 读取并校验好友私聊快照。 */
  [[nodiscard]] ServiceResult<SocialConversationSnapshot> LoadHistory(
      const SocialHistoryRequest& request) const override;

 private:
  SocialSnapshotReader& reader_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_QUERY_H_
