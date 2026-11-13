#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_EXECUTOR_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_EXECUTOR_H_

/**
 * @file world_executor.h
 * @brief 定义独占世界/房间状态、命令队列和房间计时器的串行执行器。
 */

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "application/background_task_executor.h"
#include "application/error.h"
#include "application/event_logger.h"
#include "application/system_clock.h"
#include "features/authentication/world_access.h"
#include "features/battle/durable_match_settlement_flow.h"
#include "features/battle/ice_service.h"
#include "features/battle/match_lifecycle_repair.h"
#include "features/battle/match_preparation.h"
#include "features/battle/match_start.h"
#include "features/inventory/item_use_service.h"
#include "features/practice/tutorial_session.h"
#include "protocol/codec/frame_codec.h"
#include "sessions/equipment_relay_broker.h"
#include "sessions/outbound_message.h"
#include "sessions/session_registry.h"
#include "sessions/world_coordinator.h"

namespace bf::runtime {

/** @brief 世界命令完成后回到发起 Session 的一次性回调。 */
using WorldCommandCompletion = std::function<void(
    application::ServiceResult<std::vector<OutboundMessage>>)>;

/** @brief 一次换装数据库工作完成后拥有的本人回复和可选已提交快照。 */
struct EquipmentWorldResult final {
  std::vector<OutboundMessage> replies;  ///< 原生结果、库存、装备消息。
  std::optional<application::PlayerMenuSnapshot>
      changed;  ///< 成功换装后的完整资料。
  std::vector<domain::PlayerId>
      clan_members;             ///< 徽章事务提交后需要刷新外观的家族成员。
  std::uint32_t clan_mark{0U};  ///< 与成员列表对应的已提交徽章编号。
  bool level_icon_only{
      false};  ///< 外观图标使用独立 973，不触发换装或取消准备。
  bool refresh_menu_slot{
      false};  ///< 主菜单主动换装也更新席位零；其他资料变更保持原消息链。
};

/** @brief
 * 世界线程冻结阶段授权与战斗模式后交给数据库线程；拒绝时只构造失败回复。 */
using EquipmentWorldTask =
    std::function<application::ServiceResult<EquipmentWorldResult>(
        bool, std::uint32_t)>;

/** @brief 即时复活扣券事务的授权结果及已提交库存回包。 */
struct InstantRespawnWorldResult final {
  bool authorized{false};
  std::vector<OutboundMessage> replies;
};

using InstantRespawnWorldTask =
    std::function<application::ServiceResult<InstantRespawnWorldResult>(
        const application::UseItemCommand&)>;

/** @brief 喇叭后台事务返回的耐久记录、屏蔽快照及本人库存刷新。 */
struct MegaphoneWorldResult final {
  std::optional<application::MegaphoneReceipt> receipt;
  std::vector<domain::PlayerId> blocked;
  std::vector<OutboundMessage> replies;
};

using MegaphoneWorldTask =
    std::function<application::ServiceResult<MegaphoneWorldResult>(
        const application::MegaphoneCommand&)>;

/** @brief 房间后端端口；跨进程实现的实时输入直接离开大厅命令队列。 */
class RoomExecution {
 public:
  virtual ~RoomExecution() = default;
  [[nodiscard]] virtual bool Routes(const WorldActorContext& actor,
                                    const protocol::Packet& packet) const = 0;
  [[nodiscard]] virtual bool Contains(domain::PlayerId player) const = 0;
  [[nodiscard]] virtual std::optional<application::ServiceError> Post(
      WorldActorContext actor, protocol::Packet packet,
      WorldCommandCompletion completion) = 0;
  virtual void Disconnect(domain::PlayerId player,
                          std::optional<SessionHandle> origin) = 0;
  [[nodiscard]] virtual std::optional<application::ServiceError> Equipment(
      domain::PlayerId player, EquipmentWorldTask work,
      WorldCommandCompletion completion, bool preset_selection,
      std::optional<SessionHandle> origin) = 0;
  [[nodiscard]] virtual std::optional<application::ServiceError> InstantRespawn(
      domain::PlayerId player, std::uint32_t object,
      InstantRespawnWorldTask work, WorldCommandCompletion completion) = 0;
  [[nodiscard]] virtual bool SettlementResult(
      application::MatchSettlementResult result) = 0;
};

/** @brief 世界执行器的有界队列和统一房间计时配置。 */
struct WorldExecutorOptions final {
  std::size_t queue_capacity{4096U};  ///< 尚未执行的命令、断线事件总上限。
  RoomMatchTiming match_timing{};     ///< 所有新房间冻结使用的阶段时长。
  std::chrono::milliseconds settlement_retry_delay{
      std::chrono::seconds(1)};  ///< stage 失败重试间隔。
  std::chrono::milliseconds start_compensation_retry_delay{
      std::chrono::seconds(1)};  ///< 已提交开局安装失败后的数据库补偿重试间隔。
  std::chrono::milliseconds recovery_interval{
      std::chrono::seconds(5)};           ///< 耐久日志周期恢复间隔。
  std::size_t recovery_batch_size{256U};  ///< 每次周期恢复的最大命令数。
  EquipmentRelayBroker* equipment_relay{
      nullptr};  ///< 正式组装必须提供，隔离测试可不提供。
  std::array<std::uint8_t, 4U>
      equipment_relay_address{};  ///< 客户端可达 IPv4。
  application::PlayerMenuSnapshotLoader* prize_menus{
      nullptr};  ///< 赏金提交后重新读取完整库存/钱包。
  std::uint16_t equipment_relay_port{0U};  ///< 原生独立 Relay 监听端口。
  application::TutorialSessionStore* tutorial_sessions{
      nullptr};  ///< 原103报告账本；无端口不开放107。
  application::IceLeaseProvider* ice_leases{
      nullptr};  ///< Issued only to authenticated P2P room members.
  RoomExecution* room_execution{
      nullptr};  ///< 可选独立 DS 后端，由组合根在停机前排空。
};

/** @brief 世界线程最近一次完成事件后发布的无锁运维快照。 */
struct WorldExecutorMetrics final {
  std::size_t queued_tasks{0U};          ///< 尚未开始执行的外部或内部任务。
  std::size_t active_rooms{0U};          ///< 当前活动房间数。
  std::size_t plaza_players{0U};         ///< 当前自由广场成员数。
  std::size_t scheduled_timers{0U};      ///< 尚未到期的房间阶段计时器。
  std::size_t pending_match_starts{0U};  ///< 等待持久化或安装的开局数。
  std::size_t pending_match_settlements{
      0U};  ///< 等待耐久 journal 回执的终局数。
  std::size_t pending_match_compensations{
      0U};  ///< 等待数据库中断记录的失败开局数。
};

/**
 * @brief 在唯一工作线程顺序执行全部广场、房间、战斗、timer 和断线事件。
 * @note Session 只提交拥有型值；本类不保存 socket 或 Session
 * 裸指针。结果按玩家交给 `SessionRegistry` mailbox，避免多个 I/O strand
 * 并发修改同一房间。
 */
class WorldExecutor final {
 public:
  /** @brief 保存外部端口；registry/logger 生命周期必须覆盖本对象。 */
  WorldExecutor(WorldExecutorOptions options, SessionRegistry& sessions,
                application::BackgroundTaskExecutor& background_tasks,
                application::MatchPreparationExecutor& match_preparation,
                application::MatchIdSource& match_ids,
                application::MatchStartExecutor& match_starts,
                application::MatchLifecycleRepairExecutor& match_repairs,
                const application::SystemClock& clock,
                DurableMatchSettlementFlow& settlements,
                application::EventLogger& logger,
                application::ItemUseExecutor* megaphones = nullptr,
                application::WorldAccessLoader* megaphone_access = nullptr);
  ~WorldExecutor();

