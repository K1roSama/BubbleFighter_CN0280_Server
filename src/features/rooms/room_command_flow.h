#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COMMAND_FLOW_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COMMAND_FLOW_H_

/**
 * @file room_command_flow.h
 * @brief 定义单房间 executor 内的命令路由、广播编排和计时结果映射。
 */

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "application/error.h"
#include "features/battle/captain_assignments.h"
#include "features/battle/combat_relay.h"
#include "features/battle/crazy_racing_match.h"
#include "features/battle/extreme_racing_match.h"
#include "features/battle/ice_service.h"
#include "features/battle/infection_match.h"
#include "features/battle/match_objectives.h"
#include "features/battle/match_start.h"
#include "features/battle/match_terminal.h"
#include "features/battle/pirate_map_agents.h"
#include "features/battle/pirate_match.h"
#include "features/battle/weapon_master_match.h"
#include "features/battle/zombie_pve_match.h"
#include "features/chat/user_message.h"
#include "features/inventory/item_use_service.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/practice/training_room_match.h"
#include "features/rooms/room_runtime.h"
#include "protocol/battle/match_transition_messages.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/rooms/user_host_messages.h"
#include "protocol/scenes/scene_messages.h"
#include "protocol/scenes/scene_session_messages.h"
#include "sessions/hide_seek_session.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/** @brief 一条已经确定目标玩家的房间出站消息。 */
struct RoomDelivery final {
  domain::PlayerId recipient;  ///< 必须仍属于当前房间的目标玩家。
  OutboundMessage message;     ///< 交给该玩家 Session 顺序写队列的消息。

  /** @brief 按接收玩家和完整出站消息比较。 */
  bool operator==(const RoomDelivery&) const = default;
};

/** @brief 一次房间命令产生的状态事件、定向消息和新计时集合。 */
struct RoomCommandResult final {
  RoomEvent event{RoomEvent::kNoChange};  ///< 纯状态机产生的稳定事件。
  std::vector<RoomDelivery> deliveries;   ///< 按每个目标的协议发送顺序排列。
  std::vector<RoomTimerToken> active_timers;  ///< executor 重新核对的计时令牌。
  std::optional<CombatRejectReason>
      combat_reject_reason;  ///< 安全忽略实时消息的审计原因。
  std::optional<std::string>
      rule_rejection_detail;  ///< 仅供服务端审计，不发送玩家。
  std::optional<application::MatchSettlementCommand>
      settlement_command;  ///< 进入结果态时先交给耐久
                           ///< journal、再交给结算队列的拥有型命令。
};

/**
 * @brief 安装到 Playing 房间的正式对局身份或地图浏览参与者。
 * @note 正式对局必须携带已持久化 match_id；仅原生 submode 4
 * 的地图浏览使用空身份， 不创建正式开局、结算账本或奖励。本层不生成
 * ID，也不伪造数据库成功。
 */
struct RoomMatchInstallation final {
  std::optional<domain::MatchId>
      match_id;  ///< 正式对局已持久化 UUID；地图浏览为空。
  domain::MatchScoreModel
      score_model;  ///< 只允许明确支持的普通团队/个人击爆规则。
  std::chrono::steady_clock::time_point
      started_at;  ///< 房间 executor 观察到的正式开局时刻。
  std::vector<CombatParticipant> participants;  ///< 冻结对象、队伍和原生配装。
};

/**
 * @brief 单个活动房间的同步命令路由核心。
 * @note 本对象不加锁，必须与对应 RoomRuntime 一起由同一个房间 executor
 * 串行调用。玩家资料
 *       以值快照缓存，路由过程中不访问数据库；加入、换装等外部用例完成后，应先在同一
 *       executor 调用 update_player_snapshot，再允许依赖新资料的广播。
 * @note 当前处理等待房间 C2S505/509/511/515/517/518、开局
 * C2S503/504/513/514，以及加载 阶段 C2S701/702/709 和战斗 RTT 探测
 * C2S32840。建房、加入、离开和 Session 查找由 更外层协调器负责。
 */
