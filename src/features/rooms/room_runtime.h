#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_RUNTIME_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_RUNTIME_H_

/**
 * @file room_runtime.h
 * @brief 定义单房间串行状态机、稳定座位、代次计时令牌和房间注册表。
 */

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/hide_seek_match.h"
#include "features/battle/weapon_master_match.h"
#include "features/practice/training_room.h"
#include "protocol/rooms/room_network_messages.h"

namespace bf::runtime {

/** @brief 房间从等待到加载、对局、结果和关闭的可观察阶段。 */
enum class RoomPhase : std::uint8_t {
  kWaiting,
  kLoading,
  kCountdown,
  kPlaying,
  kResults,
  kClosed,
};

/** @brief 房间命令成功后产生的稳定状态事件。 */
enum class RoomEvent : std::uint8_t {
  kNoChange,
  kMemberJoined,
  kReadyChanged,
  kTeamChanged,
  kSeatLockChanged,
  kConfigurationChanged,
  kLoadingStarted,
  kLoadProgress,
  kCountdownStarted,
  kMatchStarted,
  kResultsStarted,
  kResultsDisplayDue,
  kReturnedToWaiting,
  kRoomClosed,
  kIgnoredStaleTimer,
  kMemberLeft,
  kModeObjectiveDue,
  kRoundLoadingStarted,
  kRoundStarted,
  kHostChanged,
  kCombatTickDue,
};

/** @brief 由房间 executor 调度、完成时必须带回原代次的计时种类。 */
enum class RoomTimerKind : std::uint8_t {
  kLoadingTimeout,
  kCountdown,
  kMatchTimeout,
  kResultsDisplay,
  kResultsReturn,
  kModeObjective,
  kCombatTick,
};

/**
 * @brief 一次房间计时回调的不可伪造上下文值。
 * @note executor 安排 timer 时复制本值；回调只提交本值，不捕获裸 Room
 * 指针。房间会同时 校验 room_id、generation、kind 和
 * deadline，旧回调只能得到忽略事件。
 */
struct RoomTimerToken final {
  domain::RoomId room_id;        ///< 计时所属房间。
  std::uint64_t generation{0U};  ///< 安排计时时的房间代次。
  RoomTimerKind kind{RoomTimerKind::kLoadingTimeout};  ///< 计时语义。
  std::chrono::steady_clock::time_point deadline;  ///< steady_clock 截止点。

  /** @brief 按房间、种类和代次比较。 */
  bool operator==(const RoomTimerToken&) const = default;
};

/** @brief 显式选择的本服赏金策略一；默认不开未知原服经济规则。 */
struct UserHostPrizeRules final {
  bool enabled{false};
  std::uint32_t minimum_amount{1000U};
  std::uint32_t maximum_amount{100000U};
  bool operator==(const UserHostPrizeRules&) const = default;
};

/** @brief 一局加载、倒计时、战斗和结果展示所需的单调时钟时长。 */
struct RoomMatchTiming final {
  std::chrono::milliseconds loading_timeout{
      std::chrono::seconds(90)};  ///< 加载上限。
  std::chrono::milliseconds countdown{
      std::chrono::seconds(4)};  ///< 原生 3/2/1/START 各一秒。
  std::chrono::milliseconds match_duration{
      std::chrono::minutes(4)};  ///< 房间无时长时的回退。
  std::chrono::milliseconds result_transition{
      std::chrono::seconds(2)};  ///< 胜负展示后发送 733。
  std::chrono::milliseconds result_duration{
      std::chrono::seconds(20)};  ///< 含原生过场的结果总时长。
  std::chrono::milliseconds movement_tick{
      std::chrono::milliseconds(50)};  ///< 房间移动快照刷新周期。
  bool use_room_duration{true};  ///< 采用原生房间时长；隔离定时测试可显式关闭。
  WeaponMasterRules weapon_master;     ///< 开局冻结本服武器大师数值和主武器池。
  UserHostPrizeRules user_host_prize;  ///< 本服策略一及配置范围，不是原服常量。
  std::shared_ptr<const HideSeekScene>
      hide_seek_scene;  ///< 仅注入已核验地图；空值禁止躲猫猫开局。