  WorldExecutor(const WorldExecutor&) = delete;
  WorldExecutor& operator=(const WorldExecutor&) = delete;
  WorldExecutor(WorldExecutor&&) = delete;
  WorldExecutor& operator=(WorldExecutor&&) = delete;

  /** @brief 启动唯一 worker；重复启动返回状态错误。 */
  [[nodiscard]] std::optional<application::ServiceError> Start();

  /** @brief 非阻塞提交一条已加载数据库上下文的世界命令。 */
  [[nodiscard]] std::optional<application::ServiceError> Post(
      WorldActorContext actor, protocol::Packet packet,
      WorldCommandCompletion completion);

  /** @brief 菜单初始化成功后串行登记邀请可见的在线资料。 */
  [[nodiscard]] std::optional<application::ServiceError> PostPlayerOnline(
      application::PlayerMenuSnapshot snapshot,
      std::optional<SessionHandle> origin = std::nullopt);
  [[nodiscard]] std::optional<application::ServiceError> PostCoupleProfiles(
      std::vector<application::PlayerMenuSnapshot> snapshots);

  /** @brief 非阻塞提交断线清理；关闭后或队列满时记录错误并由调用方继续释放
   * Session。 */
  [[nodiscard]] std::optional<application::ServiceError> PostDisconnect(
      domain::PlayerId player_id, WorldSessionPhase phase,
      std::optional<SessionHandle> origin = std::nullopt);

  /** @brief 只在进出 DS 时串行变更大厅成员关系，不保存房间或战斗状态。 */
  [[nodiscard]] std::optional<application::ServiceError> PostRoomHandoff(
      WorldActorContext actor, bool entering,
      WorldCommandCompletion completion);

  /** @brief
   * 从数据库工作线程移交已提交奖励；回到世界线程后才刷新同局成绩和资料。 */
  [[nodiscard]] bool PostSettlementResult(
      application::MatchSettlementResult result) noexcept;

  /** @brief
   * 串行预留换装期间的房间，避免与开局/离场交叉；数据库工作不阻塞世界线程。 */
  [[nodiscard]] std::optional<application::ServiceError> PostEquipmentChange(
      domain::PlayerId player, EquipmentWorldTask work,
      WorldCommandCompletion completion, bool preset_selection = false,
      std::optional<SessionHandle> origin = std::nullopt);
  /** @brief 冻结当前生命，在库存提交期间保留后续出生握手与离场事件。 */
  [[nodiscard]] std::optional<application::ServiceError> PostInstantRespawn(
      domain::PlayerId player, std::uint32_t object,
      InstantRespawnWorldTask work, WorldCommandCompletion completion);
  /** @brief 世界线程冻结频道后异步扣除库存，仅广播耐久提交后的记录。 */
  [[nodiscard]] std::optional<application::ServiceError> PostMegaphone(
      application::MegaphoneCommand command, MegaphoneWorldTask work,
      WorldCommandCompletion completion);

  /** @brief 停止接收、排空已接受命令、取消未到期 timer 并合并 worker。 */
  void DrainAndStop() noexcept;

  /** @brief 返回等待执行的外部任务数快照。 */
  [[nodiscard]] std::size_t QueuedTaskCount() const noexcept;

  /** @brief 返回房间、广场、计时器和持久化待办的线程安全运维快照。 */
  [[nodiscard]] WorldExecutorMetrics Metrics() const noexcept;

 private:
  /** @brief 隐藏串行队列、房间计时器和唯一 worker 的具体实现。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_WORLD_EXECUTOR_H_
