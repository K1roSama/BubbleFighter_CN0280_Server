#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_COMMAND_FLOW_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_COMMAND_FLOW_H_

/**
 * @file social_command_flow.h
 * @brief 定义单个已认证 Session 的扩展社交命令编排与提交后推送边界。
 */

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "application/identifier.h"
#include "features/authentication/account_credentials.h"
#include "features/social/social_query.h"
#include "features/social/social_service.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/social/social_messages.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/** @brief 在线 Session 对本地扩展协议的可投递能力快照。 */
struct SocialPresenceSnapshot final {
  bool online{false};         ///< 当前是否持有活动在线租约。
  bool extended{false};       ///< 是否支持 650xx 本地扩展 opcode。
  bool native_social{false};  ///< 是否已协商原生 65013/65015 社交布局。
};

/** @brief 社交编排只读的在线能力端口；实现不得返回 Session 裸指针。 */
class SocialPresenceView {
 public:
  virtual ~SocialPresenceView() = default;

  /** @brief 返回一次无阻塞能力快照；离线时三个标志都应为 false，native
   * 必须同时为 extended。 */
  [[nodiscard]] virtual SocialPresenceSnapshot Lookup(
      domain::PlayerId player_id) const = 0;
};

/** @brief 一条已确定目标玩家的社交出站消息。 */
struct SocialDelivery final {
  domain::PlayerId
      recipient;            ///< 由认证身份或提交结果确定，不接受客户端玩家 ID。
  OutboundMessage message;  ///< 交给目标 Session 顺序写队列的拥有型消息。

  /** @brief 按接收玩家和完整出站消息比较。 */
  bool operator==(const SocialDelivery&) const = default;
};

/** @brief 一条 C2S65010 完成后的全部提交后消息。 */
struct SocialCommandResult final {
  std::vector<SocialDelivery> deliveries;  ///< 每名玩家内部保持可观察顺序。
};

/** @brief 创建 Session 社交流所需的不可变认证身份。 */
struct SocialActorIdentity final {
  domain::PlayerId player_id;        ///< 登录票据确定的玩家标识。
  domain::AccountName account_name;  ///< 登录快照确定的展示账号及唯一键。
};

/**
 * @brief 路由 C2S65010 的名单、搜索、历史、关系变化和私聊。
 * @note 本对象由一个 Session strand 独占，并持有该 Session 的 native_social
 * 协商状态和 10 秒聊天限流窗口。同步应用端口将在未来由 DB 工作池调用后回到同一
 * strand；本类不 持有数据库事务或目标 Session。
 */
class SocialCommandFlow final {
 public:
  /** @brief 注入认证身份、读写用例和在线能力视图；引用生命周期必须覆盖本对象。
   */
  SocialCommandFlow(SocialActorIdentity actor,
                    application::SocialSnapshotLoader& snapshots,
                    application::FriendRelationshipChanger& relationships,
                    application::PrivateMessageSender& private_messages,
                    const SocialPresenceView& presence);

  /**
   * @brief 处理一条 C2S65010；畸形字节抛 ProtocolError，存储/契约故障返回
   * ServiceError。
   * @note 可展示的输入、关系和限流错误被编码为当前名单状态，不返回假业务成功。
   */
  [[nodiscard]] application::ServiceResult<SocialCommandResult> Handle(
      const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 返回本 Session 是否已通过 native 动作启用 65013/65015 布局。 */
  [[nodiscard]] bool NativeSocialEnabled() const noexcept;

 private:
  [[nodiscard]] application::ServiceResult<SocialCommandResult> HandleOverview(
      const protocol::SocialRequest& request, bool enable_native);
  [[nodiscard]] application::ServiceResult<SocialCommandResult> HandleHistory(
      const protocol::SocialRequest& request);
  [[nodiscard]] application::ServiceResult<SocialCommandResult>
  HandlePrivateMessage(const protocol::SocialRequest& request);
  [[nodiscard]] application::ServiceResult<SocialCommandResult>
  HandleRelationshipChange(const protocol::SocialRequest& request);
  [[nodiscard]] application::ServiceResult<SocialCommandResult>
  DisplayBusinessError(const application::ServiceError& error);
  [[nodiscard]] bool ConsumeQuota(std::chrono::steady_clock::time_point now);

  SocialActorIdentity actor_;
  application::SocialSnapshotLoader& snapshots_;
  application::FriendRelationshipChanger& relationships_;
  application::PrivateMessageSender& private_messages_;
  const SocialPresenceView& presence_;
  bool native_social_{false};
  std::deque<std::chrono::steady_clock::time_point> command_times_;
};

[[nodiscard]] application::ServiceResult<SocialCommandResult>
SocialPresenceChanged(domain::PlayerId player,
                      application::SocialSnapshotLoader& snapshots,
                      const SocialPresenceView& presence);

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_COMMAND_FLOW_H_
