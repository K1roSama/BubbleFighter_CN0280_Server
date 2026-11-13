#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COORDINATOR_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COORDINATOR_H_

/**
 * @file room_coordinator.h
 * @brief 定义房间列表、创建、加入、离开与单房间命令流之间的串行协调边界。
 */

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "application/error.h"
#include "features/chat/user_message.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/rooms/room_command_flow.h"
#include "features/rooms/room_runtime.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {

/** @brief 协调器要求 Session 切换到的房间相关阶段；不包含登录或广场内部状态。
 */
enum class RoomSessionStage : std::uint8_t {
  kOutsideRoom,
  kWaiting,
  kLoading,
  kCountdown,
  kPlaying,
  kResults,
};

/** @brief 一名玩家在房间命令提交后必须按顺序应用的会话阶段更新。 */
struct RoomSessionUpdate final {
  domain::PlayerId player_id;  ///< 必须由 SessionRegistry 可定位的玩家。
  RoomSessionStage stage{
      RoomSessionStage::kWaiting};  ///< 提交消息投递前后的目标阶段。

  /** @brief 按玩家和目标房间阶段比较。 */
  bool operator==(const RoomSessionUpdate&) const = default;
};

/**
 * @brief 一次跨房间协调操作的完整内存结果。
 * @note deliveries
 * 已按每个接收者的客户端观察顺序排列；网络层只能搬入各自顺序发送队列， 不得按
 * opcode 重排。room_id 为空表示列表或玩家原本不属于任何房间。
 */
struct RoomCoordinatorResult final {
  RoomEvent event{RoomEvent::kNoChange};  ///< 权威房间状态事件。
  std::optional<domain::RoomId> room_id;  ///< 本次操作关联的房间。
  std::vector<RoomDelivery> deliveries;   ///< 按目标玩家确定的拥有型消息。
  std::vector<RoomSessionUpdate>
      session_updates;  ///< Session 层必须应用的阶段变化。
  std::vector<RoomTimerToken> active_timers;  ///< 操作后仍有效的房间计时令牌。
  std::optional<CombatRejectReason>
      combat_reject_reason;  ///< 安全忽略实时消息的审计原因。
  std::optional<std::string>
      rule_rejection_detail;  ///< 仅供服务端审计，不发送玩家。
  std::optional<application::MatchSettlementCommand>
      settlement_command;  ///< 进入结果态时必须先提交耐久 journal
                           ///< 的可靠结算命令。
};

/**
 * @brief 调用一次协调命令所需的已认证玩家上下文。
 * @note
 * 创建和加入必须携带同一只读事务产生的菜单快照；列表、离开和已在房间内的命令只需
 *       player_id。快照按值进入本结构，避免排队后借用 Session 或数据库内存。
 */
struct RoomActorContext final {
  domain::PlayerId player_id;  ///< 已认证玩家标识。
  std::optional<application::PlayerMenuSnapshot>
      player_snapshot;       ///< 创建/加入时必填。
  std::string peer_address;  ///< 认证 TCP 对端，仅用于房间直连候选。
  std::optional<application::IceLease> ice_lease;
};

/**
 * @brief 在 registry executor 上独占全部活动房间与命令流的同步协调器。
 * @note 本类不加锁、不查数据库、不持有 Session。调用方必须把 C2S、timer
 * 和断线离开事件 投递到同一个串行 executor；返回后再按 RoomDelivery 目标查找
 * Session。
 */
class RoomCoordinator final {
 public:
  [[nodiscard]] std::optional<application::UserHostPrizeTerms> UserHostPrize(
      domain::RoomId room_id) const;
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  AssetsCommitted(application::PlayerMenuSnapshot snapshot);
  [[nodiscard]] std::optional<application::UseItemCommand>
  PrepareInstantRespawn(domain::PlayerId player, std::uint32_t object) const;
  [[nodiscard]] bool AuthorizeInstantRespawn(
      const application::UseItemCommand& command);
  /** @brief 查询所在对局的死亡预设切换资格，不修改房间状态。 */
  [[nodiscard]] bool CanChangeCombatPreset(
      domain::PlayerId player) const noexcept;
  /** @brief 已提交换装的等待房间同步；成员已离开则不再推送。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  EquipmentChanged(application::PlayerMenuSnapshot snapshot);

  /** @brief 刷新已提交的家族徽章并广播原生 991，不改变准备或战斗状态。 */
  [[nodiscard]] RoomCoordinatorResult ClanMarkChanged(domain::PlayerId player,
                                                      std::uint32_t mark);
  /** @brief 使用统一对局计时配置创建空协调器。 */
  explicit RoomCoordinator(
      RoomMatchTiming timing = RoomMatchTiming{},
      std::optional<domain::RoomId> isolated_room = std::nullopt,
      std::uint16_t isolated_capacity = 12U);

  RoomCoordinator(const RoomCoordinator&) = delete;
  RoomCoordinator& operator=(const RoomCoordinator&) = delete;

