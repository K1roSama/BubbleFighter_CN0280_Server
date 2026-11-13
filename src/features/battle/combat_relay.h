#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_COMBAT_RELAY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_COMBAT_RELAY_H_

/**
 * @file combat_relay.h
 * @brief 定义单场比赛内角色动作与弹丸命中的认证转发账本。
 */

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/match_reward.h"
#include "features/battle/native_team_spawn.h"
#include "protocol/battle/combat_messages.h"
#include "protocol/battle/smart_ping_messages.h"
#include "protocol/codec/frame_codec.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

inline constexpr std::size_t kNativeWeaponBarCount =
    5U;  ///< 客户端切枪/开火栏位数量。

/** @brief 比赛开始时冻结的一名玩家对象 ID 和原生武器模板配装。 */
struct CombatParticipant final {
  domain::PlayerId player_id;         ///< 认证玩家。
  std::uint32_t actor_object_id{0U};  ///< 本场 Character 对象编号。
  std::uint8_t team_code{0U};  ///< 开局冻结队伍；普通玩家对局只允许 1/2。
  std::array<std::optional<std::uint32_t>, kNativeWeaponBarCount>
      weapon_templates;  ///< 每栏允许的原生模板；空栏不可切换或开火。
  bool ordinary_respawn_allowed{
      true};  ///< 当前模式是否允许 704/32926 普通复活链。
  std::uint8_t seat_index{
      0U};                    ///< 开局冻结席位；与 Character 对象编号分开保存。
  bool lucci_pouch{false};    ///< 数据库开局快照确认的福袋能力。
  bool chocolate_box{false};  ///< 数据库开局快照确认的巧克力掉落能力。
  std::uint32_t fighter{1U};  ///< 已装备角色；4 的原生补弹可额外恢复生命。
  std::vector<std::uint32_t>
      equipment_items;  ///< 数据库确认的装备，供热血复活减时计算。
  std::optional<std::uint32_t>
      ordinary_spawn_point;  ///< 由房间冻结的普通团队出生点。
  std::optional<NativeTeamSpawnRange>
      ordinary_spawn_range;  ///< 原狙击特例的冻结同队随机范围。
  std::vector<NativeTeamSpawnPose>
      ordinary_spawn_poses;  ///< 由房间冻结的原WB出生姿态。
};

/** @brief 服务端接受的受击者生命周期状态；不是完整生命值模型。 */
enum class CombatLifeState : std::uint8_t {
  kAlive,
  kBubbled,
  kDefeated,
};

/** @brief 已认证实时消息被忽略但连接无需关闭的稳定原因。 */
enum class CombatRejectReason : std::uint8_t {
  kInactiveMatchPhase,
  kRateLimited,
  kDuplicateAction,
  kDuplicateProjectile,
  kProjectileLimit,
  kUnknownProjectile,
  kForeignProjectile,
  kTerminalProjectile,
  kUnknownTarget,
  kDuplicateImpact,
  kDuplicateLifecycle,
  kInvalidLifecycleState,
  kUnknownRelatedActor,
  kRespawnDisabled,
  kRespawnMismatch,
  kUnrecoveredAction,
  kModeWeaponRestricted,  ///< 原生模式禁止使用该武器栏，拒绝时不消耗弹丸或动作状态。
  kHealthOutOfRange,
  kRespawnDurationMismatch,
};

/** @brief 一条发送给指定比赛成员的已验证消息。 */
struct CombatDelivery final {
  domain::PlayerId
      recipient;  ///< 普通转发为同场其他成员；服务端响应可指向请求者本人。
  OutboundMessage
      message;  ///< 原 opcode/body 的拥有型副本，或服务端生成的定向响应。

  /** @brief 按接收玩家和同行消息比较。 */
  bool operator==(const CombatDelivery&) const = default;
};

/** @brief 房间 tick 刷新的一条最新移动快照；移动允许跨输入丢弃旧值。 */
struct CombatMovementDelivery final {
  domain::PlayerId sender;     ///< 产生该快照的认证真人。
  domain::PlayerId recipient;  ///< 当前仍在房间内、需要观察该快照的玩家。
  OutboundMessage message;     ///< 32769/32885 的最新原生负载。

  /** @brief 按发送者、接收者和完整消息比较。 */
  bool operator==(const CombatMovementDelivery&) const = default;
};

/** @brief 分开正常入站低counter与17/21原本机routing1；不推定routing2高counter。
 */
enum class NativeConstructorLane : std::uint8_t {
  kPeerIncoming,
  kMode17OwnerLoopback,
  kMode21OwnerLoopback,
};

/** @brief 已认证出站构造计划及17/21本机队列事实；不授予对象ID或初始快照权限。
 */
struct NativeConstructorFact final {
  domain::PlayerId
      receiver;  ///< 实际出站目标，或由独立lane标识的17/21本机队列拥有者。
  std::uint32_t owner_object{
      0U};  ///< 已认证真人对象；counter号段owner不能取自任意payload。
  std::uint16_t opcode{
      0U};  ///< 已通过身份、阶段、配装与去重校验的实际构造报文。
  std::uint32_t count{0U};  ///< spread是实际原生构造次数，不是命中或分值。
  std::uint64_t first_observed_ordinal{
      0U};  ///< 本场已观察构造序号；不是原生对象ID。
  std::uint64_t source_life{
      0U};  ///< 原CombatRelay生命代次允许初始0，不转成客户端声明。
  NativeConstructorLane lane{
      NativeConstructorLane::kPeerIncoming};  ///< 不混合高/低counter域。
  bool order_uncertain{
      false};  ///< 17/21被拒构造可能已本机入队，后续成功不能消除此缺口。
};

