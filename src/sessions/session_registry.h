#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_REGISTRY_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_REGISTRY_H_

/**
 * @file session_registry.h
 * @brief 定义连接注册、在线玩家 RAII 租约和房间结果到 Session mailbox
 * 的并发投递边界。
 */

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/rooms/room_coordinator.h"
#include "features/scenes/plaza_coordinator.h"
#include "sessions/gateway_login_flow.h"
#include "sessions/outbound_message.h"
#include "sessions/social_command_flow.h"
#include "sessions/world_coordinator.h"

namespace bf::runtime {

/** @brief 进程内 Session 实例和代次组成的不可混用句柄。 */
struct SessionHandle final {
  std::uint64_t id{0U};          ///< 进程内单调连接编号。
  std::uint64_t generation{0U};  ///< 防止迟到任务命中新注册连接的代次。

  /** @brief 只有编号和代次都非零时才可注册或投递。 */
  [[nodiscard]] bool IsValid() const noexcept;

  /** @brief 按连接编号和代次提供稳定全序。 */
  auto operator<=>(const SessionHandle&) const = default;
};

/** @brief registry 一次投递给单个 Session strand 的拥有型原子批次。 */
struct SessionDispatch final {
  std::vector<OutboundMessage>
      messages;  ///< 保持协调器给该玩家的原始消息顺序。
  std::optional<RoomSessionStage>
      room_stage;  ///< 可选房间阶段，与消息在同一任务中应用。
  std::optional<PlazaSessionStage>
      plaza_stage;  ///< 可选广场阶段，与消息在同一任务中应用。
  std::optional<WorldSessionPhase>
      world_phase;                       ///< 组合世界协调器给出的最终阶段。
  std::optional<std::uint32_t> channel;  ///< 组合协调器明确要求更新的频道。
  bool disconnect{false};  ///< Server-only moderation request, never decoded
                           ///< from client input.
  bool flush_before_disconnect{
      false};  ///< Server-only final notice; drain for at most 500ms.
};

/**
 * @brief Session 对 registry 暴露的线程安全 mailbox 端口。
 * @note 实现只能快速把拥有型批次 post 到自己的 strand，不能在 registry
 * 调用线程阻塞 I/O、 等待数据库或同步重入房间协调器。
 */
class SessionMailbox {
 public:
  virtual ~SessionMailbox() = default;

  /** @brief 接受一个拥有型批次；关闭/过载时返回显式错误，不得返回假成功。 */
  [[nodiscard]] virtual std::optional<application::ServiceError> Post(
      SessionDispatch dispatch) = 0;
};

/** @brief 一名玩家未找到活动 Session 或 Session 拒绝批次时的投递结果。 */
struct SessionDispatchFailure final {
  domain::PlayerId player_id;       ///< 失败目标玩家。
  application::ServiceError error;  ///< 已脱敏失败原因。
};

/** @brief 一次多 Session 投递的可审计汇总；跨 Session 不承诺全有或全无。 */
struct SessionDispatchReport final {
  std::size_t submitted_sessions{0U};  ///< mailbox 已接受的 Session 数。
  std::vector<domain::PlayerId> offline_players;  ///< 投递时无活动玩家绑定。
  std::vector<SessionDispatchFailure>
      rejected_batches;  ///< mailbox 明确拒绝的批次。
};

/** @brief 只供 RAII 注册和租约共享生命周期的隐藏 registry 状态。 */
class SessionRegistrySharedState;

/**
 * @brief 一个 Session 在 registry 中的移动型 RAII 注册。
 * @note 析构先删除
 * Session，再释放仍属于该句柄的玩家绑定；迟到的在线租约析构通过句柄比较
 *       不会删除新连接的绑定。
 */
class SessionRegistration final {
 public:
  /** @brief 构造不关联 Session 的空注册。 */
  SessionRegistration() noexcept = default;

  /** @brief 注销仍活动的句柄及其玩家绑定；空注册不执行操作。 */
  ~SessionRegistration();

  /** @brief 转移注册所有权，并将来源对象置空。 */
  SessionRegistration(SessionRegistration&& other) noexcept;

  /** @brief 先注销当前句柄，再转移来源对象的注册所有权。 */
  SessionRegistration& operator=(SessionRegistration&& other) noexcept;

  /** @brief 注册代表唯一句柄所有权，因此禁止复制构造。 */
  SessionRegistration(const SessionRegistration&) = delete;

  /** @brief 注册代表唯一句柄所有权，因此禁止复制赋值。 */
  SessionRegistration& operator=(const SessionRegistration&) = delete;

  /** @brief 返回本注册的稳定句柄；已移动对象返回零值。 */
  [[nodiscard]] SessionHandle Handle() const noexcept;

  /**
   * @brief 创建只为本 Session 认领玩家的 `OnlinePlayerRegistry` 适配器。
   * @note 适配器和其租约可晚于注册对象析构；所有操作都会校验句柄仍活动。
   */
  [[nodiscard]] std::unique_ptr<OnlinePlayerRegistry> MakeOnlinePlayerRegistry()
      const;

  /** @brief 返回注册是否仍持有活动句柄。 */
  [[nodiscard]] bool Active() const noexcept;

 private:
  friend class SessionRegistry;
  SessionRegistration(SessionHandle handle,
                      std::weak_ptr<SessionRegistrySharedState> state);
  void Release() noexcept;