  /** @brief 按全部房间阶段时长比较。 */
  bool operator==(const RoomMatchTiming&) const = default;
};

/** @brief 创建房间时由协议/配置边界校验后交给运行时的参数。 */
struct RoomCreateSpec final {
  std::uint16_t capacity{12U};  ///< 玩家座位数；当前兼容范围为 2～16 的偶数。
  std::uint32_t mode_id{1U};    ///< 当前模式标识，仅用于共同作战队伍规则。
  std::string password;         ///< 房间口令；不进入快照、日志或广播。
  bool user_hosted{false};      ///< 原生赏金房间；主办者在固定 12 号观战席。
  bool hide_seek_v1{false};     ///< 独立业务变体；原生场景仍使用模式1。
};

/** @brief 房间成员的只读值快照，不暴露运行时内部引用。 */
struct RoomMemberSnapshot final {
  domain::PlayerId player_id;   ///< 已认证玩家。
  std::uint16_t seat{0U};       ///< 房间生命周期内稳定的零基座位。
  std::uint8_t team{1U};        ///< 普通房间为 1/2；共同作战等待期为 1。
  bool ready{false};            ///< 非房主的准备状态。
  bool assets_received{false};  ///< 是否已提交地图资产消息。
  bool load_completed{false};   ///< 是否已提交客户端加载完成消息。
  std::optional<std::uint32_t> object_id;  ///< 由座位推导并验证的战斗对象 ID。

  /** @brief 原生队伍 36 是观战者，参与加载与广播但不参与比赛计分。 */
  [[nodiscard]] bool IsObserver() const noexcept { return team == 36U; }

  /** @brief 按成员全部权威状态字段比较。 */
  bool operator==(const RoomMemberSnapshot&) const = default;
};

/** @brief 玩家离开后供 Session 层生成清座位、迁主或返回等待房间消息的结果。 */
struct RoomDeparture final {
  std::shared_ptr<class RoomRuntime>
      room;  ///< 离开动作作用的房间；删除后仍可安全检查关闭态。
  std::uint16_t departed_seat{0U};  ///< 需要向其余成员清除的座位。
  std::optional<domain::PlayerId>
      new_host;                   ///< 房主离开后按最低座位选出的新房主。
  bool start_interrupted{false};  ///< 加载/倒计时因成员离开而回到等待态。
  bool room_removed{false};       ///< 最后一名成员离开，registry 已删除房间。
};

/**
 * @brief 单个房间的无锁串行运行时。
 * @note 所有方法必须由该房间的同一 executor 顺序调用。Session 只保存 room_id
 * 和代次，不能 跨线程直接修改成员；网络和数据库 completion 必须投回 executor
 * 后再调用本类。
 */
class RoomRuntime final {
 public:
  [[nodiscard]] bool IsHideSeek() const noexcept { return hide_seek_v1_; }

  [[nodiscard]] bool IsTraining() const noexcept {
    return mode_id_ == kTrainingMode;
  }

  /** @brief 显式区分移交发起者与接任者，避免两个玩家标识误传。 */
  struct HostTransfer final {
    domain::PlayerId actor;   ///< 当前已认证房主。
    domain::PlayerId target;  ///< 接任的房内玩家。
  };

  RoomRuntime(const RoomRuntime&) = delete;
  RoomRuntime& operator=(const RoomRuntime&) = delete;

  /** @brief 修改非房主准备状态；房主始终保持未准备。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> SetReady(
      domain::PlayerId player_id, bool ready);

  /** @brief 在普通等待房间切换 1/2 队伍，并按半数容量拒绝已满目标队伍。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> SwitchTeam(
      domain::PlayerId player_id);
  /** @brief
   * 仅权威躲猫猫状态机可在Playing阶段把猫转入蓝方；不受普通半数队伍容量限制。
   */
  [[nodiscard]] application::ServiceResult<RoomEvent> ConvertHideSeekMember(
      domain::PlayerId player_id);

