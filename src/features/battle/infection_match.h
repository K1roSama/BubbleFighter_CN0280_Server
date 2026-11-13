#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_INFECTION_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_INFECTION_MATCH_H_

/** @file infection_match.h @brief
 * 维护原生僵尸母体、感染授权与按阵营结束的局内状态。 */
#include "features/battle/combat_relay.h"

namespace bf::runtime {
/** @brief 模式 8/28 的动态阵营账本；数据库开局身份保持冻结。 */
class InfectionMatch final {
 public:
  /** @brief 指定候选母体及开场等待截止；在持久化开局和截止之后转换。 */
  InfectionMatch(std::uint32_t mode, domain::PlayerId mother,
                 std::chrono::steady_clock::time_point ready_at = {});
  /** @brief 处理已确认死亡、授权复活和全员感染，返回全房原生通知。 */
  [[nodiscard]] std::vector<OutboundMessage> Tick(
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 消费本生命计时授权；有认证僵尸伤害的子体允许报告本地计时结束。 */
  [[nodiscard]] std::vector<OutboundMessage> Transition(
      domain::PlayerId player, CombatRelay& combat,
      std::chrono::steady_clock::time_point now = {});
  /** @brief
   * 核对本人401的32905初始时长，按生命消费一次；报告不能改变服务端截止时间。 */
  [[nodiscard]] CombatRelayResult ReportDuration(domain::PlayerId player,
                                                 const protocol::Packet& packet,
                                                 const CombatRelay& combat);
  /** @brief 返回最早尚未发出授权的复活时刻。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const;
  /** @brief 1 人类胜，2 僵尸胜，0 仍在进行；时间到按存活人类判定。 */
  [[nodiscard]] std::uint8_t Winner(bool timeout = false) const noexcept;
  /** @brief 返回终局规范阵营 1/2，供奖励判定而不改写开局快照。 */
  [[nodiscard]] std::uint8_t Team(domain::PlayerId player) const noexcept;
  /** @brief 核对变身后专用武器，丢弃转换前在途切枪并授权母体两种原生近战。 */
  [[nodiscard]] bool PrepareAction(domain::PlayerId player,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat);
  /** @brief 原生32772母体变身/命中爆风，登记与33282/32907共用的伤害对象。 */
  [[nodiscard]] application::ServiceResult<CombatRelayResult> HandleBlast(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 仅观察战斗账本已接受的近战创建和命中，建立单次后续爆风凭据。 */
  void Observe(domain::PlayerId player, const protocol::Packet& packet,
               const CombatRelay& combat,
               std::chrono::steady_clock::time_point now);
  /** @brief 感染模式母体原生暴走请求、属性与清理；蓄力由原客户端计算。 */
  [[nodiscard]] CombatRelayResult HandleFever(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  [[nodiscard]] static bool AcceptsFever(const protocol::Packet& packet);

 private:
  using Time = std::chrono::steady_clock::time_point;

  /** @brief 当前死亡代次对应的唯一复活授权。 */
  struct Respawn final {
    std::uint64_t generation;
    Time due;
    bool authorized{false};
    std::uint64_t last_humiliation{0U};
    bool duration_reported{false};
    bool native_damage_seen{false};
  };

  [[nodiscard]] OutboundMessage Convert(domain::PlayerId player,
                                        CombatRelay& combat);
  std::uint32_t mode_;
  domain::PlayerId mother_;
  Time ready_at_;
  bool started_{false};
  std::uint8_t winner_{0U};
  std::set<domain::PlayerId> infected_;
  std::map<domain::PlayerId, Respawn> respawns_;
  std::map<domain::PlayerId, std::array<std::optional<std::uint32_t>, 5U>>
      previous_weapons_;

  /** @brief 母体一次暴走的授权、原生回执和到期清理。 */
  struct Fever final {
    Time requested;
    Time until;
    std::uint64_t life;
    std::uint32_t base_weapon;
    bool item_applied{false};
    bool attribute_applied{false};
    bool blast_pending{true};
    bool cleanup_reported{false};
    bool ended{false};
  };

  std::optional<Fever> fever_;
  [[nodiscard]] std::vector<OutboundMessage> UpdateFever(CombatRelay& combat,
                                                         Time now);

  [[nodiscard]] bool FeverActive() const noexcept {
    return fever_ && !fever_->ended;
  }

  /** @brief 母体爪子创建时的生命代次；不能用爆风命中循环生成爆风。 */
  struct Melee final {
    Time created;
    std::uint64_t life;
  };

  /** @brief 一个已认证近战命中对应一个5200爆风；容忍网络传输延迟。 */
  struct BlastPermit final {
    Time created;
    domain::PlayerId target;
    std::uint64_t target_life;
    std::uint64_t mother_life;
    std::uint16_t type;
  };

  std::map<std::uint32_t, Melee> melee_;
  std::vector<BlastPermit> blasts_;
  bool initial_blast_pending_{false};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_INFECTION_MATCH_H_