/** @brief 一条实时消息的接受状态、忽略原因和同行转发。 */
struct CombatRelayResult final {
  bool accepted{false};  ///< 是否提交了账本变化并允许转发。
  std::optional<CombatRejectReason>
      reject_reason;  ///< 忽略消息时的可审计原因。
  std::vector<CombatDelivery>
      deliveries;  ///< 接受时按 PlayerId 升序；复活授权只包含请求者。
  std::vector<NativeConstructorFact>
      native_constructors;  ///< 同时已消费到接收端有序计数账本。
};

/**
 * @brief 单场比赛 executor 独占的实时动作与弹丸最小权威账本。
 * @note 本类只验证身份、字段、装备引用、速率、cookie
 * 和目标范围；客户端仍执行碰撞、伤害及
 *       武器修正。不得把“允许转发”表述为服务端权威伤害判定。
 */
class CombatRelay final {
 public:
  /** @brief 冻结参赛者与仅接收广播的观战者；无效/重复身份、空参赛名单抛
   * `std::invalid_argument`。
   */
  explicit CombatRelay(const std::vector<CombatParticipant>& participants,
                       bool individual_scoring = false,
                       const std::vector<domain::PlayerId>& observers = {},
                       std::uint32_t mode_id = 0U,
                       bool grenades_forbidden = false);

  /** @brief 更新当前房主；只允许其严格合法的其他成员预测动作被无副作用忽略。 */
  void SetHost(domain::PlayerId player_id) noexcept;

  CombatRelay(const CombatRelay&) = delete;
  CombatRelay& operator=(const CombatRelay&) = delete;