  /** @brief
   * 等待阶段由当前房主移交给房内成员；保持席位、队伍及其他成员准备状态。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> TransferHost(
      HostTransfer request);

  /** @brief 由房主锁定或解锁空座位；已占用座位不可锁。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> SetSeatLocked(
      domain::PlayerId actor, std::uint16_t seat, bool locked);

  /**
   * @brief 房主提交配置后同步模式，仅在玩法规则改变时清空准备状态。
   * @note 完整 SceneConfiguration
   * 由上层房间记录保存；本方法只同步运行时必须执行的约束。
   */
  [[nodiscard]] application::ServiceResult<RoomEvent> ApplyConfigurationMode(
      domain::PlayerId actor, std::uint32_t mode_id, bool rules_changed = true);

  /** @brief 房主仅切换轮换地图，保留准备状态、队伍和当前模式。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> ApplyRotatingMapChange(
      domain::PlayerId actor);

  /**
   * @brief 由房主开始加载；默认要求全部非房主成员已准备。
   * @param actor 发起操作的玩家，必须是当前房主。
   * @param force 与已验证旧服务兼容的强制开始标志；不改变成员准备事实。
   * @param now 由房间 executor 取得的单调时钟当前值，用于建立加载期限。
   * @param timing 本局加载、倒计时、对局和结果阶段的已校验时限。
   * @param random_teams 随机组按实际玩家洗牌并均分；共同作战阵营规则优先。
   */
  [[nodiscard]] application::ServiceResult<RoomEvent> StartLoading(
      domain::PlayerId actor, bool force,
      std::chrono::steady_clock::time_point now, RoomMatchTiming timing,
      bool random_teams = false);

  /** @brief 记录地图资产接收；最后一名完成两类加载信号时推进倒计时或开局。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> RecordAssetsReceived(
      domain::PlayerId player_id, std::chrono::steady_clock::time_point now);

  /** @brief 记录客户端加载完成；最后一名完成两类加载信号时推进倒计时或开局。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> RecordLoadCompleted(
      domain::PlayerId player_id, std::chrono::steady_clock::time_point now);

  /** @brief 校验客户端对象 ID 必须严格等于 `1000000 + seat * 100000`。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> RecordObjectId(
      domain::PlayerId player_id, std::uint32_t object_id);

  /** @brief 目标或管理员判定对局结束后进入结果态；重复结束不会生成第二套计时。
   */
  [[nodiscard]] application::ServiceResult<RoomEvent> FinishMatch(
      std::chrono::steady_clock::time_point now);

  /** @brief 结果态暂停面板和回房计时，直到奖励事务已提交；非结果态抛逻辑错误。
   */
  void AwaitCommittedRewards();

  /** @brief 从奖励确认时刻安排完整结果展示窗口；非结果态抛逻辑错误。 */
  void BeginCommittedResults(std::chrono::steady_clock::time_point now);

  /** @brief 更新目标定时器截止点；仅由所属房间目标状态机调用。 */
  void SetObjectiveDeadline(
      std::optional<std::chrono::steady_clock::time_point> deadline);
  /** @brief 已安装炸弹或回合间隔接管普通时限，避免普通超时提前整局结算。 */
  void SuspendMatchTimeout() noexcept;
  /** @brief 保留同局身份进入下一爆破回合加载；仅 Playing 可用。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> RestartRound(
      std::chrono::steady_clock::time_point now);

  /**
   * @brief 应用一个到期 timer；旧代次、已取消或错误种类的回调安全返回忽略事件。
   * @note now 早于令牌 deadline 时不推进状态，返回 kNoChange。
   */
  [[nodiscard]] application::ServiceResult<RoomEvent> ApplyTimer(
      const RoomTimerToken& token, std::chrono::steady_clock::time_point now);

  /** @brief 主动中断当前加载/对局/结果并恢复等待态；关闭房间拒绝恢复。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> ReturnToWaiting();

  /** @brief 装备初始化结束后重新开始完整对局时限；只允许开局安装前由 executor
   * 调用。 */
  void BeginAuthorizedMatchClock(std::chrono::steady_clock::time_point now);

  /** @brief 战斗账本安装成功后开启房间级移动快照 tick。 */
  void EnableCombatTick(std::chrono::steady_clock::time_point now);

  /** @brief 返回当前状态所有有效 timer 值；调用方只能据此安排异步计时。 */
  [[nodiscard]] std::vector<RoomTimerToken> ActiveTimers() const;