class RoomCommandFlow final {
 public:
  /** @brief 奖金只能在503/504消费授权后冻结，查询和设置不扣款。 */
  [[nodiscard]] std::optional<application::UserHostPrizeTerms> UserHostPrize()
      const;
  /** @brief 仅接受已提交资产快照，推送105/802不触发换装或准备。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> AssetsCommitted(
      application::PlayerMenuSnapshot snapshot);

  [[nodiscard]] std::optional<application::UseItemCommand>
  PrepareInstantRespawn(domain::PlayerId player, std::uint32_t object) const;
  [[nodiscard]] bool AuthorizeInstantRespawn(
      const application::UseItemCommand& command);
  /**
   * @brief 校验 SceneInfo、运行时成员和全部资料快照后创建路由器。
   * @return 契约不一致返回 kInternal；客户端输入不在此工厂处理。
   */
  [[nodiscard]] static application::ServiceResult<
      std::unique_ptr<RoomCommandFlow>>
  Create(std::shared_ptr<RoomRuntime> room, protocol::SceneInfo scene,
         std::vector<application::PlayerMenuSnapshot> players,
         RoomMatchTiming timing = RoomMatchTiming{});

  /**
   * @brief 解析并处理一条已解密房间 frame，返回定向广播和当前有效计时令牌。
   * @note ProtocolError 表示不可信负载畸形，Session
   * 应按协议违规关闭；业务权限和阶段错误 通过 ServiceError
   * 返回，不产生部分广播。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> Handle(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now,
      std::string_view peer_address = {},
      const std::optional<application::IceLease>& ice_lease = std::nullopt);

  /**
   * @brief
   * 用权威房间席位、队伍和昵称生成附近/队伍聊天，过滤双向屏蔽目标后回显发送者。
   * @note 所有房间阶段均可聊天；team_only 只投递同队成员。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleChat(
      domain::PlayerId actor, bool team_only,
      const domain::UserMessage& message,
      std::span<const domain::PlayerId> blocked_peers) const;

  /**
   * @brief 将 executor 到期回调映射到房间状态和客户端消息。
   * @note 旧 generation/不匹配令牌只返回 kIgnoredStaleTimer，不发送任何消息。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> ApplyTimer(
      const RoomTimerToken& token, std::chrono::steady_clock::time_point now);

  /**
   * @brief 由服务端确认比分或模式目标达成后立即结束 Playing
   * 对局并生成可靠结算命令。
   * @note 本入口不接受客户端直接调用。它先冻结统计并完整构造命令，成功后才推进
   * Results， 因而校验失败不会留下“已结束但没有结算命令”的房间状态。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> FinishMatch(
      application::MatchCompletionKind completion,
      std::chrono::steady_clock::time_point now);

  /**
   * @brief 最后一名成员即将离开 Playing
   * 房间时，无副作用地构造全员放弃结算命令。
   * @note
   * 仅允许当前房间恰有一名成员；命令把最后成员也标记离场。调用方必须先成功取得命令，
   *       再销毁房间，避免断线清理丢失已经持久化的对局事实。
   */
  [[nodiscard]] application::ServiceResult<application::MatchSettlementCommand>
  PrepareAbandonedSettlement(std::chrono::steady_clock::time_point now) const;

  /**
   * @brief 复制尚未获得耐久 journal 回执的终局命令，供数据库失败后重新提交。
   * @note 只有 Results 阶段可能存在；确认保存前结果返回 timer 不得把房间恢复为
   * Waiting。
   */
  [[nodiscard]] application::ServiceResult<application::MatchSettlementCommand>
  PendingSettlement() const;

  /**
   * @brief 确认指定终局已由 MatchSettlementJournal
   * 耐久保存，并允许结果态最终回房。
   * @note dispatcher 过载但 journal 已保存也可确认；match_id
   * 不匹配时拒绝清除，避免迟到回执 误确认下一局。重复确认返回 kInvalidState。
   */
  [[nodiscard]] application::ServiceResult<bool> AcknowledgeSettlementStaged(
      const domain::MatchId& match_id);

  /**
   * @brief 在进入 Playing 后冻结本局战斗对象与原生武器栏，并启用实时消息路由。
   * @note 调用方必须从已验证对象注册和物品目录构造 participants；本层不会把
   * item_id 猜成 客户端原生模板。参与者必须与当前房间成员及其 C2S709 对象 ID
   * 完全一致。
   * @return 首次安装返回 true；同一局重复安装返回
   * kInvalidState，不会替换活动账本。
   */
  [[nodiscard]] application::ServiceResult<bool> InstallCombat(
      const RoomMatchInstallation& installation);

