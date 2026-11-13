#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_WEAPON_MASTER_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_WEAPON_MASTER_MATCH_H_

/**
 * @file weapon_master_match.h
 * @brief 武器大师局内经验、临时换枪和按生命去重的助攻规则。
 */

#include <map>
#include <random>
#include <set>

#include "features/battle/combat_relay.h"
#include "protocol/battle/weapon_master_messages.h"

namespace bf::runtime {

/** @brief
 * 本服可配置玩法；客户端未包含原服经验表或随机武器权重，不宣称为原服数值。 */
struct WeaponMasterRules final {
  std::uint32_t final_level{10U};  ///< 到达此局内等级结束比赛，范围 2～100。
  std::uint32_t level_step{100U};  ///< 每级累计经验增量，范围 1～10000。
  std::uint32_t pang_experience{
      100U};  ///< 一次确认击爆所得局内经验，不是账号奖励。
  std::uint32_t assist_experience{50U};  ///< 一次确认原生助攻所得局内经验。
  std::vector<std::uint32_t> primary_pool{2004U, 3103U,
                                          4004U};  ///< 等权主武器模板池。
  /** @brief 按完整冻结规则比较。 */
  bool operator==(const WeaponMasterRules&) const = default;
};

/** @brief 一次真实状态变化的原生广播；拒绝请求不产生伪成功报文。 */
struct WeaponMasterUpdate final {
  std::vector<OutboundMessage>
      messages;               ///< 完整 S2C1008；成绩由房间统一编码。
  bool score_changed{false};  ///< 局内经验或助攻次数实际变化。
  bool finished{false};       ///< 至少一名在场玩家到达最终等级。
  std::optional<CombatRejectReason> rejection;  ///< 幂等、安全拒绝原因。
};

/**
 * @brief 与 CombatRelay 共用已认证成员和生命周期的模式 26 状态机。
 * @note 原生 2026/39 不携带伤害值；本类验证本生命真实命中和确认终结者，伤害比例
 *       仍由原生受害端报告。不得把命中数量当作伤害比例，亦不得以该包独立授权击爆。
 */
class WeaponMasterMatch final {
 public:
  /** @brief 校验规则并冻结本场随机源；种子由外层生成，测试可显式给定。 */
  explicit WeaponMasterMatch(WeaponMasterRules rules, std::uint32_t seed);
  /** @brief 首次正式开局登记玩家和临时武器；重复调用无副作用。 */
  [[nodiscard]] WeaponMasterUpdate Start(CombatRelay& combat);
  /** @brief 只在 CombatRelay 已接受该包后观察真实命中、死亡与复活。 */
  [[nodiscard]] WeaponMasterUpdate Observe(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 处理原生助攻/连续未得分换枪；无效和重复请求不改经验、武器或次数。
   */
  [[nodiscard]] WeaponMasterUpdate Request(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 返回含助攻次数的真实局内进度；只接受本局冻结玩家。 */
  [[nodiscard]] protocol::WeaponMasterProgress Progress(
      domain::PlayerId player) const;
  /** @brief 接收过服务器换枪前的旧模板只可安全忽略，不能重新装备。 */
  [[nodiscard]] bool StaleSwitch(domain::PlayerId sender,
                                 const protocol::Packet& packet,
                                 const CombatRelay& combat) const;

  /** @brief
   * 最终等级已到达时，等待已确认死亡的原生迟到助攻收据，不再开始新战斗。 */
  [[nodiscard]] bool Finishing() const noexcept { return target_reached_; }

  /** @brief 有界助攻窗口的截止时刻，供房间计时器调度。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const noexcept {
    return finish_deadline_;
  }

  /** @brief 无后续客户端报文时也能在窗口结束后完成终局。 */
  [[nodiscard]] WeaponMasterUpdate Tick(
      const CombatRelay& combat, std::chrono::steady_clock::time_point now);

 private:
  using Time = std::chrono::steady_clock::time_point;

  /** @brief 同一次真实生命的助攻候选；只在对应死亡确认后发放一次。 */
  struct Assist final {
    protocol::WeaponMasterAssistRequest request;
    Time received;
  };

  /** @brief 整局保留玩家进度，离场不会抹去结算事实。 */
  struct Player final {
    std::uint32_t experience{0U};
    std::uint32_t assists{0U};
    std::uint32_t no_pang_respawns{0U};
    std::optional<std::uint64_t> settled_death;
    std::optional<std::uint64_t> completed_respawn;
    std::optional<std::uint64_t> rerolled_life;
    std::set<std::uint32_t> sources;
    std::set<std::uint32_t> former_primaries;
    std::optional<Assist> pending;
    bool assist_claimed{false};
  };

  [[nodiscard]] OutboundMessage Change(
      domain::PlayerId player, CombatRelay& combat,
      protocol::WeaponMasterChangeReason reason);
  void Award(domain::PlayerId player, std::uint32_t amount, CombatRelay& combat,
             WeaponMasterUpdate& update);
  [[nodiscard]] bool ValidAssist(
      domain::PlayerId victim,
      const protocol::WeaponMasterAssistRequest& request,
      const CombatRelay& combat) const;
  void SettleAssist(domain::PlayerId victim, CombatRelay& combat, Time now,
                    WeaponMasterUpdate& update);
  void FinishIfReady(const CombatRelay& combat, Time now,
                     WeaponMasterUpdate& update);
  WeaponMasterRules rules_;
  std::mt19937 random_;
  std::map<domain::PlayerId, Player> players_;
  bool started_{false};
  bool target_reached_{false};
  bool finished_{false};
  std::optional<Time> finish_deadline_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_WEAPON_MASTER_MATCH_H_