  SessionHandle handle_;
  std::weak_ptr<SessionRegistrySharedState> state_;
};

/**
 * @brief 线程安全维护 Session mailbox、每 Session 单一玩家和每玩家单一 Session
 * 的注册表。
 * @note mutex 只保护索引；`SessionMailbox::post` 一律在解锁后调用。房间
 * executor 可把 `RoomCoordinatorResult` 整体移入 `dispatch`，无需持有 Session
 * 指针。
 */
class SessionRegistry final : public SocialPresenceView {
 public:
  /** @brief 创建有界 registry；maximum_sessions 为零时抛出
   * `std::invalid_argument`。 */
  explicit SessionRegistry(std::size_t maximum_sessions = 4096U);

  /** @brief registry 持有全局句柄索引，因此禁止复制构造。 */
  SessionRegistry(const SessionRegistry&) = delete;

  /** @brief registry 持有全局句柄索引，因此禁止复制赋值。 */
  SessionRegistry& operator=(const SessionRegistry&) = delete;

  /** @brief 注册一个非空 mailbox 并返回 RAII 注册；容量耗尽返回 `kOverloaded`。
   */
  [[nodiscard]] application::ServiceResult<SessionRegistration> RegisterSession(
      const std::shared_ptr<SessionMailbox>& mailbox);

  /**
   * @brief 按玩家聚合协调器消息和阶段更新，再投递到对应 mailbox。
   * @return 重复且冲突的阶段更新、无效玩家 ID 等协调器契约错误返回
   * ServiceError；离线或 单个 mailbox 拒绝则记录在
   * report，不阻止其他玩家收到自己的批次。
   */
  [[nodiscard]] application::ServiceResult<SessionDispatchReport> Dispatch(
      RoomCoordinatorResult result);

  /**
   * @brief 按玩家聚合广场消息和阶段更新，再投递到对应 mailbox。
   * @return 与房间投递相同：协调器契约错误整体失败，离线或单 mailbox
   * 拒绝进入报告。
   */
  [[nodiscard]] application::ServiceResult<SessionDispatchReport> Dispatch(
      PlazaCoordinatorResult result);

  /**
   * @brief 投递组合世界结果；消息、最终阶段与可选频道在同一个 mailbox
   * 任务中应用。
   * @return 同一玩家存在冲突阶段/频道时，在任何 mailbox 调用前返回契约错误。
   */
  [[nodiscard]] application::ServiceResult<SessionDispatchReport> Dispatch(
      WorldCoordinatorResult result);

  /**
   * @brief 按目标玩家投递已提交的社交回复与推送，不附带世界或房间阶段变化。
   * @return 无效目标属于运行时契约错误；离线或 mailbox 拒绝仍进入可审计
   * report。
   */
  [[nodiscard]] application::ServiceResult<SessionDispatchReport> Dispatch(
      SocialCommandResult result);

  /**
   * @brief 更新指定活动 Session 的扩展社交协商状态；必须已认领在线玩家。
   * @note native_social=true 时 extended 必须同时为
   * true；句柄代次不匹配时拒绝迟到更新。
   */
  [[nodiscard]] std::optional<application::ServiceError> SetSocialCapabilities(
      SessionHandle handle, bool extended, bool native_social);

  /** @brief 返回玩家当前在线及扩展社交能力快照，不暴露 Session 或 mailbox。 */
  [[nodiscard]] SocialPresenceSnapshot Lookup(
      domain::PlayerId player_id) const override;

  /**
   * @brief 判断玩家是否仍由给定主网关 Session 持有唯一在线租约。
   * @note 战队子连接用此绑定主会话生命周期；句柄代次不匹配或 mailbox
   *       已释放均返回 false，不延长主会话寿命。
   */
  [[nodiscard]] bool IsCurrentPlayerSession(domain::PlayerId player_id,
                                            SessionHandle handle) const;

  /** @brief 冻结玩家当前主会话句柄；不延长租约，使用时仍须复核。 */
  [[nodiscard]] std::optional<SessionHandle> CurrentPlayerSession(
      domain::PlayerId player) const;
  void SetWorldState(SessionHandle handle, WorldSessionState state);
  [[nodiscard]] std::optional<WorldSessionState> WorldState(
      domain::PlayerId player) const;
  [[nodiscard]] application::ServiceResult<bool> DispatchIfCurrent(
      domain::PlayerId player, SessionHandle handle,
      std::vector<OutboundMessage> messages);
  /** @brief
   * 冻结句柄原子选择mailbox后投递消息和phase，旧玩家绑定不能命中新连接。 */
  [[nodiscard]] application::ServiceResult<bool> DispatchIfCurrent(
      domain::PlayerId player, SessionHandle handle, SessionDispatch dispatch);

  /** @brief 返回当前 RAII 注册仍活动的 Session 数。 */
  [[nodiscard]] std::size_t SessionCount() const;
  /** @brief 返回当前持有在线租约的玩家数。 */
  [[nodiscard]] std::size_t OnlinePlayerCount() const;
  [[nodiscard]] std::vector<std::pair<domain::PlayerId, SessionHandle>>
  OnlineSessions() const;
  void DisconnectIfCurrent(domain::PlayerId player, SessionHandle handle);

 private:
  std::shared_ptr<SessionRegistrySharedState> state_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_SESSION_REGISTRY_H_