  /**
   * @brief 开局持久化、配装准备或战斗账本安装失败时，把 Playing 房间恢复为
   * Waiting。
   * @note 只供 world executor
   * 调用；若数据库开局已经提交，调用前必须先把该记录可靠补偿为
   *       interrupted。已经安装 active match 后绝不允许回滚开局。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  AbortFailedMatchStart(std::string_view notice);

  /**
   * @brief 在同一 executor 上替换某成员的权威资料快照。
   * @return 玩家不在房间或快照 ID
   * 不一致时返回错误；成功返回是否覆盖了既有快照。
   */
  [[nodiscard]] application::ServiceResult<bool> UpdatePlayerSnapshot(
      application::PlayerMenuSnapshot snapshot);

  /**
   * @brief 在 registry
   * 已完成加入后，原子纳入新成员资料并生成本人快照和同房增量广播。
   * @note
   * 只能在等待阶段调用；任一协议编码失败都会回滚内部资料缓存，调用方随后必须回滚
   *       registry 加入，不能把半完成成员暴露给 Session。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> AddMemberSnapshot(
      application::PlayerMenuSnapshot snapshot);

  /**
   * @brief 在 registry
   * 已完成离开后移除资料快照，并为存活成员生成清席、迁主和中断回房消息。
   * @param departed 已由 registry 移除的玩家 ID，必须与 departure.player_id
   * 一致。
   * @param departure
   * 必须属于本房间且尚有存活成员；最后一人离开由协调器直接销毁本对象。
   * @param interrupted_notice 加载/倒计时被中断时发送给存活成员的安全提示。
   */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  RemoveMemberSnapshot(domain::PlayerId departed,
                       const RoomDeparture& departure,
                       std::string_view interrupted_notice,
                       std::chrono::steady_clock::time_point now =
                           std::chrono::steady_clock::now());

  /** @brief 是否仍有尚未产生终局的开局账本，包括爆破回合加载。 */
  [[nodiscard]] bool HasUnsettledMatch() const noexcept;

  /** @brief 当前模式及玩家生命周期是否允许死亡期间选择预设。 */
  [[nodiscard]] bool CanChangeCombatPreset(
      domain::PlayerId player) const noexcept;

  /** @brief 已提交换装刷新资料、取消准备并推送本人和同行的原生装备。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> EquipmentChanged(
      application::PlayerMenuSnapshot snapshot);

  /** @brief 刷新已提交的家族徽章并广播原生 991，不改变准备或战斗状态。 */
  [[nodiscard]] RoomCommandResult ClanMarkChanged(domain::PlayerId player,
                                                  std::uint32_t mark);

  /** @brief 为已经在房间中的指定玩家生成拥有负载的 S2C112 初始快照消息。 */
  [[nodiscard]] application::ServiceResult<OutboundMessage>
  MakeSceneSessionMessage(domain::PlayerId selected) const;

  /** @brief 返回当前完整场景值；引用只能在所属 executor 当前调用栈中使用。 */
  [[nodiscard]] const protocol::SceneInfo& Scene() const noexcept;

  /** @brief
   * 查询本局17/21本机创建顺序缺口；未安装或未知玩家返回true，不授予对象身份。
   */
  [[nodiscard]] bool NativeLoopbackOrderUncertain(
      domain::PlayerId player) const noexcept;

  /** @brief 查询指定接收者已观察的构造次数；零不证明初始对象编号完整。 */
  [[nodiscard]] std::uint64_t ObservedNativeConstructors(
      domain::PlayerId receiver, std::uint32_t owner_object) const noexcept;

  /** @brief 复制当前成员资料，包含已提交换装和结算更新，供服务端强制离房使用。
   */
  [[nodiscard]] std::optional<application::PlayerMenuSnapshot> PlayerSnapshot(
      domain::PlayerId player) const;

  /** @brief 接收同局已提交奖励，更新成绩和回房资料；错局或重复回执无副作用。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  ApplySettlementResult(const application::MatchSettlementResult& settlement,
                        std::chrono::steady_clock::time_point now);

  /** @brief 开局持久化完成后发布目标对象和初始目标成绩，不能早于 S2C704。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  InitialMatchEffects(std::chrono::steady_clock::time_point now);

  /** @brief 已提交的图标偏好同步为完整 973，保留席位及准备状态。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> LevelIconChanged(
      application::PlayerMenuSnapshot snapshot);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> CoupleChanged(
      application::PlayerMenuSnapshot snapshot);

 private:
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleHideSeek(
      domain::PlayerId player, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> AdvanceHideSeek(
      std::chrono::steady_clock::time_point now);
  void AppendHideSeek(RoomCommandResult& result,
                      std::vector<HideSeekDelivery> messages) const;
  void SynchronizeHideSeekTeams(RoomCommandResult& result);
  std::unique_ptr<HideSeekSession> hide_seek_;
  std::map<domain::PlayerId, std::uint32_t> hide_seek_capabilities_;
  std::map<domain::PlayerId, std::uint8_t> hide_seek_observer_loading_;
  /** @brief 内部路由结果由公开handle统一追加已协商网络状态。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleImpl(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 开赛授权绑定房主、冻结名单和有限时间窗口，不能跨局复用。 */
  struct UserHostAuthorization final {
    application::UserHostPrizeTerms terms;
    std::vector<RoomMemberSnapshot> roster;
    std::chrono::steady_clock::time_point eligible_at;
    std::chrono::steady_clock::time_point expires_at;
  };