  /**
   * @brief 严格处理 C2S401～404 或把房间内命令路由到对应 RoomCommandFlow。
   * @note 畸形负载抛出 ProtocolError；可展示的建房/加入失败编码为
   * S2C102/103，不产生 半完成成员关系；内部契约错误以 ServiceError 返回。
   */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> Handle(
      RoomActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 将已校验的附近/队伍聊天路由到玩家当前房间命令流。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> HandleChat(
      domain::PlayerId actor, bool team_only,
      const domain::UserMessage& message,
      std::span<const domain::PlayerId> blocked_peers);

  /**
   * @brief 将一个房间 timer 回调路由到原房间，并返回显式 Session 阶段变化。
   * @note 已销毁房间的迟到回调返回
   * kIgnoredStaleTimer，不投递消息或重新安排计时器。
   */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> ApplyTimer(
      const RoomTimerToken& token, std::chrono::steady_clock::time_point now);

  /** @brief 路由服务端权威终局事件，并返回需投递数据库工作池的完整结算命令。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> FinishMatch(
      domain::RoomId room_id, application::MatchCompletionKind completion,
      std::chrono::steady_clock::time_point now);

  /** @brief 在同一 executor 上替换房间成员的权威资料值快照。 */
  [[nodiscard]] application::ServiceResult<bool> UpdatePlayerSnapshot(
      application::PlayerMenuSnapshot snapshot);

  /** @brief 在指定 Playing 房间安装经目录解析的本局战斗参与者快照。 */
  [[nodiscard]] application::ServiceResult<bool> InstallCombat(
      domain::RoomId room_id, const RoomMatchInstallation& installation);

  /** @brief 安装完成后生成初始目标对象和计时器。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  InitialMatchEffects(domain::RoomId room_id,
                      std::chrono::steady_clock::time_point now);

  /** @brief 回滚指定房间未安装战斗账本的失败开局，并生成全员返回等待态消息。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  AbortFailedMatchStart(domain::RoomId room_id, std::string_view notice);

  /**
   * @brief 复制活动房间或已关闭房间仍等待 journal 回执的终局命令。
   * @note 最后一名成员离开后协调器保留命令
   * tombstone，直到确认耐久保存，避免销毁 flow 后 composition root
   * 丢失唯一内存副本。
   */
  [[nodiscard]] application::ServiceResult<application::MatchSettlementCommand>
  PendingSettlement(domain::RoomId room_id) const;

  /** @brief 按房间和 match_id 确认 journal 已保存；迟到或错局回执不得清除待办。
   */
  [[nodiscard]] application::ServiceResult<bool> AcknowledgeSettlementStaged(
      domain::RoomId room_id, const domain::MatchId& match_id);

  /** @brief 按原对局身份应用已提交奖励；旧局、已销毁或重复回执不污染新房间。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  ApplySettlementResult(const application::MatchSettlementResult& settlement,
                        std::chrono::steady_clock::time_point now);

  /** @brief 返回活动房间数，用于容量指标和测试；不暴露容器。 */
  [[nodiscard]] std::size_t RoomCount() const noexcept;

  /** @brief 按房间 ID 复制当前列表场景；不存在返回空。 */
  [[nodiscard]] std::optional<protocol::SceneInfo> Scene(
      domain::RoomId room_id) const;

  /** @brief 按房间 ID 顺序复制等待房间描述，供世界线程筛选快速进入候选。 */
  [[nodiscard]] std::vector<protocol::SceneInfo> WaitingScenes() const;

  /** @brief 返回当前房间成员最新的世界线程资料；离房或未知玩家返回空。 */
  [[nodiscard]] std::optional<application::PlayerMenuSnapshot> PlayerSnapshot(
      domain::PlayerId player) const;

  /** @brief 按玩家唯一成员索引取得只读运行时共享所有权；不存在返回空。 */
  [[nodiscard]] std::shared_ptr<RoomRuntime> FindRoom(
      domain::PlayerId player_id) const;

  /** @brief 按房间标识取得只读运行时共享所有权；不存在返回空。 */
  [[nodiscard]] std::shared_ptr<RoomRuntime> FindRoom(
      domain::RoomId room_id) const;

  /** @brief 已提交的图标偏好同步为完整 973，保留席位及准备状态。 */
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  LevelIconChanged(application::PlayerMenuSnapshot snapshot);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> CoupleChanged(
      application::PlayerMenuSnapshot snapshot);

 private:
  /** @brief 一个房间由 registry 中的 runtime 和此处唯一命令流共同组成。 */
  struct RoomEntry final {
    std::unique_ptr<RoomCommandFlow> flow;
  };

  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> HandleList(
      domain::PlayerId actor, const protocol::Packet& packet) const;
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> HandleCreate(
      RoomActorContext actor, const protocol::Packet& packet,
      std::uint16_t capacity = 12U);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  HandleSurvivalQueue(RoomActorContext actor, const protocol::Packet& packet);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  HandleTrainingEntry(RoomActorContext actor, const protocol::Packet& packet,
                      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> HandleJoin(
      RoomActorContext actor, const protocol::Packet& packet);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult> HandleLeave(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCoordinatorResult>
  HandleRoomCommand(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now,
      std::string_view peer_address = {},
      const std::optional<application::IceLease>& ice_lease = std::nullopt);

  [[nodiscard]] RoomCoordinatorResult Decorate(domain::RoomId room_id,
                                               RoomCommandResult result) const;
  void AppendPhaseUpdates(RoomCoordinatorResult& result,
                          const std::shared_ptr<RoomRuntime>& room) const;

  RoomMatchTiming timing_;
  std::uint16_t isolated_capacity_{12U};
  RoomRegistry registry_;
  std::map<domain::RoomId, RoomEntry> entries_;
  std::map<domain::RoomId, application::MatchSettlementCommand>
      closed_pending_settlements_;  ///< 房间已销毁但 journal 尚未确认的终局
                                    ///< tombstone。
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COORDINATOR_H_
