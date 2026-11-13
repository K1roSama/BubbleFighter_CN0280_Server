#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SERVICE_H_

/**
 * @file social_service.h
 * @brief 定义好友申请、关系、屏蔽和私聊的单事务应用用例。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "application/error.h"
#include "application/identifier.h"
#include "features/chat/user_message.h"
#include "features/social/social_target.h"

namespace bf::application {

inline constexpr std::uint32_t kMaximumFriends =
    100U;  ///< 旧服务好友人数上限。
inline constexpr std::uint32_t kMaximumPendingRequests =
    100U;  ///< 旧服务待处理申请压力上限。
inline constexpr std::uint32_t kMaximumBlockedPlayers =
    100U;  ///< 旧服务单向屏蔽名单上限。

/** @brief 会改变好友、申请或屏蔽状态的已发布动作。 */
enum class FriendAction : std::uint8_t {
  kAdd,
  kAccept,
  kDecline,
  kRemove,
  kBlock,
  kUnblock,
};

/** @brief 经认证玩家对目标登录账号执行的一次好友关系命令。 */
struct FriendChangeCommand final {
  domain::PlayerId actor_player_id;  ///< 由活动会话确定，不能接受客户端冒充。
  domain::SocialTarget
      target_account_name;  ///< 已按启动器账号规则解析的目标账号。
  FriendAction action{FriendAction::kAdd};  ///< 要执行的关系动作。
  std::optional<domain::UserMessage>
      request_message;  ///< 仅添加申请可携带的非空附言。
};

/** @brief 经认证玩家向目标登录账号发送的一条私聊。 */
struct SendPrivateMessageCommand final {
  domain::PlayerId sender_player_id;         ///< 由活动会话确定的发送者。
  domain::SocialTarget target_account_name;  ///< 已校验目标登录账号。
  domain::UserMessage message;  ///< 已通过 Unicode 与线协议长度规则的正文。
};

/** @brief 事务锁定双方资料后返回的权威身份快照。 */
struct SocialParticipants final {
  domain::PlayerId actor_player_id;   ///< 发起者玩家编号。
  domain::PlayerId target_player_id;  ///< 由目标账号唯一解析的玩家编号。
  std::string target_account_name;    ///< 数据库当前游戏昵称。
  std::string actor_nickname;
};

/**
 * @brief 双方锁定后读取的好友、申请、屏蔽和容量事实。
 * @note blocked_either_direction 为任意一方屏蔽；actor_has_block
 * 只表示发起者拥有该屏蔽行。
 */
struct SocialRelationshipSnapshot final {
  bool are_friends{false};                ///< 是否存在有序好友对。
  bool blocked_either_direction{false};   ///< 双方任意方向是否存在屏蔽。
  bool actor_has_block{false};            ///< 发起者是否主动屏蔽目标。
  bool incoming_request_exists{false};    ///< 是否存在目标发给发起者的申请。
  std::uint32_t actor_friend_count{0U};   ///< 发起者当前好友数。
  std::uint32_t target_friend_count{0U};  ///< 目标当前好友数。
  std::uint32_t pending_request_pressure{
      0U};                              ///< 旧行为的发出方/接收方申请压力计数。
  std::uint32_t actor_block_count{0U};  ///< 发起者当前单向屏蔽数。
};

/** @brief 已提交关系命令的权威目标和是否实际改变持久状态。 */
struct FriendChangeResult final {
  domain::PlayerId target_player_id;  ///< 用于提交后在线推送的目标玩家。
  std::string target_account_name;    ///< 数据库权威展示账号名。
  bool changed{false};                ///< 幂等删除/解除等无行可改时为 false。
};

/** @brief 已提交私聊的持久化消息游标和权威目标。 */
struct PrivateMessageResult final {
  domain::MessageId message_id;       ///< 数据库生成的稳定历史游标。
  domain::PlayerId target_player_id;  ///< 用于提交后在线推送的目标玩家。
  std::string target_account_name;    ///< 数据库权威游戏昵称。
  std::string sender_nickname;
};

/** @brief 发送目标查找参数；实现须同时锁定发起者与目标资料。 */
struct SocialTargetLookup final {
  domain::PlayerId actor_player_id;          ///< 已认证发起者。
  domain::SocialTarget target_account_name;  ///< 唯一登录键来源。
};

/** @brief 好友申请插入参数；空附言与没有附言语义相同。 */
struct FriendRequestWrite final {
  SocialParticipants participants;                     ///< 已锁定的双方身份。
  std::optional<domain::UserMessage> request_message;  ///< 首次申请附言。
};

/** @brief 私聊插入参数；消息正文不得进入普通日志。 */
struct PrivateMessageWrite final {
  SocialParticipants participants;  ///< 已锁定并确认关系的双方身份。
  domain::UserMessage message;      ///< 持久化正文。
};

/**
 * @brief 社交用例独占的短事务端口。
 * @note 所有调用使用同一连接和事务；实现须按玩家 ID
 * 升序锁双方资料，避免互发操作死锁。
 */