  /** @brief 按稳定座位升序复制当前成员快照。 */
  [[nodiscard]] std::vector<RoomMemberSnapshot> Members() const;

  /** @brief 按稳定座位复制参赛者，排除仍留在 members() 中的观战主办者。 */
  [[nodiscard]] std::vector<RoomMemberSnapshot> Participants() const;

  /** @brief 查找一个玩家的值快照；不存在返回空。 */
  [[nodiscard]] std::optional<RoomMemberSnapshot> GetMember(
      domain::PlayerId player_id) const;

  /** @brief 返回房间标识。 */
  [[nodiscard]] domain::RoomId RoomId() const noexcept;
  /** @brief 返回当前房主。 */
  [[nodiscard]] domain::PlayerId HostPlayerId() const noexcept;
  /** @brief 返回当前阶段。 */
  [[nodiscard]] RoomPhase Phase() const noexcept;
  /** @brief 返回每次阶段失效时递增的代次。 */
  [[nodiscard]] std::uint64_t Generation() const noexcept;
  /** @brief 返回配置容量。 */
  [[nodiscard]] std::uint16_t Capacity() const noexcept;
  /** @brief 返回包含固定观战席的原生会话槽位数量。 */
  [[nodiscard]] std::uint16_t SessionSlotCount() const noexcept;
  /** @brief 当前是否为由观战主办者出资的赏金房间。 */
  [[nodiscard]] bool UserHosted() const noexcept;
  /** @brief 返回当前影响队伍规则的模式编号。 */
  [[nodiscard]] std::uint32_t ModeId() const noexcept;
  /** @brief 网络选择仅由等待阶段房主修改，新房间固定默认服务器直连。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> SetNetworkMode(
      domain::PlayerId actor, protocol::RoomNetworkMode mode);
  /** @brief 同一次确认保存网络与房间规则，只有等待阶段的房主可以修改。 */
  [[nodiscard]] application::ServiceResult<RoomEvent> SetRoomSettings(
      domain::PlayerId actor, protocol::RoomNetworkMode mode,
      std::uint32_t global_rules);
  [[nodiscard]] std::uint32_t GlobalRules() const noexcept;

  [[nodiscard]] const std::vector<std::uint32_t>& RestrictedItems()
      const noexcept {
    return restricted_items_;
  }

  [[nodiscard]] std::uint64_t ItemRulesRevision() const noexcept {
    return item_rules_revision_;
  }

  [[nodiscard]] application::ServiceResult<RoomEvent> SetRestrictedItems(
      domain::PlayerId actor, std::vector<std::uint32_t> items);
  /** @brief 当前房间保存的网络偏好；不等同于已经建立P2P连接。 */
  [[nodiscard]] protocol::RoomNetworkMode NetworkMode() const noexcept;
  /** @brief 按升序复制当前锁定的空席位。 */
  [[nodiscard]] std::vector<std::uint16_t> LockedSeats() const;

 private:
  friend class RoomRegistry;

  /** @brief executor 内部可变成员记录；只能通过值快照离开房间对象。 */
  struct Member final {
    domain::PlayerId player_id;
    std::uint16_t seat{0U};
    std::uint8_t team{1U};
    bool ready{false};
    bool assets_received{false};
    bool load_completed{false};
    std::optional<std::uint32_t> object_id;
  };

  /** @brief registry 删除成员索引后消费的内部离开结果。 */
  struct LeaveState final {
    std::uint16_t departed_seat{0U};
    std::optional<domain::PlayerId> new_host;
    bool start_interrupted{false};
    bool closed{false};
  };

  RoomRuntime(domain::RoomId room_id, domain::PlayerId host,
              RoomCreateSpec spec);

