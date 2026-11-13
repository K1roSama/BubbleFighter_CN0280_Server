#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_COORDINATOR_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_COORDINATOR_H_

/**
 * @file world_coordinator.h
 * @brief 定义菜单、自由广场与房间之间的串行切换和命令路由边界。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <variant>
#include <vector>

#include "application/error.h"
#include "features/battle/ice_service.h"
#include "features/chat/megaphone.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/practice/local_horde_tutor.h"
#include "features/practice/local_single_play.h"
#include "features/rooms/room_coordinator.h"
#include "features/scenes/plaza_directory.h"
#include "protocol/codec/frame_codec.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/** @brief Session 登录完成后可观察的世界/房间阶段。 */
enum class WorldSessionPhase : std::uint8_t {
  kMenu,
  kPlaza,
  kRoomWaiting,
  kRoomLoading,
  kRoomCountdown,
  kRoomPlaying,
  kRoomResults,
  kTutorialLoading,    ///< 原103本地单人场景，不属于竞技房间注册表。
  kTutorialPlaying,    ///< 仅有client_reported教程握手，不有正式比赛计分权限。
  kSinglePlayWaiting,  ///< 原15终局736后的Room4私有等待，允许原107重开/退出。
};

/** @brief Session 提交世界命令时携带的当前阶段和频道。 */
struct WorldSessionState final {
  WorldSessionPhase phase{
      WorldSessionPhase::kMenu};  ///< 调用 strand 当前权威阶段。
  std::uint32_t channel{0U};      ///< 0 未选、60 自由、80 家族。

  /** @brief 按世界阶段和频道比较。 */
  bool operator==(const WorldSessionState&) const = default;
};

/**
 * @brief 一名玩家在 world executor 操作后必须应用的阶段变化。
 * @note channel 为空表示保留该 Session 的既有频道；房间 timer
 * 无法也不应改写频道。
 */
struct WorldSessionUpdate final {
  domain::PlayerId player_id;  ///< SessionRegistry 可定位的玩家。
  WorldSessionPhase phase{
      WorldSessionPhase::kMenu};         ///< 操作完成后的世界阶段。
  std::optional<std::uint32_t> channel;  ///< 可选的新频道编号。

  /** @brief 按玩家、世界阶段和可选频道比较。 */
  bool operator==(const WorldSessionUpdate&) const = default;
};

/** @brief 一条由世界协调器确定接收玩家的拥有型消息。 */
struct WorldDelivery final {
  domain::PlayerId recipient;  ///< 接收消息的已认证玩家。
  OutboundMessage message;     ///< 保持协议观察顺序的消息值。

  /** @brief 按接收玩家和完整出站消息比较。 */
  bool operator==(const WorldDelivery&) const = default;
};

/** @brief 一次世界命令的全部消息、阶段变化和有效房间 timer。 */
struct WorldCoordinatorResult final {
  RoomEvent event{
      RoomEvent::kNoChange};  ///< 房间状态事件；广场命令保持无变化。
  std::optional<domain::RoomId> room_id;  ///< 房间命令关联标识。
  std::vector<WorldDelivery> deliveries;  ///< 按每名玩家观察顺序排列。
  std::vector<WorldSessionUpdate>
      session_updates;                        ///< Session strand 应用的变化。
  std::vector<RoomTimerToken> active_timers;  ///< 操作后需要调度的房间 timer。
  std::optional<CombatRejectReason>
      combat_reject_reason;  ///< 安全忽略实时消息的审计原因。
  std::optional<std::string>
      rule_rejection_detail;  ///< 仅供服务端审计，不发送玩家。
  std::optional<application::MatchSettlementCommand>
      settlement_command;  ///< 移交异步
                           ///< journal/结算桥接器、不得在网络线程执行的命令。
  std::optional<std::variant<application::TutorialSessionStart,
                             application::TutorialSessionFinish>>
      tutorial_command;  ///< 独立103客户端报告账本，绝非竞技对局或奖励事务。
  std::optional<std::pair<domain::PlayerId, domain::MatchId>>
      local_release_owner;
};

