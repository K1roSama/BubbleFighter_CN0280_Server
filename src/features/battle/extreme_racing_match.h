#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MATCH_H_

/**
 * @file extreme_racing_match.h
 * @brief 定义极限竞速五赛段、认证位置检查、每玩家金币去重和确定性成绩状态机。
 */

#include <deque>
#include <map>

#include "features/battle/combat_relay.h"
#include "features/battle/extreme_racing_maps.h"
#include "features/battle/extreme_racing_scene.h"
#include "protocol/battle/extreme_racing_messages.h"

namespace bf::runtime {
/** @brief 本服明确选择的时间与计分策略，不能视为已恢复的原服数值。 */
struct ExtremeRacingRules final {
  std::chrono::milliseconds countdown{2500};
  std::chrono::milliseconds stage_duration{210000};
  std::chrono::milliseconds retire_duration{20000};
  std::chrono::milliseconds result_duration{5000};
  std::chrono::milliseconds position_max_age{2000};
  float region_tolerance{20.0F};
  float coin_radius{60.0F};
  bool require_last_checkpoint{true};
  std::chrono::milliseconds cannon_authorization{
      3000};                      ///< 770 发出后房主上报 32902 的有效期。
  float cannon_tolerance{30.0F};  ///< 32902 炮台坐标与原图标记的最大偏差。
  std::array<std::int32_t, 16U> finish_points{100, 80, 60, 40, 30, 20, 10, 5,
                                              4,   3,  2,  1,  1,  1,  1,  1};
};
/** @brief 状态机阶段；展示结果期间不能接受新成绩。 */
enum class ExtremeRacingPhase : std::uint8_t {
  kWaiting,
  kCountdown,
  kRunning,
  kRetiring,
  kRoundResult,
  kFinished,
};
/** @brief 业务拒绝原因；不生成空成功包，不清除已确认成绩。 */
enum class ExtremeRacingReject : std::uint8_t {
  kUnknownParticipant,
  kInactivePhase,
  kWrongStage,
  kDuplicate,
  kUnknownObject,
  kUnknownCheckpoint,
  kCheckpointTooEarly,
  kMissingCheckpoint,
  kInvalidLife,
  kMissingFreshPosition,
  kOutsideRegion,
  kClockRegression,
};
/** @brief 完成原因；无人留场不应被上层误当作五赛段正常完成。 */
enum class ExtremeRacingCompletion : std::uint8_t {
  kFiveStages,
  kNoParticipants
};

/** @brief 一次操作的真实状态输出；消息均为已恢复的 761/764 格式。 */
struct ExtremeRacingUpdate final {
  std::vector<OutboundMessage> messages;
  std::vector<OutboundMessage>
      host_messages;  ///< 只发给房主：S2C770 由房主本地开炮并上报 32902。
  std::optional<std::uint32_t>
      stage_changed;  ///< 上层应按顺序完成原生赛段切换和复位。
  std::optional<ExtremeRacingCompletion> completion;  ///< 只在首次完成时产生。
  std::optional<ExtremeRacingReject> rejection;
  bool accepted{false};  ///< 该客户端请求是否实际提交；定时事件不属于请求。
  bool score_changed{false};
};

/** @brief 冻结成员的完整成绩；即使离场，也保留本场已获得的分数及原席位。 */
struct ExtremeRacingProgress final {
  std::uint16_t seat{0U};
  std::int32_t finish_points{0};
  std::int32_t coin_points{0};
  std::int32_t round_finish_points{0};
  std::int32_t round_coin_points{0};
  std::int32_t tie_break{0};  ///< 本服使用各赛段用时及未完成惩罚的毫秒和。
  std::uint16_t round_place{0U};
  std::optional<std::uint32_t> checkpoint;
  bool departed{false};  ///< 本服用原生标签 4 将离场者排后。
  /** @brief 比较不可变快照，供一次消费与结算校验。 */
  bool operator==(const ExtremeRacingProgress&) const = default;
};

/**
 * @brief 单场 executor 独占的竞速核心，与 CombatRelay 共用冻结身份及当前生命。
 * @note 绑定的 CombatRelay
 * 必须比本对象活得更久；房间层负责授权消息分发与一次持久化终局。
 *       地板/消失路线/投石发射为客户端本地控制，位置仍不是服务器物理模拟或反作弊证明。
 */
class ExtremeRacingMatch final {
 public:
  /** @brief
   * 校验固定地图、本服规则及外部已恢复的金币配置；空金币仅供分离验证核心。 */
  explicit ExtremeRacingMatch(std::string_view map,
                              std::vector<ExtremeRacingCoinPlacement> coins,
                              ExtremeRacingRules rules = {});
  /** @brief
   * 使用原图场景，首次启动发送对象和危险物初始化，每关发送实际赛段切换。 */
  explicit ExtremeRacingMatch(const ExtremeRacingScene& scene,
                              ExtremeRacingRules rules = {});
  ExtremeRacingMatch(const ExtremeRacingMatch&) = delete;
  ExtremeRacingMatch& operator=(const ExtremeRacingMatch&) = delete;
  /** @brief 从模式 24 的真实成员冻结参赛名单；重复开局不重置分数。 */
  [[nodiscard]] ExtremeRacingUpdate Start(
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 只在同一包已获 CombatRelay
   * 接受后调用，保存本赛段、本生命的最近移动证据。 */
  void Observe(domain::PlayerId sender, const protocol::Packet& packet,
               const CombatRelay& combat,
               std::chrono::steady_clock::time_point now);
  /** @brief 处理
   * 754/755/32934；严格解析、先推进计时，再验证身份、生命、赛段和位置。 */
  [[nodiscard]] ExtremeRacingUpdate Request(
      domain::PlayerId sender, const protocol::Packet& packet,
      const CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief
   * 无客户端报文时也能完成超时、结果展示和五赛段终局；延迟调度不跳过新赛段。 */
  [[nodiscard]] ExtremeRacingUpdate Tick(
      std::chrono::steady_clock::time_point now);
  /**
   * @brief 房主按 770 本地开炮后的 C2S32902：须匹配未消费授权、炮台坐标与方向；
   *        通过后以房主对象登记弹丸（命中由房主上报）并转发给其他成员。
   */
  [[nodiscard]] CombatRelayResult CannonProjectile(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /**
   * @brief 原图2赛段3的本机投石碰撞32772兼容；不证明服务器物理碰撞。
   * @note
   * 仅真实参赛者近期位置、生命和固定原生模板/向量；damager只回本人且只能命中本人。
   */
  [[nodiscard]] CombatRelayResult StoneBlast(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 离场不可再次参赛；重复离场无效，最后一人离场只发出一次终止事件。 */
  [[nodiscard]] ExtremeRacingUpdate Leave(
      domain::PlayerId player, std::chrono::steady_clock::time_point now);
  /** @brief 返回包含离场者的冻结成绩。 */
  [[nodiscard]] ExtremeRacingProgress Progress(domain::PlayerId player) const;
  /** @brief 返回冻结原生出生索引；当前赛段再乘 100 后构成原图标记编号。 */
  [[nodiscard]] std::uint32_t SpawnPoint(domain::PlayerId player) const;
  /** @brief 按原生字段比较顺序排列：离场标志、总分、同分值、原席位。 */
  [[nodiscard]] std::vector<domain::PlayerId> Standings() const;

  /** @brief 当前状态机阶段。 */
  [[nodiscard]] ExtremeRacingPhase Phase() const noexcept { return phase_; }

  /** @brief 当前原生赛段；正式开始前为零，之后为 1～5。 */
  [[nodiscard]] std::uint32_t Stage() const noexcept { return stage_; }

  /** @brief 下一次必要的房间计时调度时刻。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const noexcept {
    auto result = deadline_;
    if (RacingPhase())
      for (const auto& [object, due] : next_fire_) {
        static_cast<void>(object);
        if (!result || due < *result) result = due;
      }
    return result;
  }

 private:
  using Time = std::chrono::steady_clock::time_point;

  /** @brief 一次真实移动及其生命代次，跨赛段或新生命不能重复使用。 */
  struct Position final {
    std::array<float, 3U> value;
    Time received;
    std::uint64_t life;
  };

  /** @brief 整场玩家账本；金币消费不随复活或赛段切换清空。 */
  struct Player final {
    std::uint32_t object;
    std::uint32_t spawn_point{
        0U};  ///< 冻结的原生同队席位序号，跨关不因离场重新排序。
    ExtremeRacingProgress score;
    std::optional<Position> position;
    std::set<std::uint32_t> coins;
    bool round_settled{false};
  };

  /** @brief 一次 770 发射授权，绑定发出时房主和时间；换房主不能继承未消费授权。
   */
  struct PendingShot final {
    std::uint32_t cannon;
    std::uint32_t weapon;
    domain::PlayerId controller;
    Time issued;
    Time expires;
  };

  [[nodiscard]] bool RacingPhase() const noexcept {
    return phase_ == ExtremeRacingPhase::kRunning ||
           phase_ == ExtremeRacingPhase::kRetiring;
  }

  void ArmCannons(Time now);
  void FireCannons(Time now, ExtremeRacingUpdate& update);
  void Countdown(Time now, ExtremeRacingUpdate& update);
  void CloseRound(Time now, ExtremeRacingUpdate& update);
  void Complete(ExtremeRacingCompletion kind, ExtremeRacingUpdate& update);
  void AppendTimer(ExtremeRacingUpdate& update,
                   protocol::ExtremeRacingTimer kind,
                   std::chrono::milliseconds duration, bool active) const;
  void Records(ExtremeRacingUpdate& update,
               protocol::ExtremeRacingUpdateKind kind) const;
  void SynchronizeCombat();
  [[nodiscard]] bool AllFinished() const;
  [[nodiscard]] std::optional<ExtremeRacingReject> ValidPosition(
      const Player& player, domain::PlayerId sender, const CombatRelay& combat,
      Time now) const;
  const ExtremeRacingMap* map_;
  std::optional<ExtremeRacingScene> scene_;
  CombatRelay* combat_{
      nullptr};  ///< 开局绑定；阶段推进同时撤销旧生命与旧复位授权。
  ExtremeRacingRules rules_;
  std::map<std::uint32_t, ExtremeRacingCoinPlacement> coins_;
  std::map<domain::PlayerId, Player> players_;
  ExtremeRacingPhase phase_{ExtremeRacingPhase::kWaiting};
  std::uint32_t stage_{0U};
  std::uint16_t finishers_{0U};
  Time running_since_{};
  std::optional<Time> last_time_;
  std::optional<Time> deadline_;
  std::map<std::uint32_t, Time>
      next_fire_;  ///< 当前赛段炮台对象 -> 下一次发射时刻。
  std::deque<PendingShot> pending_shots_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_MATCH_H_