  [[nodiscard]] application::ServiceResult<RoomEvent> AddMember(
      domain::PlayerId player_id, std::string_view password);
  [[nodiscard]] LeaveState RemoveMember(domain::PlayerId player_id);
  [[nodiscard]] application::ServiceResult<RoomEvent> RecordLoadSignal(
      domain::PlayerId player_id, bool assets,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool AllMembersLoaded() const noexcept;
  [[nodiscard]] RoomEvent AdvanceAfterLoading(
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] RoomEvent EnterPlaying(
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] RoomEvent EnterResults(
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] RoomEvent ResetToWaiting();
  [[nodiscard]] static RoomMemberSnapshot SnapshotOf(const Member& member);
  void ResetReadyState() noexcept;
  void ResetTransientMemberState() noexcept;
  void InvalidatePhase() noexcept;

  domain::RoomId room_id_;
  domain::PlayerId host_player_id_;
  std::uint16_t capacity_{12U};
  std::uint32_t mode_id_{1U};
  std::string password_;
  bool user_hosted_{false};
  bool hide_seek_v1_{false};
  RoomPhase phase_{RoomPhase::kWaiting};
  std::uint64_t generation_{1U};
  protocol::RoomNetworkMode network_mode_{
      protocol::RoomNetworkMode::kServerDirect};  ///< 每个新房间独立初始化。
  std::uint32_t global_rules_{0U};                ///< 每个新房间默认允许手雷。
  std::map<domain::PlayerId, Member> members_;
  std::vector<std::uint32_t> restricted_items_;
  std::uint64_t item_rules_revision_{0U};
  std::set<std::uint16_t> locked_seats_;
  RoomMatchTiming timing_;
  std::optional<std::chrono::steady_clock::time_point> loading_deadline_;
  std::optional<std::chrono::steady_clock::time_point> countdown_deadline_;
  std::optional<std::chrono::steady_clock::time_point> match_deadline_;
  std::optional<std::chrono::steady_clock::time_point> result_display_deadline_;
  std::optional<std::chrono::steady_clock::time_point> result_return_deadline_;
  std::optional<std::chrono::steady_clock::time_point> objective_deadline_;
  std::optional<std::chrono::steady_clock::time_point> combat_tick_deadline_;
  bool round_loading_{false};
};

/**
 * @brief 在单一 registry executor 上维护房间所有权和玩家唯一成员索引。
 * @note 房间以 shared_ptr 持有，异步方只能保存 weak_ptr 加 RoomTimerToken；从
 * registry 删除 后旧 completion 即使锁住对象，也会被 closed/generation
 * 校验拒绝。
 */
class RoomRegistry final {
 public:
  /** @brief 可选固定房间号用于独立 DS；默认保留本地多房间注册行为。 */
  explicit RoomRegistry(
      std::optional<domain::RoomId> isolated_room = std::nullopt);
  /** @brief 创建最多 128 个房间之一，并为房主分配座位 0。 */
  [[nodiscard]] application::ServiceResult<std::shared_ptr<RoomRuntime>> Create(
      domain::PlayerId host, RoomCreateSpec spec);

  /** @brief 在等待房间校验口令并分配最低可用稳定座位。 */
  [[nodiscard]] application::ServiceResult<std::shared_ptr<RoomRuntime>> Join(
      domain::PlayerId player_id, domain::RoomId room_id,
      std::string_view password);

  /** @brief 玩家不在房间时返回空；最后一人离开会原子删除房间和成员索引。 */
  [[nodiscard]] std::optional<RoomDeparture> Leave(domain::PlayerId player_id);

  /** @brief 按玩家成员索引查找房间。 */
  [[nodiscard]] std::shared_ptr<RoomRuntime> FindByPlayer(
      domain::PlayerId player_id) const;

  /** @brief 按房间标识查找；不存在返回空。 */
  [[nodiscard]] std::shared_ptr<RoomRuntime> FindById(
      domain::RoomId room_id) const;

  /** @brief 返回活动房间数。 */
  [[nodiscard]] std::size_t RoomCount() const noexcept;

 private:
  [[nodiscard]] std::optional<domain::RoomId> AllocateRoomId();

  static constexpr std::size_t kMaximumRooms = 128U;
  static constexpr std::uint64_t kMaximumWireRoomId = 32767U;
  std::uint64_t next_room_id_{0U};
  std::optional<domain::RoomId> isolated_room_;
  bool isolated_room_used_{false};
  std::map<domain::RoomId, std::shared_ptr<RoomRuntime>> rooms_;
  std::map<domain::PlayerId, domain::RoomId> membership_;
};

/** @brief 返回用于日志、指标和测试的稳定房间阶段名。 */
[[nodiscard]] std::string_view RoomPhaseName(RoomPhase phase) noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_RUNTIME_H_