  std::uint32_t user_host_amount_{0U};
  std::optional<UserHostAuthorization> user_host_authorization_;
  std::optional<application::UserHostPrizeTerms> frozen_user_host_prize_;
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleUserHost(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] protocol::UserHostStartStatus UserHostStatus(
      domain::PlayerId actor) const;
  [[nodiscard]] bool UserHostRosterMatches() const;
  [[nodiscard]] RoomCommandResult UserHostReply(
      domain::PlayerId actor, protocol::UserHostStartStatus status) const;
  void CancelUserHostAuthorization(RoomCommandResult& result);
  void ResetUserHostPrize() noexcept;

  /** @brief 绑定原生区域动作、生命代次和传送确认的短期状态。 */
  struct MapRegionPending final {
    std::uint64_t life{0U};
    std::uint16_t region{0U};
    std::uint32_t kind{0U};
    std::array<float, 3U> destination{};
    std::chrono::steady_clock::time_point accepted_at{};
    bool completed{false};
  };

  std::map<domain::PlayerId, MapRegionPending> map_region_pending_;
  /** @brief 950 区域申请及 32922 传送确认；目标仅取自本图原始网格。 */
  [[nodiscard]] RoomCommandResult HandleMapRegion(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 校验本人席位，仅在加载、倒计时和战斗期间无状态回显 RTT 探测。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleMatchLatencyProbe(domain::PlayerId actor,
                          std::span<const std::uint8_t> body) const;

  /** @brief 保存一次有效延迟探测的开始时刻和仍可提交报告的房间成员。 */
  struct LatencyProbe final {
    std::chrono::steady_clock::time_point
        started_at;                              ///< 接受 C2S517 的单调时间。
    std::set<domain::PlayerId> pending_players;  ///< 尚未提交一次报告的成员。
  };

  /** @brief 本局持久身份、计分模型、开始时刻和包含离场者的开局名单。 */
  struct ActiveMatch final {
    ~ActiveMatch() noexcept = default;

    domain::MatchId match_id;             ///< 开局前持久化的稳定 UUID。
    domain::MatchScoreModel score_model;  ///< 终局采用的明确普通计分模型。
    std::uint32_t score_target{
        0U};  ///< 开局冻结的击爆目标；0 或超界表示仅限时。
    std::chrono::steady_clock::time_point
        started_at;  ///< 计算有界有效时长的单调时钟起点。
    std::map<domain::PlayerId, domain::MatchResultParticipantInput>
        participants;  ///< 按玩家稳定排序且不因离场删除。
    std::map<domain::PlayerId, protocol::NativeScorePlayer>
        score_players;  ///< 开局冻结的原生成绩身份，离场后仍可编码完整名单。
    bool rewards_applied{false};  ///< 同局已提交奖励回执至多应用一次。
    bool terminal_staged{
        false};  ///< 终局已冻结保存，奖励回执到达前也不得重新构造终局。
    std::uint8_t winner{0U};  ///< 终局权威胜方，供迟到奖励刷新使用。
  };

  RoomCommandFlow(
      std::shared_ptr<RoomRuntime> room, protocol::SceneInfo scene,
      std::map<domain::PlayerId, application::PlayerMenuSnapshot> players,
      RoomMatchTiming timing);

  /** @brief 结束原生地图浏览并回等待房间；不产生正式比赛结算或奖励。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> FinishMapBrowse();
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleStatus(
      domain::PlayerId actor, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleTransferHost(domain::PlayerId actor,
                     std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleSwitchTeam(
      domain::PlayerId actor, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleSeatLock(
      domain::PlayerId actor, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleConfiguration(domain::PlayerId actor,
                      std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleRotatingMap(
      domain::PlayerId actor, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleSceneMovement(domain::PlayerId actor,
                      std::span<const std::uint8_t> body) const;
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleStart(
      domain::PlayerId actor, std::span<const std::uint8_t> body,
      std::chrono::steady_clock::time_point now, bool rotating);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleLoading(
      domain::PlayerId actor, std::uint16_t opcode,
      std::span<const std::uint8_t> body,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleLatencyProbe(domain::PlayerId actor, std::span<const std::uint8_t> body,
                     std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleLatencyReport(domain::PlayerId actor,
                      std::span<const std::uint8_t> body,
                      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleCombat(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 本服网络设置查询/保存；偏好和实际回退状态分开下发。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleNetworkSetting(domain::PlayerId actor,
                       std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleItemSetting(
      domain::PlayerId actor, std::span<const std::uint8_t> body,
      std::uint16_t response_opcode);
  void AppendItemState(RoomCommandResult& result) const;
  [[nodiscard]] std::string ItemRejection(
      const application::PlayerMenuSnapshot& snapshot) const;
  /** @brief 只向已协商网络设置扩展的成员发布状态，避免旧客户端收到未知消息。 */
  void AppendNetworkState(RoomCommandResult& result) const;
  void AppendNetworkPeers(RoomCommandResult& result);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleNetworkRegister(domain::PlayerId actor,
                        std::span<const std::uint8_t> body,
                        std::string_view peer_address,
                        const std::optional<application::IceLease>& ice_lease);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleIceSignal(
      domain::PlayerId actor, std::span<const std::uint8_t> body,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  HandleNetworkMovement(domain::PlayerId actor,
                        std::span<const std::uint8_t> body,
                        std::chrono::steady_clock::time_point now);
  /** @brief Flush the latest movement snapshots once per room combat tick. */
  void AppendMovementTick(RoomCommandResult& result);
  /** @brief 有界保留开局 NPC 消息的原序列；正式身份安装后仍走通常校验路径。 */
  [[nodiscard]] RoomCommandResult StageNpcPrestart(
      domain::PlayerId actor, const protocol::Packet& packet);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> HandleObjective(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<RoomCommandResult> AdvanceObjectives(
      std::chrono::steady_clock::time_point now);

  [[nodiscard]] application::ServiceResult<RoomCommandResult>
  CompleteLoadingEvent(RoomEvent event,
                       std::chrono::steady_clock::time_point now);
  [[nodiscard]] RoomCommandResult MakeResult(RoomEvent event) const;
  [[nodiscard]] application::ServiceResult<application::MatchSettlementCommand>
  BuildSettlementCommand(application::MatchCompletionKind completion,
                         std::chrono::steady_clock::time_point now) const;
  bool PreserveCombatPerformance() noexcept;
  /** @brief 广播竞速核心输出；五赛段完成时结算，否则重新计算房间截止点。 */
  [[nodiscard]] application::ServiceResult<RoomCommandResult> ApplyRacingUpdate(
      RoomCommandResult result, ExtremeRacingUpdate update,
      std::chrono::steady_clock::time_point now);
  void AppendScore(RoomCommandResult& result, std::uint8_t winner) const;
  void AppendMatchResult(RoomCommandResult& result,
                         const application::MatchSettlementCommand& command);
  /** @brief 判断特殊击爆模式是否达到目标；普通团队/个人对战仅由时间结束。 */
  [[nodiscard]] bool OrdinaryScoreTargetReached() const noexcept;
  void Broadcast(RoomCommandResult& result,
                 std::span<const OutboundMessage> messages) const;
  [[nodiscard]] OutboundMessage MemberUpdate(domain::PlayerId player_id) const;
  [[nodiscard]] protocol::SceneSessionSnapshot RoomSnapshot(
      domain::PlayerId selected) const;
  void AppendReturnToWaiting(RoomCommandResult& result,
                             std::string_view notice);

  std::shared_ptr<RoomRuntime> room_;
  protocol::SceneInfo scene_;
  std::map<domain::PlayerId, application::PlayerMenuSnapshot> players_;
  RoomMatchTiming timing_;
  std::chrono::milliseconds
      default_match_duration_;  ///< 无房间时长时使用最初服务配置。
  std::optional<std::uint32_t>
      host_duration_override_;  ///< 本模式内经 513 明确调整的时间。
  std::map<std::uint32_t, LatencyProbe> latency_probes_;
  std::map<domain::PlayerId, protocol::MapAssetsRequest> map_assets_;
  std::optional<protocol::StartMatchRequest> normal_match_;
  std::optional<protocol::StartRotatingMatchRequest> rotating_match_;
  std::unique_ptr<TrainingRoomMatch>
      training_;  ///< 无持久化比赛身份的私人训练木桩。
  std::unique_ptr<CombatRelay>
      combat_;  ///< 仅本局 Playing 阶段有效的唯一战斗账本。
  std::optional<std::uint32_t>
      pending_soccer_ball_;  ///< 加载阶段先保存房主足球身份。
  std::unique_ptr<MatchObjectives>
      objectives_;                       ///< 目标模式独立账本，爆破跨回合保留。
  std::unique_ptr<PirateMatch> pirate_;  ///< 海盗房主脚本与对象握手。
  std::unique_ptr<InfectionMatch>
      infection_;  ///< 僵尸母体、动态阵营与特殊复活授权。
  std::unique_ptr<WeaponMasterMatch>
      weapon_master_;  ///< 武器大师本局临时配装与经验账本。
  std::unique_ptr<ExtremeRacingMatch>
      racing_;  ///< 极限竞速五赛段、检查点和金币账本。
  std::unique_ptr<CrazyRacingMatch>
      crazy_racing_;  ///< 原BFRunner终点和检查点，独立于击爆胜负。
  std::unique_ptr<CaptainAssignments>
      captain_;  ///< 队长模式选任、变身与临时武器。

  /** @brief 队长时间计量：FeverSpecCaptain
   * timeIncrement=50/100ms，按选任代次独立计时。 */
  struct CaptainFeverClock final {
    std::uint64_t assignment{0U};
    std::uint64_t life{0U};  ///< 泡封/获救换代后不把旧生命空闲时间补给新生命。
    std::chrono::steady_clock::time_point last{};
    std::uint64_t serial{0U};
  };

  std::map<domain::PlayerId, CaptainFeverClock> captain_fever_clock_;
  std::unique_ptr<ZombiePveMatch>
      zombie_pve_;              ///< Boss 地图生成记录和原生脚本终局。
  PirateMapAgents map_agents_;  ///< 公主等地图角色，不参与波次怪物计数。
  std::vector<OutboundMessage>
      loading_map_agents_;  ///< 最多 64 个生成包，等待全员加载完成。
  /** @brief Boss/海盗开局序列；每项保留发送者，防止房主更换后继承旧权限。 */
  std::vector<std::pair<domain::PlayerId, protocol::Packet>> npc_prestart_;
  std::size_t npc_prestart_bytes_{0U};  ///< 本服内存上限计数，不是原协议字段。
  /** @brief
   * 当前加载场景未进入CombatRelay的本机构造缺口；每个真人只保留首次opcode。 */
  std::map<domain::PlayerId, std::uint16_t> native_prestart_gaps_;
  std::set<domain::PlayerId>
      network_setting_clients_;  ///< 已主动查询扩展的当前房间成员。
  std::map<domain::PlayerId, std::uint32_t>
      room_setting_versions_;  ///< 独立于 ICE 协商版本。
  std::map<domain::PlayerId, std::uint16_t> item_setting_clients_;

  struct NetworkEndpoint {
    std::uint32_t public_ip{}, private_ip{}, port{};
    std::uint64_t serial{};
    bool ice{false};
    std::optional<application::IceLease> lease;
    std::chrono::steady_clock::time_point signal_window{};
    unsigned signals{0};
  };

  std::map<domain::PlayerId, NetworkEndpoint> network_endpoints_;

  struct PendingNetworkMovement {
    std::uint64_t serial{0U};
    std::uint16_t opcode{0U};
    std::vector<std::uint8_t> payload;
  };

  std::map<domain::PlayerId, PendingNetworkMovement> pending_network_movements_;
  std::map<std::pair<domain::PlayerId, domain::PlayerId>,
           std::array<std::uint8_t, 32>>
      network_keys_;
  std::uint64_t network_epoch_{0}, network_generation_{0};
  std::optional<std::chrono::steady_clock::time_point> ice_restarted_at_;
  std::optional<protocol::Packet>
      pending_pirate_wave_start_;  ///< 持久开局前保留首波请求。
  std::chrono::steady_clock::time_point
      round_deadline_{};  ///< 当前回合普通截止点。
  std::optional<ActiveMatch>
      active_match_;  ///< 从已持久化开局延续到终局命令移交的事实。
  std::optional<application::MatchSettlementCommand>
      pending_settlement_;  ///< journal 确认前由房间 executor
                            ///< 保留的终局值副本。
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_COMMAND_FLOW_H_