class SocialTransaction {
 public:
  virtual ~SocialTransaction() = default;

  /** @brief
   * 解析目标账号并按稳定顺序锁定双方玩家；目标不存在返回可展示的输入错误。 */
  [[nodiscard]] virtual ServiceResult<SocialParticipants> LockParticipants(
      const SocialTargetLookup& lookup) = 0;

  /** @brief 在双方锁仍持有时读取关系与上限计数，供应用层统一判定。 */
  [[nodiscard]] virtual ServiceResult<SocialRelationshipSnapshot>
  InspectRelationship(const SocialParticipants& participants) = 0;

  /** @brief 首次写入好友申请；重复方向不覆盖原申请时间或附言。 */
  [[nodiscard]] virtual ServiceResult<bool> InsertFriendRequestIfAbsent(
      const FriendRequestWrite& write) = 0;

  /** @brief 插入有序好友对；返回 false 表示事务事实与应用快照冲突。 */
  [[nodiscard]] virtual ServiceResult<bool> InsertFriendshipIfAbsent(
      const SocialParticipants& participants) = 0;

  /** @brief 删除双方好友关系；不存在时幂等返回 false。 */
  [[nodiscard]] virtual ServiceResult<bool> DeleteFriendship(
      const SocialParticipants& participants) = 0;

  /** @brief 删除双方任意方向的当前好友申请并返回是否删除过行。 */
  [[nodiscard]] virtual ServiceResult<bool> DeleteRequestsBetween(
      const SocialParticipants& participants) = 0;

  /** @brief 只删除目标发给发起者的申请并返回是否删除过行。 */
  [[nodiscard]] virtual ServiceResult<bool> DeleteIncomingRequest(
      const SocialParticipants& participants) = 0;

  /** @brief 插入发起者到目标的单向屏蔽；重复操作幂等返回 false。 */
  [[nodiscard]] virtual ServiceResult<bool> InsertBlockIfAbsent(
      const SocialParticipants& participants) = 0;

  /** @brief 删除发起者拥有的单向屏蔽；不存在时幂等返回 false。 */
  [[nodiscard]] virtual ServiceResult<bool> DeleteOwnedBlock(
      const SocialParticipants& participants) = 0;

  /** @brief 写入一条私聊并返回生成的非零消息游标。 */
  [[nodiscard]] virtual ServiceResult<domain::MessageId> InsertPrivateMessage(
      const PrivateMessageWrite& write) = 0;

  /** @brief 原子发布本次关系变化或私聊；必须是最后一次事务调用。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 社交事务工厂，由数据库工作线程上的 MySQL adapter 实现。 */
class SocialUnitOfWork {
 public:
  virtual ~SocialUnitOfWork() = default;

  /** @brief 开始短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<std::unique_ptr<SocialTransaction>>
  Begin() = 0;
};

/** @brief 社交运行时依赖的好友关系写用例接口。 */
class FriendRelationshipChanger {
 public:
  virtual ~FriendRelationshipChanger() = default;

  /** @brief 执行已认证关系命令；只有成功返回才允许刷新或推送名单。 */
  [[nodiscard]] virtual ServiceResult<FriendChangeResult> Change(
      const FriendChangeCommand& command) const = 0;
};

/** @brief 社交运行时依赖的私聊持久化用例接口。 */
class PrivateMessageSender {
 public:
  virtual ~PrivateMessageSender() = default;

  /** @brief 持久化已认证私聊；只有提交后的成功结果才允许在线推送。 */
  [[nodiscard]] virtual ServiceResult<PrivateMessageResult> Send(
      const SendPrivateMessageCommand& command) const = 0;
};

/** @brief 在一个数据库事务内执行好友、申请和屏蔽状态变化。 */
class FriendRelationshipService final : public FriendRelationshipChanger {
 public:
  /** @brief 注入事务工厂；其生命周期必须覆盖本服务。 */
  explicit FriendRelationshipService(SocialUnitOfWork& unit_of_work) noexcept;

  /** @brief 执行已认证关系命令；失败时不发布任何在线推送。 */
  [[nodiscard]] ServiceResult<FriendChangeResult> Change(
      const FriendChangeCommand& command) const override;

 private:
  SocialUnitOfWork& unit_of_work_;
};

/** @brief 只允许未屏蔽的既有好友在一个事务中持久化私聊。 */
class PrivateMessageService final : public PrivateMessageSender {
 public:
  /** @brief 注入与关系服务共用的事务工厂。 */
  explicit PrivateMessageService(SocialUnitOfWork& unit_of_work) noexcept;

  /** @brief 锁定双方、复核关系并写入私聊；提交后结果才可用于在线推送。 */
  [[nodiscard]] ServiceResult<PrivateMessageResult> Send(
      const SendPrivateMessageCommand& command) const override;

 private:
  SocialUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_SOCIAL_SERVICE_H_