/**
 * @brief 一次已认证世界命令所需的 Session 值上下文。
 * @note 进入场景、建房和加入必须携带最新一致性资料快照；`has_clan_membership`
 * 只允许来自 已完成的应用/数据库查询，客户端不能自报。
 */
struct WorldActorContext final {
  domain::PlayerId player_id;  ///< 已认证玩家标识。
  WorldSessionState state;     ///< 所属 strand 当前阶段。
  std::optional<application::PlayerMenuSnapshot>
      player_snapshot;              ///< 场景快照输入。
  bool has_clan_membership{false};  ///< 是否有权进入频道 80。
  std::vector<domain::PlayerId>
      chat_blocked_peers;  ///< 与本人任一方向屏蔽、不得接收聊天的玩家。
  bool muted{false};
  bool has_chat_penalty_history{
      false};  ///< 由数据库访问快照传入，客户端不能自报。
  std::size_t online_player_count{
      0U};  ///< world executor 从在线租约表读取的全服人数快照。
  std::optional<domain::MatchId>
      tutorial_session_id;  ///< 原107由执行器分配的独立UUID，不接收客户端ID。
  std::optional<std::pair<std::uint64_t, std::uint64_t>>
      orig_session;  ///< 认证网关句柄(id,generation)，非客户端输入。
  std::string
      peer_address;  ///< 网关 TCP 对端；P2P 候选公网地址不能由客户端自报。
  std::optional<application::IceLease> ice_lease;
};

/**
 * @brief 在单一 world executor 上独占广场目录与 RoomCoordinator 的组合根。
 * @note
 * 场景切换串行执行；失败时撤销成员变更，不能留下双重成员关系。
 */
class WorldCoordinator final {
 public:
  /** @brief 原103独立教程的加载期限，永不调度180s或普通竞技超时。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  TutorialDeadline() const;
  [[nodiscard]] std::vector<WorldCoordinatorResult> TickTutorials(
      std::chrono::steady_clock::time_point now);
  /** @brief 数据库确认后才发布教程场景或原736返回，不改竞技记录。 */
  void TutorialAcknowledge(domain::PlayerId player,
                           const domain::MatchId& session, bool begin,
                           bool publish_phase = true);
  void TutorialBeginFailed(domain::PlayerId player,
                           const domain::MatchId& session,
                           bool publish_phase = true);
  /** @brief 只中断指定旧教程UUID，不删除同player新网关的菜单/party/room身份。
   */
  [[nodiscard]] WorldCoordinatorResult InterruptTutorial(
      domain::PlayerId player, const domain::MatchId& session,
      std::string reason = "disconnect");
  [[nodiscard]] bool HasTutorial(domain::PlayerId player) const;
  [[nodiscard]] bool HasSingleWaiting(domain::PlayerId player) const;
  [[nodiscard]] std::optional<domain::MatchId> LocalSessionId(
      domain::PlayerId player) const;
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  AssetsCommitted(application::PlayerMenuSnapshot snapshot);
  [[nodiscard]] WorldCoordinatorResult SuppressRecruitment(
      domain::PlayerId player);
  /** @brief 将已提交装备快照同步到玩家当前所在的房间或广场。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  EquipmentChanged(application::PlayerMenuSnapshot snapshot,
                   bool refresh_menu_slot = false);

  /** @brief 将广场成员移交 DS，只保留大厅可见的外部成员归属标记。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  EnterDedicatedRoom(const WorldActorContext& actor);
  /** @brief DS 确认离场或失败后，使用最新已提交资料安全返回广场。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  LeaveDedicatedRoom(WorldActorContext actor);

  /** @brief 刷新已提交的家族徽章并广播原生 991，不改变准备或战斗状态。 */
  [[nodiscard]] WorldCoordinatorResult ClanMarkChanged(domain::PlayerId player,
                                                       std::uint32_t mark);
  /** @brief 以每实例 16 席的自动分流广场目录和统一房间计时创建组合协调器。 */
  explicit WorldCoordinator(RoomMatchTiming timing = RoomMatchTiming{});