  /**
   * @brief 处理已解密动作或命中消息。
   * @note 固定布局畸形、冒充对象或装备不一致抛
   * ProtocolError，应关闭发送连接；重复/过期等
   *       可由网络重发或迟到造成的状态拒绝只返回 reject_reason。
   */
  [[nodiscard]] application::ServiceResult<CombatRelayResult> Handle(
      domain::PlayerId actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 启用房间级移动
   * tick；未启用时保留旧的逐包转发语义，便于独立账本调用方。 */
  void EnableMovementTickAggregation(bool enabled = true) noexcept;

  /** @brief 取出本 tick 每名角色的最新移动，并按接收者展开广播。 */
  [[nodiscard]] std::vector<CombatMovementDelivery> FlushMovementTick();

  /** @brief 当前尚未发送的角色移动快照数。 */
  [[nodiscard]] std::size_t PendingMovementCount() const noexcept;

  /** @brief 清空动作、限流和弹丸状态；新一场比赛必须调用或创建新对象。 */
  void Reset() noexcept;

  /** @brief 新爆破回合重置瞬时账本但保留整局统计。 */
  void ResetRound() noexcept;

  /**
   * @brief 转发本场真实玩家的智能标记给同队（含本人回显）。
   * @note 发起者对象不属于认证玩家抛
   * ProtocolError；模式不允许或一秒冷却内只返回 reject_reason；观战者、海盗 NPC
   * 与足球返回 kPermissionDenied。
   */
  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleSmartPing(
      domain::PlayerId sender, const protocol::SmartPing& ping,
      std::chrono::steady_clock::time_point now);

  /** @brief 仅被击败且尚未授权出生的真实玩家可切换下次出生配装。 */
  [[nodiscard]] bool CanChangePreset(domain::PlayerId player) const noexcept;
  /** @brief 仅当前死亡生命、尚未出生且允许普通复活的本人可购买即时复活。 */
  [[nodiscard]] bool CanInstantRespawn(domain::PlayerId player,
                                       std::uint32_t object) const noexcept;
  /** @brief 库存事务提交后给当前生命记账，供 32926 校验。 */
  [[nodiscard]] bool AuthorizeInstantRespawn(domain::PlayerId player,
                                             std::uint64_t life) noexcept;
  /** @brief 已扣除的对局道具，为后续属性/回血包分配限时、按目标一次性凭据。 */
  void AuthorizeItemEffects(domain::PlayerId player, std::uint32_t type,
                            std::uint32_t mode,
                            std::chrono::steady_clock::time_point now);

  /** @brief 从已验证的服务器库存快照更新死亡玩家配装，不改变生命或统计。 */
  [[nodiscard]] bool ChangePreset(
      domain::PlayerId player,
      const std::array<std::optional<std::uint32_t>, 5U>& weapons,
      std::optional<std::uint32_t> fighter = std::nullopt,
      std::vector<std::uint32_t> equipment = {});

  /** @brief 对提前到达的热血复活请求，等待可信期限后仅向本人发送一次 707。 */
  [[nodiscard]] std::vector<CombatDelivery> TickRespawns(
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  RespawnDeadline() const;

  /** @brief 返回确认生命周期代次，供掉落与请求关联去重。 */
  [[nodiscard]] std::optional<std::uint64_t> LifeGeneration(
      domain::PlayerId player_id) const;

  /** @brief 对局内只做本机表现的对等包：32824 对象事件（7CA300，16
   * 字节）、32832 角色召唤物 （7CD430，32 字节）、33275 对象状态（7D6410，8
   * 字节）。 */
  [[nodiscard]] static bool IsPeerEffectOpcode(std::uint16_t opcode) noexcept;
  /** @brief 严格长度；首 DWORD
   * 必须是发送者本人角色对象；限速后原样转发给其他成员。 */
  [[nodiscard]] CombatRelayResult RelayPeerEffect(
      domain::PlayerId sender, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 返回当前未过清理窗口（含终态）的弹丸 cookie 数，供指标与测试使用。
   */
  [[nodiscard]] std::size_t ProjectileCount() const noexcept;

  /** @brief
   * 真实接收端已观察的低counter构造次数；不包含未观察的本机高counter预测。
   * @note 计数没有初始2018/982 checkpoint授权能力，不能直接转换成ChrMole ID。
   */
  [[nodiscard]] std::uint64_t ObservedNativeConstructors(
      domain::PlayerId receiver, std::uint32_t owner_object) const noexcept;
  /** @brief 17/21被拒构造可能已消费本机routing1；此诊断不证明初始对象计数完整。
   */
  [[nodiscard]] bool NativeLoopbackOrderUncertain(
      std::uint32_t owner_object) const noexcept;

  /** @brief 返回玩家本场已接受的压缩开火数；玩家不存在返回空值。 */
  [[nodiscard]] std::optional<std::uint64_t> AcceptedShots(
      domain::PlayerId player_id) const;

  /** @brief 返回最后有效移动或可信复活位置；未知或玩家不存在返回空值。 */
  [[nodiscard]] std::optional<std::array<float, 3U>> LastPosition(
      domain::PlayerId player_id) const;

  /** @brief 返回受击者当前最小生命周期状态；玩家不存在返回空值。 */
  [[nodiscard]] std::optional<CombatLifeState> LifeState(
      domain::PlayerId player_id) const;

  /** @brief
   * 返回由已接受受击者报告累计的泡封、击爆、死亡和救援；玩家不存在返回空值。 */
  [[nodiscard]] std::optional<domain::MatchPerformance> Performance(
      domain::PlayerId player_id) const;

  /**
   * @brief 将离场玩家及其对象从转发目标集合移除，并返回离场前确认统计。
   * @note
   * 调用方必须先把返回统计复制到本局终局快照；移除后该对象和其未终结弹丸均不再有效。
   */
  [[nodiscard]] std::optional<domain::MatchPerformance> RemoveParticipant(
      domain::PlayerId player_id) noexcept;

 private:
  /** @brief 服务端实际授予的单次后续物品效果凭据。 */
  struct ItemEffectPermit final {
    domain::PlayerId source;
    std::uint64_t source_life{0U};
    domain::PlayerId target;
    std::uint64_t target_life{0U};
    std::uint16_t opcode{0U};
    std::uint32_t type{0U};
    std::uint32_t attribute{0U};
    std::uint32_t duration{0U};
    std::chrono::steady_clock::time_point expires{};
    std::chrono::steady_clock::time_point created{};
    float radius{0.0F};  ///< 非零为已消费范围道具，命中还需最近位置和原生范围。
  };

  std::vector<ItemEffectPermit> item_effects_;
  friend class PirateMatch;
  friend class HordeTutorMatch;
  friend class PirateMapAgents;
  friend class InfectionMatch;
  friend class WeaponMasterMatch;
  friend class ExtremeRacingMatch;
  friend class CaptainAssignments;
  friend class ZombiePveMatch;
  friend class MatchObjectives;
  friend class RoomCommandFlow;
  friend class TrainingRoomMatch;

  /** @brief 羞辱的接近动作，绑定双方当前生命；结束不得直接产生击杀。 */
  struct PendingHumiliation final {
    std::uint64_t source_life{0U};
    std::uint64_t target_life{0U};
    std::chrono::steady_clock::time_point started{};
  };

  /** @brief 已完成羞辱只授权目标本生命的限量掉落。 */
  struct HumiliationEvent final {
    std::uint64_t serial{0U};
    std::uint64_t life{0U};
    std::chrono::steady_clock::time_point completed{};
    domain::PlayerId source;  ///< 认证动作来源，供感染模式判定缩短倒计时。
  };

  /** @brief 已确认击爆的来源、生命代次和掉落接受期限起点。 */
  struct DefeatEvent final {
    std::uint32_t attacker{0U};
    std::uint64_t life{0U};
    std::chrono::steady_clock::time_point time{};
    std::optional<std::uint64_t>
        bubble_life;  ///< 若从泡封击爆，保留原生命以关联提前到达的队长报告。
    std::optional<std::array<float, 3U>>
        position;  ///< 确认击败包中的位置，供海盗掉落关联。
  };

  /** @brief 某次泡封内等待受害者确认的踢爆请求。 */
  struct PendingKick final {
    std::uint32_t attacker{0U};    ///< 已认证攻击者。
    std::uint64_t generation{0U};  ///< 请求所属泡封代次。
    std::chrono::steady_clock::time_point
        requested_at{};  ///< 本地五秒结果接受窗口起点。
  };

  /** @brief 同一次泡封中待确认的营救请求。 */
  struct PendingRescue final {
    std::uint32_t helper{0U};                      ///< 认证营救者。
    std::uint64_t generation{0U};                  ///< 泡封生命代次。
    std::chrono::steady_clock::time_point time{};  ///< 五秒确认窗口。
    bool recovered{false};  ///< 恢复已确认，等待原生计分效果。
    std::uint8_t kind{0U};  ///< 完成效果必须对应本次营救类型。
  };

  [[nodiscard]] CombatRelayResult HandleRescue(
      domain::PlayerId sender, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  /** @brief 每名参与者的限流、位置和开火去重状态。 */
  struct ActorState final {
    /** @brief 竞速核心授予的当前赛段窗口；活人复位只在该赛段首次 704/32926
     * 链消费。 */
    struct RacingStage final {
      std::uint32_t stage{0U};
      std::uint32_t spawn_point{
          0U};  ///< 7BFF70 按冻结席位和同队序号生成，超过六人按模六回绕。
      std::array<float, 3U>
          spawn_position{};  ///< 当前赛段原始 WB 出生点，仅在授权完成后采用。
      bool needs_reset{false};
      std::optional<std::chrono::steady_clock::time_point> begins;
      std::optional<std::chrono::steady_clock::time_point> expires;
    };

    CombatParticipant participant;  ///< 本场冻结身份与配装。
    std::optional<std::chrono::steady_clock::time_point>
        rate_window;                     ///< 当前一秒窗口起点。
    std::size_t messages_in_window{0U};  ///< 当前窗口已接受解析的动作数。
    std::optional<std::chrono::steady_clock::time_point>
        last_smart_ping;                            ///< 标记冷却起点。
    std::optional<std::array<float, 3U>> position;  ///< 最近有效移动位置。
    std::optional<std::array<std::uint32_t, 3U>>
        last_fire;                     ///< 栏、serial、低 24 位 token。
    std::uint64_t accepted_shots{0U};  ///< 已登记非重复 cookie 的开火数。
    CombatLifeState life_state{
        CombatLifeState::kAlive};  ///< 经顺序校验后的受击者状态。
    domain::MatchPerformance
        performance;               ///< 本局确认统计；目标分由模式规则维护。
    bool life_settled{false};      ///< 泡封静止动画是否已确认。
    std::uint8_t death_stage{0U};  ///< 0/1/2 两段死亡动画进度。
    std::optional<std::uint32_t> respawn_point;   ///< 已授权且待完成的出生点。
    std::vector<std::uint8_t> protection_report;  ///< 最近一次不透明保护报告。
    /** @brief 本场已接受的32787正文；恢复、复活和回合重置不重新授权旧确认。 */
    std::set<std::vector<std::uint8_t>> defeat_reports;
    std::uint64_t life_generation{
        0U};  ///< 接受新的泡封/击败时递增，隔离旧生命请求。
    std::uint64_t item_inventory_generation{
        0U};  ///< 道具栏仅死亡或整局/回合重置失效，泡封保留。
    std::uint64_t item_drop_event{
        0U};  ///< 已确认泡封、阵亡或复活事件，供道具掉落去重。
    std::optional<std::chrono::steady_clock::time_point> item_drop_at;

    /** @brief 真实消费针筒/变箱后，本次泡封的原生自救凭据。 */
    struct ItemEscape final {
      std::uint64_t life{0U};
      std::chrono::steady_clock::time_point expires{};
      bool recovered{false};
    };

    std::optional<ItemEscape> item_escape;
    std::optional<PendingKick> pending_kick;  ///< 受害者当前待确认请求。
    std::map<std::uint32_t, PendingKick>
        bubble_impacts;  ///< 按攻击者保存当前泡封内的已验证弹丸命中。
    std::map<std::uint32_t, PendingKick>
        turret_impacts;  ///< 当前生命存活时的炮台命中；不能代替泡封后的第二次命中。
    std::optional<std::array<std::uint64_t, 3U>>
        active_kick;  ///< 攻击者动画的目标/参数/代次。
    std::optional<RacingStage>
        racing_stage;  ///< 仅已绑定的模式 24 使用；其他模式不获得跨关权限。
    std::optional<domain::PlayerId>
        controller;  ///< 海盗 NPC 的认证房主，空值代表真实成员。

    /** @brief 复活等待绑定已确认生命代次；到期不等于客户端已完成复活报告。 */
    struct RespawnWait {
      std::uint64_t life;
      std::int32_t duration_ms;
      std::chrono::steady_clock::time_point due;
      bool reported{false};
      std::optional<std::uint32_t> pending_point;
    };

    std::optional<RespawnWait> respawn_wait;
    bool pirate_coin_source{
        false};  ///< 仅由海盗波次计划登记，公主和地图代理不发金币。
    bool training_dummy{
        false};  ///< 私人训练场的固定木桩，允许真人救援同队木桩。
    bool soccer_ball{
        false};  ///< 仅足球模式登记的中立球，不参与生命和击杀统计。
    bool infection_actor{
        false};  ///< 感染局内僵尸可踩踏已完成死亡动画的同队子体。
    bool melee_uses_equipped_template{
        false};  ///< 脚本 NPC 可用已登记枪械模板创建近战对象。
    std::uint8_t storm_source{
        0U};  ///< 1=已登记 .KephiOI；2=原生地图 Boss；真人没有此能力。
    std::optional<std::chrono::steady_clock::time_point> last_storm_time;
    std::optional<
        std::pair<std::uint32_t, std::chrono::steady_clock::time_point>>
        storm_reaction;
    std::map<std::uint32_t, PendingHumiliation> humiliations;
    std::optional<HumiliationEvent> humiliated;
    std::optional<DefeatEvent> defeated;
    /** @brief 真实消费 138 或拾取 9021 后的本生命火箭筒及剩余两发上限。 */
    std::optional<std::pair<std::uint64_t, std::uint32_t>> bazooka;
    std::uint32_t bazooka_template{
        9021U};  ///< 临时道具武器：PVE9025、BFRunner9100。

    /** @brief
     * BFRunner真实消费147/234后的一次原生5047/5046投掷，限当前生命及窗口。 */
    struct RunnerThrow final {
      std::uint64_t life;
      std::uint32_t weapon;
      std::chrono::steady_clock::time_point expires;
    };

    std::optional<RunnerThrow> runner_throw;

    /** @brief 原128消费后最多三个32832香蕉召唤；不授予任意角色召唤物。 */
    struct RunnerBanana final {
      std::uint64_t life;
      std::uint32_t remaining;
      std::chrono::steady_clock::time_point expires;
    };

    std::optional<RunnerBanana> runner_banana;
    std::set<std::uint32_t>
        npc_blast_templates;  ///< Original Boss skill weapons, set by map
                              ///< registration.

    /** @brief 实际消费112后的原9001武器；35发来自WeaponSpecDetail Item/9001。
     */
    struct GiantWeapon final {
      std::uint64_t life{0U};
      std::chrono::steady_clock::time_point expires{};
      std::uint32_t remaining_ammunition{35U};
    };

    std::optional<GiantWeapon> giant_weapon;

    /** @brief 一次真实空袭的十枚炸弹额度；已起飞飞机可跨施放者死亡继续投弹。 */
    struct Airstrike final {
      std::chrono::steady_clock::time_point created;
      std::uint32_t remaining{10U};
    };

    std::vector<Airstrike> airstrikes;
    std::optional<std::uint32_t>
        captain_weapon;  ///< 988/32894实际消费后才允许第4栏队长近战。

    /** @brief 82C8F0复活计数与707/104、32894、32828/11的本生命单次授权。 */
    struct DeathmatchFever final {
      std::uint32_t respawns_without_pang{0U};
      std::optional<std::uint64_t> completed_life;
      std::optional<std::uint64_t> claimed_life;
      std::optional<std::chrono::steady_clock::time_point> requested;
      bool item_applied{false};
      std::optional<std::chrono::steady_clock::time_point> active_until;
      std::uint64_t active_life{0U};
    };

    DeathmatchFever deathmatch_fever;

    /** @brief 队长104实际使用后的一次5211/5213爆风，不借用手雷栏权限。 */
    struct CaptainBlast final {
      std::uint16_t weapon;
      std::chrono::steady_clock::time_point expires;
      bool consumed{false};
    };

    std::optional<CaptainBlast> captain_blast;
    std::optional<std::uint64_t> instant_respawn_life;
    std::optional<std::pair<std::uint64_t, float>>
        health_report;  ///< 按生命隔离的本人 HP 显示同步。
    std::optional<std::chrono::steady_clock::time_point> health_report_at;
    bool survival_alliance_pose{
        false};  ///< 本生命已确认的 Area32 Alliance 动作。

    /** @brief 绑定双方生命的近战请求；五秒为服务端迟到窗口，不是原生动画时长。
     */
    struct SurvivalMelee final {
      std::uint32_t victim{0U};
      std::uint64_t source_life{0U};
      std::uint64_t victim_life{0U};
      std::array<float, 3U> source_position{};
      std::chrono::steady_clock::time_point started{};
      std::chrono::steady_clock::time_point confirmed_at{};
      bool confirmed{false};
      bool ended{false};
    };

    std::optional<SurvivalMelee> survival_melee;
    std::uint8_t selected_weapon_bar{
        2U};  ///< 生存原生初始近战栏；只由已验证的 select 更新。
    std::uint16_t survival_medkits{
        0U};  ///< 538870 初始一个 580，加地图真实拾取，上限 999。
    std::set<std::uint32_t>
        survival_primary_weapons;  ///< 最多三个不同的原生主武器模板。
    std::map<std::uint32_t, std::uint32_t>
        survival_supplies;  ///< 按原生类型合并的物资账本。
    std::array<std::optional<std::uint32_t>, kNativeWeaponBarCount>
        initial_weapons;
    std::optional<std::chrono::steady_clock::time_point>
        survival_treatment_start;
    std::optional<std::chrono::steady_clock::time_point> survival_water_report;

    /** @brief 本场单个已消费药包的原生治疗效果。 */
    struct SurvivalHeal final {
      std::uint16_t id{0U};
      std::uint8_t stage{0U};
      std::chrono::steady_clock::time_point created{};
    };

    std::map<std::uint16_t, SurvivalHeal> survival_heals;
    std::set<std::uint16_t>
        survival_used_effects;  ///< 终止后保留，阻止编号复用。
    std::map<std::uint32_t,
             std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>
        area_items;  ///< 使用者生命、原生范围道具开始时刻，结束包撤销尚未使用的凭据。
    std::map<std::uint32_t,
             std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>
        area_reactions;  ///< 同一生命原生不可叠加效果的重复报告窗口。

    /** @brief 无cookie的32930等待同次原生调用紧随的32900确定真实投掷物。 */
    struct PendingAreaCast final {
      protocol::Packet packet;
      protocol::NativeAreaCast request;
      std::chrono::steady_clock::time_point created{};
      std::map<std::uint32_t, std::chrono::steady_clock::time_point>
          sources;  ///< 冻结cookie及创建时刻，过期后复用同号也不是原来源。
    };

    std::optional<PendingAreaCast> pending_area_cast;
    /** @brief 真实300消费后本生命的一次投掷资格，默认十秒传输窗口。 */
    std::optional<
        std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>
        turret_throw;
  };

  /** @brief 一次合法137召唤等待原生2206装配报告绑定隐式场景对象。 */
  struct PendingTurret final {
    domain::PlayerId owner;
    std::chrono::steady_clock::time_point created{};
    std::array<float, 3U> position{};
  };

  /** @brief NpcTurret专用生命周期；场景ID独立于所有伤害cookie。 */
  struct TurretState final {
    domain::PlayerId owner;
    std::chrono::steady_clock::time_point created{};
    std::chrono::steady_clock::time_point changed{};
    std::uint32_t state{0U};
    bool ended{false};
    std::optional<std::uint32_t> native_fire_tick;
    std::chrono::steady_clock::time_point last_fire_received{};
    std::chrono::steady_clock::time_point next_fire{};
  };

  std::vector<PendingTurret> pending_turrets_;
  std::map<std::uint32_t, TurretState> turrets_;
  /** @brief 仅处理物品300来源及已绑定炮台；其余动作交还既有账本。 */
  [[nodiscard]] std::optional<CombatRelayResult> HandleTurret(
      domain::PlayerId sender, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 消费已登记炮台投掷物，生成一次待绑定召唤。 */
  [[nodiscard]] CombatRelayResult SummonTurret(
      ActorState& owner, const protocol::Packet& packet,
      const protocol::NativeAreaCast& request,
      std::chrono::steady_clock::time_point now);
  /** @brief 新局/回合移除本服炮台身份及来源资格。 */
  void ResetTurrets() noexcept;
  /** @brief 已确认/授权复活才可消费连续未Pang奖励；应用包不得重发或跨生命。 */
  [[nodiscard]] bool DeathmatchFeverItem(
      domain::PlayerId player, bool apply,
      std::chrono::steady_clock::time_point now);
  /** @brief 核对实际属性11的模式时长，并消费一次应用或本人清理收据。 */
  [[nodiscard]] CombatRelayResult DeathmatchFeverAttribute(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 已确认的泡封/死亡撤销旧生命热血，并同步原生属性清理。 */
  void ClearDeathmatchFever(ActorState& actor, CombatRelayResult& result);
  /** @brief 当前生命的临时武器优先，开局库存配装保持原值。 */
  [[nodiscard]] static std::optional<std::uint32_t> EquippedTemplate(
      const ActorState& actor, std::uint32_t bar) noexcept;

  /** @brief 一个本场 cookie 的创建者、目标集合和终态。 */
  struct ProjectileState final {
    /** @brief 原100%贯穿的开火快照；死亡/换页不撤销已发出的本次弹丸。 */
    struct NativePiercing final {
      std::uint32_t weapon_template{0U};
      std::uint8_t source_team{0U};
      std::uint64_t source_life{0U};
      std::size_t maximum_character_targets{0U};
      bool ally{false};
      bool ally_bubble{false};
      bool enemy_bubble{false};
    };

    std::uint32_t owner_object_id{0U};  ///< 接受开火时绑定的对象。
    std::chrono::steady_clock::time_point created_at{};  ///< 有界保留窗口起点。
    std::set<std::uint32_t> targets;  ///< 已命中过的对象/世界标识。
    bool terminal{
        false};  ///< 普通keep=0终止；已冻结原100%贯穿保留同帧不同目标回报。
    std::uint8_t weapon_bar{0U};  ///< 冻结的开火栏；手雷引爆要求 3。
    bool attached{false};         ///< 黏附手雷每个 cookie 只允许附着一次。
    bool destroyed{false};        ///< 删除回执独立于命中/引爆终态。
    bool self_only{false};        ///< 本机环境伤害只能命中创建者本人。
    std::optional<std::uint64_t>
        self_life;  ///< 竞速环境damager绑定创建者生命；复活后旧cookie失效。
    bool explosive_projectile{false};  ///< Frozen native bazooka/PVE poison
                                       ///< projectile; may send 32900.
    std::uint32_t area_type{
        0U};  ///< 冻结武器/道具来源及区域能力；爆风继承投射物来源。
    std::optional<NativePiercing>
        native_piercing;  ///< 仅已恢复的实际原模板/能力，不由hit请求声明。
  };

  /** @brief 每个暴风对各真人的独立受击/清理状态，绑定创建时生命代次。 */
  struct StormVictim final {
    std::uint64_t life{0U};
    bool enemy{false};
    bool impact{false};
    bool reaction{false};
    bool ended{false};
  };

  /** @brief 原生暴风对象及每个观察端的独立生命周期。 */
  struct StormState final {
    std::uint32_t source{0U};
    std::uint32_t type{0U};
    std::chrono::steady_clock::time_point created{};
    std::chrono::milliseconds flight{};
    std::map<domain::PlayerId, StormVictim> victims;
  };

  [[nodiscard]] CombatRelayResult HandleStorm(
      domain::PlayerId sender, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);

  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleAction(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 药包消耗、原生阶段和终止清单；只处理已认证的本人生存动作。 */
  [[nodiscard]] CombatRelayResult HandleSurvivalTreatment(
      ActorState& actor, const protocol::Packet& packet,
      const protocol::NativeSurvivalTreatment& request,
      std::chrono::steady_clock::time_point now);
  /** @brief 真实560消耗、两根杆的显式对象及原生33279连接。 */
  [[nodiscard]] CombatRelayResult HandleSurvivalTrap(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 原生747取消本人尚未连接的第一根杆，返还一个真实560。 */
  [[nodiscard]] bool CollectSurvivalTrap(
      ActorState& actor, std::uint32_t id,
      std::chrono::steady_clock::time_point now);
  /** @brief 仅删除本服实际创建的杆；隐式绊线对象号不当作伤害cookie。 */
  [[nodiscard]] CombatRelayResult DestroySurvivalTrap(
      ActorState& actor, const protocol::Packet& packet, std::uint32_t id,
      std::chrono::steady_clock::time_point now);
  /** @brief 已删除两杆的一次原生绊线爆炸授权；失败不消耗凭据。 */
  [[nodiscard]] std::optional<std::uint32_t> SurvivalTrapBlast(
      const ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now) const;
  /** @brief 本人近战请求和受害者确认；成功只结算一次直接击爆。 */
  [[nodiscard]] CombatRelayResult HandleSurvivalMelee(
      ActorState& actor, const protocol::Packet& packet,
      const protocol::NativeSurvivalMelee& request,
      std::chrono::steady_clock::time_point now);
  /** @brief 32813 的生存近战结束分支，向本人回送以解除 122 状态。 */
  [[nodiscard]] CombatRelayResult HandleSurvivalMeleeEnd(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 切离治疗栏或生命周期变化时释放当前读条，保留已消费库存和编号墓碑。
   */
  void CancelSurvivalTreatment(ActorState& actor, CombatRelayResult& result,
                               bool clear_effect);
  /** @brief 冻结配装或本场真实拾取的武器才能同步到对应栏。 */
  [[nodiscard]] bool CanEquip(const ActorState& actor, std::uint32_t bar,
                              std::uint32_t weapon) const noexcept;
  /** @brief 验证已消费道具产生的本人属性/恢复报告，范围效果额外校验距离和生命。
   */
  [[nodiscard]] CombatRelayResult HandleItemEffect(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  /** @brief 处理已登记冰冻波/磁铁结束；不识别的无害清除仍由原动作路径处理。 */
  [[nodiscard]] bool EndAreaItem(ActorState& actor,
                                 const protocol::Packet& packet,
                                 std::chrono::steady_clock::time_point now);
  /** @brief 仅暂存已投出原生Ward的238，成功暂存不发送空确认或创建效果。 */
  [[nodiscard]] CombatRelayResult StageAreaCast(
      ActorState& actor, const protocol::Packet& packet,
      const protocol::NativeAreaCast& request,
      std::chrono::steady_clock::time_point now);
  /** @brief 合法引爆提交时按来源、位置及期限插入真实32930，再发送32900。 */
  void CommitAreaCast(ActorState& actor, const protocol::Packet& detonation,
                      std::uint32_t source,
                      std::chrono::steady_clock::time_point now,
                      CombatRelayResult& result);
  [[nodiscard]] application::ServiceResult<CombatRelayResult>
  HandleBubbleAction(ActorState& actor,
                     const protocol::NativeBubbleAction& request,
                     const protocol::Packet& packet,
                     std::chrono::steady_clock::time_point now);
  [[nodiscard]] CombatRelayResult HandleHumiliation(
      ActorState& actor, std::uint32_t target, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleHit(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<CombatRelayResult>
  HandleWeaponObject(ActorState& actor, const protocol::Packet& packet,
                     std::chrono::steady_clock::time_point now);
  /** @brief 581010 的本人水伤，不要求背包装备环境伤害模板5911。 */
  [[nodiscard]] CombatRelayResult HandleSurvivalWater(
      ActorState& actor, const protocol::Packet& packet,
      const protocol::NativeWeaponObjectRequest& request,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleLifecycle(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleRespawn(
      ActorState& actor, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  void BeginHotbloodRespawn(ActorState& actor,
                            std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool ConsumeActionQuota(
      ActorState& actor, std::chrono::steady_clock::time_point now) noexcept;
  void PruneProjectiles(std::chrono::steady_clock::time_point now);
  /** @brief
   * GoDamager+888统一登记到客户端Manager+984；包括各类对象的未过期墓碑。 */
  [[nodiscard]] bool ContainsDamagerCookie(std::uint32_t cookie) const noexcept;
  [[nodiscard]] CombatRelayResult AcceptedResult(
      domain::PlayerId sender, const protocol::Packet& packet) const;
  [[nodiscard]] static bool IsMovementOpcode(std::uint16_t opcode) noexcept;
  [[nodiscard]] static CombatRelayResult RejectedResult(
      CombatRejectReason reason);
  /** @brief
   * 记录已接受出站和17/21本机低counter构造；拒绝及高counter预测均不计数。 */
  void RecordNativeConstructors(CombatRelayResult& result,
                                const ActorState& source,
                                const protocol::Packet& packet,
                                std::uint32_t count);
  /** @brief 道具消费凭据通过后，按房间实际32894接收者记录mode3飞机/保护罩构造。
   */
  void RecordItemEffectConstructors(CombatRelayResult& result,
                                    domain::PlayerId source,
                                    const protocol::Packet& packet);
  /** @brief 已消费的136效果经32894确认后，才开放原飞机产生的十枚炸弹额度。 */
  void ActivateAirstrike(domain::PlayerId player,
                         std::chrono::steady_clock::time_point now);
  /** @brief 17/21构造拒绝使本机顺序不确定；不放宽拒绝，也不产生构造事实。 */
  [[nodiscard]] application::ServiceResult<CombatRelayResult>
  FinishNativeConstructorAttempt(
      domain::PlayerId source, const protocol::Packet& packet,
      application::ServiceResult<CombatRelayResult> result);
  /** @brief
   * 原mode17/21本机可能先入队的构造异常同样冻结gap；只改变内部同步诊断。 */
  void MarkNativeConstructorGap(domain::PlayerId source, std::uint16_t opcode);
  /** @brief 原本机队列可能消费低计数器的消息，供房间阶段门禁共享同一分类。 */
  [[nodiscard]] static bool IsNativeConstructorOpcode(
      std::uint16_t opcode) noexcept;
  /** @brief 执行同行特效校验；外层统一记录拒绝与协议异常引起的本机构造缺口。 */
  [[nodiscard]] CombatRelayResult RelayPeerEffectChecked(
      domain::PlayerId sender, const protocol::Packet& packet,
      std::chrono::steady_clock::time_point now);
  void RecordLifecycleStatistics(
      const ActorState& victim,
      const protocol::NativeLifecycleRequest& request) noexcept;

  std::map<domain::PlayerId, ActorState> actors_;
  std::set<domain::PlayerId>
      observers_;  ///< 仅接收比赛广播，不注册伤害对象或奖励统计。
  std::map<domain::PlayerId, OutboundMessage> pending_movements_;
  bool movement_tick_aggregation_enabled_{false};
  std::optional<domain::PlayerId>
      host_;  ///< 当前房主；预测动作绝不代替目标成员真实输入。
  bool individual_scoring_{
      false};                  ///< 个人模式中其他玩家均为对手，与名义队伍无关。
  std::uint32_t mode_id_{0U};  ///< 房间冻结模式；生存专属动作仅允许 23/25。
  bool grenades_forbidden_{
      false};  ///< 房间规则只限制真人手雷，不限制受控 NPC 攻击。
  std::set<std::uint32_t> object_ids_;
  std::map<std::uint32_t, domain::PlayerId>
      object_owners_;  ///< 对象到冻结玩家的只读反向索引。
  std::map<std::pair<domain::PlayerId, std::uint32_t>, std::uint64_t>
      native_constructor_ordinals_;  ///< 接收者/owner二元键；不混用cookie、slot或高counter。
  std::set<std::uint32_t>
      native_loopback_gaps_;  ///< 原17/21被拒创建不能借自报counter修复。
  std::map<std::uint32_t, PendingRescue>
      rescues_;  ///< 以泡封者对象为键的营救握手。
  std::map<std::uint32_t, ProjectileState> projectiles_;
  std::map<std::uint32_t, ProjectileState>
      weapon_objects_;  ///< 近战/爆炸对象及销毁墓碑。

  /** @brief 本服显式创建的诡雷杆；保留消耗、配对、删除及爆炸墓碑。 */
  struct SurvivalTrapRod final {
    domain::PlayerId owner;
    std::uint64_t life{0U};
    std::array<float, 3U> position{};
    std::uint32_t stage{0U};
    std::uint32_t peer{0U};
    bool linked{false};
    bool removed{false};
    bool exploded{false};
    std::optional<std::chrono::steady_clock::time_point> removed_at;
  };

  std::map<std::uint32_t, SurvivalTrapRod>
      survival_traps_;  ///< 整局保留墓碑及安装去重依据。
  std::map<std::uint32_t, StormState>
      storms_;  ///< GoDamager +888 编号，与武器伤害对象共享命名空间。
  std::map<std::uint32_t, std::uint32_t>
      storm_serials_;  ///< 原生角色号段共享计数，NPC 退场后仍防旧包。
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_COMBAT_RELAY_H_