  /** @brief 协调器拥有唯一的世界状态，因此禁止复制构造。 */
  WorldCoordinator(const WorldCoordinator&) = delete;

  /** @brief 协调器拥有唯一的世界状态，因此禁止复制赋值。 */
  WorldCoordinator& operator=(const WorldCoordinator&) = delete;

  /**
   * @brief 路由 C2S53/105/106/401～404/502/511 及房内命令。
   * @note ProtocolError 表示不可信负载畸形；阶段/权限错误返回
   * ServiceError；可展示的加房
   *       失败仍由房间协调器编码为客户端响应而不改变当前广场成员关系。
   */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> Handle(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 路由有效房间 timer，并把房间阶段转换为世界阶段。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> ApplyTimer(
      const RoomTimerToken& token, std::chrono::steady_clock::time_point now);

  /** @brief 将服务端权威比分/目标终局路由到房间，并保留完整结算命令所有权。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> FinishMatch(
      domain::RoomId room_id, application::MatchCompletionKind completion,
      std::chrono::steady_clock::time_point now);

  /**
   * @brief 断线时从当前广场或房间索引移除玩家；不向已断开的本人投递消息。
   * @param player_id 已断开且需要从世界索引移除的玩家。
   * @param phase Session 最后确认的世界阶段，用于选择广场或房间离场路径。
   * @param now world executor
   * 接收断线事件的单调时间，用于最后成员离场的可靠终局时长。
   */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> Disconnect(
      domain::PlayerId player_id, WorldSessionPhase phase,
      std::chrono::steady_clock::time_point now);

  /** @brief 在指定 Playing 房间安装经物品目录解析的本局战斗参与者快照。 */
  [[nodiscard]] application::ServiceResult<bool> InstallCombat(
      domain::RoomId room_id, const RoomMatchInstallation& installation);

  /** @brief 在开局已安装后发布初始目标对象和目标计时。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  InitialMatchEffects(domain::RoomId room_id,
                      std::chrono::steady_clock::time_point now);

  /** @brief 开局外部准备失败后恢复等待态；只允许尚未安装 match_id 的 Playing
   * 房间。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  AbortFailedMatchStart(domain::RoomId room_id, std::string_view notice);

  /** @brief 复制活动或已关闭房间仍等待 journal 回执的终局命令。 */
  [[nodiscard]] application::ServiceResult<application::MatchSettlementCommand>
  PendingSettlement(domain::RoomId room_id) const;

  /**
   * @brief 将 journal 成功回执投回世界 executor，按 room_id/match_id
   * 清除内存待办。
   * @note 只有耐久 stage 成功后调用；仅进入 dispatcher 队列不构成耐久回执。
   */
  [[nodiscard]] application::ServiceResult<bool> AcknowledgeSettlementStaged(
      domain::RoomId room_id, const domain::MatchId& match_id);

  /** @brief 将已提交奖励交给仍持有同一对局的房间，生成有序原生成绩刷新。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  ApplySettlementResult(const application::MatchSettlementResult& settlement);

  /** @brief 全体房间成员频道一致时返回其服务端频道，未知或混合返回零。 */
  [[nodiscard]] std::uint32_t RoomChannel(domain::RoomId room) const;
  /** @brief 返回广场只读状态，供装配前测试和指标读取。 */
  [[nodiscard]] const PlazaDirectory& Plaza() const noexcept;
  /** @brief 返回房间只读状态，供装配前测试和指标读取。 */
  [[nodiscard]] const RoomCoordinator& Rooms() const noexcept;
  [[nodiscard]] bool AuthorizeInstantRespawn(
      const application::UseItemCommand& command);

  /** @brief 在世界线程登记已完成登录的在线资料，供原生邀请按昵称定位。 */
  /** @brief 新认证网关菜单身份重建；不同于仅刷新资产的player_online。 */
  void GatewayPlayerOnline(application::PlayerMenuSnapshot snapshot);
  void PlayerOnline(application::PlayerMenuSnapshot snapshot);
  /** @brief 发起付费事务前验证在线角色和场景并冻结频道；菜单使用原生单人会话。
   */
  [[nodiscard]] application::ServiceResult<application::MegaphoneCommand>
  PrepareMegaphone(application::MegaphoneCommand command) const;
  /** @brief 按已扣除模板选择全服或同频道接收者，并应用双向屏蔽。 */
  [[nodiscard]] WorldCoordinatorResult MegaphoneDeliveries(
      const application::MegaphoneReceipt& receipt,
      const std::vector<domain::PlayerId>& blocked) const;

  /** @brief 已提交的图标偏好同步为完整 973，保留席位及准备状态。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  LevelIconChanged(application::PlayerMenuSnapshot snapshot);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  CoupleChanged(application::PlayerMenuSnapshot snapshot);

 private:
  /** @brief 世界线程保存的已登录角色资料及频道状态。 */
  struct OnlinePlayer final {
    application::PlayerMenuSnapshot snapshot;
    WorldSessionState state;
  };

  /** @brief 具有截止时间的本人邀请或拜访同意请求。 */
  struct PartyInvitation final {
    domain::PlayerId requester;
    domain::PlayerId target;
    bool party{false};
    bool visit{false};
    std::optional<domain::RoomId> room;
    std::chrono::steady_clock::time_point expires;
  };

  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> HandleImpl(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> HandleParty(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief C2S510按昵称踢人；仅等待房主可移除本房间、非同队伍成员。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleRoomKick(WorldActorContext actor, const protocol::Packet& packet,
                 std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::vector<domain::PlayerId> PartyMembers(
      domain::PlayerId player) const;
  void PublishParty(WorldCoordinatorResult& result, domain::PlayerId player);
  [[nodiscard]] protocol::ScenePartyState PartySceneState(
      domain::PlayerId player, bool allow_recruiting = true) const;
  void SyncPartyPlaza(WorldCoordinatorResult& result, domain::PlayerId player);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleRecruitment(WorldActorContext actor, const protocol::Packet& packet,
                    std::chrono::steady_clock::time_point now);
  void LeaveParty(WorldCoordinatorResult& result, domain::PlayerId player,
                  bool notify);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> VisitRoom(
      domain::PlayerId visitor, domain::RoomId room,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleChannel(WorldActorContext actor, const protocol::Packet& packet);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleReturnToMenu(WorldActorContext actor, const protocol::Packet& packet,
                     std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleEnterPlaza(WorldActorContext actor, const protocol::Packet& packet);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> HandleCreate(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> HandleJoin(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 按原生模式、人数选项和离房历史选房，并在同一世界命令内接纳全队。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleQuickJoin(WorldActorContext actor, const protocol::Packet& packet,
                  std::chrono::steady_clock::time_point now);
  /** @brief C2S108：从当前同频道广场成员构造房间邀请列表。 */
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandlePlazaUserList(const WorldActorContext& actor,
                      const protocol::Packet& packet) const;
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult>
  HandleLeaveRoom(WorldActorContext actor, const protocol::Packet& packet,
                  std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<WorldCoordinatorResult> HandleChat(
      WorldActorContext actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool ConsumeChatQuota(
      domain::PlayerId player_id, std::chrono::steady_clock::time_point now);

  PlazaDirectory plaza_;
  RoomCoordinator rooms_;
  LocalHordeTutorCoordinator
      tutorials_;  ///< 与房间/NPC竞技权限分离的原103本地教程。
  LocalSinglePlayCoordinator single_play_;
  std::map<domain::PlayerId, OnlinePlayer> online_players_;
  std::set<domain::PlayerId> dedicated_members_;
  std::map<domain::PlayerId, std::vector<domain::PlayerId>> parties_;
  std::vector<PartyInvitation> party_invitations_;
  std::set<domain::PlayerId> recruiting_parties_;
  // UserSlot +132 indexes the client's eight PartyMarkSmall textures, not a
  // player UID.
  std::map<std::uint16_t, std::map<domain::PlayerId, std::uint32_t>>
      party_badges_;
  std::map<domain::PlayerId, std::deque<std::chrono::steady_clock::time_point>>
      chat_times_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_COORDINATOR_H_
