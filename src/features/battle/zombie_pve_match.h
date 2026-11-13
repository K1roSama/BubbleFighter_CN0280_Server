#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MATCH_H_

/** @file zombie_pve_match.h @brief 原版 Boss
 * 地图角色登记、特殊复活和房主脚本终局。 */
#include <deque>
#include <string>

#include "features/battle/combat_relay.h"

namespace bf::runtime {
/** @brief 模式 16 的客户端脚本协作账本；碰撞和 HP 仍由原版客户端计算。 */
class ZombiePveMatch final {
 public:
  /** @brief 保存已解析地图；对象号须在加载完成后由 install 固定。 */
  explicit ZombiePveMatch(std::string map);
  /** @brief 固定房主和 +4 独立 NPC 对象号段，拒绝与真人对象号段重叠。 */
  void Install(domain::PlayerId host, CombatRelay& combat);
  /** @brief 离场前移交已有 NPC 控制权，清理追踪目标并保留幸存者状态。 */
  [[nodiscard]] std::vector<OutboundMessage> MemberLeft(
      domain::PlayerId departed, domain::PlayerId next_host,
      CombatRelay& combat);
  /** @brief 读取地图白名单后接纳 32890..32893；调用者须安装账本后重放开局消息。
   */
  [[nodiscard]] bool MapMessage(const protocol::Packet& packet,
                                CombatRelay* combat);
  /** @brief 只筛选房主 +4 号段的开局候选动作；暂存不授予 NPC 或武器身份。 */
  [[nodiscard]] static bool PrestartNpcAction(const protocol::Packet& packet,
                                              std::uint16_t host_seat);
  /** @brief 按生成记录绑定 NPC，并处理转换前在途武器消息。 */
  [[nodiscard]] bool PrepareAction(domain::PlayerId sender,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat);
  /** @brief 有界暂存房主在首次槽位定位前发送的 NPC 动作，不猜测生成顺序。 */
  [[nodiscard]] bool DeferAction(
      domain::PlayerId sender, const protocol::Packet& packet,
      const CombatRelay& combat,
      std::chrono::steady_clock::time_point now = {});
  /** @brief 明确槽位绑定后取出该对象的动作，调用者仍须逐包通过正常战斗校验。 */
  [[nodiscard]] std::vector<protocol::Packet> TakeDeferredActions(
      std::uint32_t object, std::chrono::steady_clock::time_point now = {});
  /** @brief Authenticate pursuit, drop/pickup/effects and diagnostic score
   * reports. */
  [[nodiscard]] CombatRelayResult HandleEvent(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const;
  /** @brief S2C932 maps this server's five recovered drop types to native skill
   * slots. */
  [[nodiscard]] static OutboundMessage SkillSlots();
  /** @brief S2C926 must precede 510: native map loading consumes this task
   * table. */
  [[nodiscard]] OutboundMessage RescueTasks() const;
  /** @brief 死亡完成时按生命代次扣减 LP，发出一次 730 并固定复活分支。 */
  [[nodiscard]] std::vector<OutboundMessage> Tick(
      CombatRelay& combat, std::chrono::steady_clock::time_point now = {});
  /** @brief 消费死亡授权，将耗尽 LP 的玩家转换为阵营 35。 */
  [[nodiscard]] std::vector<OutboundMessage> Transition(domain::PlayerId player,
                                                        CombatRelay& combat);
  /** @brief 保存当前房主的终局通知，由 tick 对照本次 Boss/守卫状态确认。 */
  void Claim(domain::PlayerId sender, const protocol::Packet& packet);
  /** @brief 规范胜方 1 僵尸/2 人类；超时未通关为失败。 */
  [[nodiscard]] std::uint8_t Winner(bool timeout = false) const noexcept;
  /** @brief 当前显示阵营；数据库参与者仍保存开局人类队伍。 */
  [[nodiscard]] std::uint8_t Team(domain::PlayerId player) const noexcept;
  /** @brief 分类只取合法登记的NPC和已确认击败，925不能指定或增加类别。 */
  void ObserveScoreUnits(const CombatRelay& combat);
  [[nodiscard]] std::array<std::uint32_t, 16U> ScoreUnits(
      domain::PlayerId player) const noexcept;

 private:
  /** @brief 与地图 CHR_PROPERTY/生成器绑定的角色，槽位可在移除后复用。 */
  struct Agent final {
    std::uint64_t generation{0U};  ///< 合法生成递增；未绑定动作不能跨槽位重用。
    bool active{false};
    std::uint8_t team{0U};
    std::optional<std::uint32_t> object;
    std::vector<std::uint8_t> spawn;
    std::uint32_t pursuit_target{0xffffffffU};
    std::uint32_t spawner{0U};
    std::uint32_t default_weapon{0U};
    bool boss{false};
    bool protected_object{false};
    std::optional<std::uint8_t> score_unit;
    std::optional<std::uint64_t> classified_life;
    std::optional<float> reported_hp;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> boss_phase;
    std::chrono::steady_clock::time_point boss_phase_time{};
    std::optional<std::chrono::steady_clock::time_point> teleport_due;
    bool boss_final_sequence{false};
  };

  [[nodiscard]] bool Bind(std::uint32_t slot, std::uint32_t object,
                          CombatRelay& combat);
  [[nodiscard]] bool RememberUnbound(std::uint32_t object);
  [[nodiscard]] bool IsNpcObject(std::uint32_t object) const;
  void ObserveTerminalState(const CombatRelay& combat);
  /** @brief 单组营救任务的调度与结算状态。 */
  enum class RescueState { kWaiting, kActive, kSucceeded, kFailed };

  /** @brief 记录营救组的市民身份、敌人数及已确认死亡。 */
  struct Rescue final {
    std::uint32_t citizen{0U};
    std::uint32_t enemies{0U};
    std::uint32_t spawned{0U};
    bool citizen_spawned{false};
    bool failure_spawned{false};
    RescueState state{RescueState::kWaiting};
    std::map<std::uint32_t, bool> defeated;
    std::optional<std::uint32_t> citizen_object;
    bool citizen_dead{false};
  };

  void ObserveRescueDeaths(const CombatRelay& combat);
  std::vector<Rescue> rescues_;
  std::optional<std::chrono::steady_clock::time_point> rescue_started_;
  std::uint32_t rescue_index_{0U};
  std::string map_;
  domain::PlayerId host_;
  std::set<std::uint32_t> npc_bases_;
  std::array<Agent, 64U> agents_{};
  std::deque<std::uint32_t> pending_;
  std::set<std::uint32_t> unbound_objects_;

  /** @brief 尚未定位的动作只保留原房主及当时合法生成集合，不由自报对象猜槽位。
   */
  struct DeferredActions final {
    domain::PlayerId controller;
    std::chrono::steady_clock::time_point received_at;
    std::chrono::steady_clock::time_point expires_at;
    std::map<std::uint32_t, std::uint64_t> authorized_spawns;
    std::vector<protocol::Packet> packets;
  };

  void PruneDeferredActions(std::chrono::steady_clock::time_point now);
  std::map<std::uint32_t, DeferredActions> deferred_actions_;
  std::set<std::uint32_t> retired_;
  std::set<domain::PlayerId> infected_;

  /** @brief 已认证掉落物的身份、位置、期限和原始请求。 */
  struct Drop final {
    std::uint32_t object{0U};
    std::uint32_t type{0U};
    std::uint8_t team{0U};
    std::array<float, 3U> position{};
    std::chrono::steady_clock::time_point expires{};
    std::vector<std::uint8_t> request;
  };

  std::vector<Drop> drops_;

  struct InfectedDrops final {
    std::uint64_t life{0U};
    std::set<std::vector<std::uint8_t>> requests;
  };

  std::map<domain::PlayerId, InfectedDrops> infected_drops_;
  std::uint32_t drop_serial_{61'000'000U};
  std::chrono::steady_clock::time_point drop_window_{};
  unsigned drops_in_window_{0U};
  std::map<domain::PlayerId, std::map<std::uint32_t, std::uint32_t>> items_;

  /** @brief 单次技能使用的玩家、生命代次和有效期限。 */
  struct SkillUse final {
    domain::PlayerId player;
    std::uint32_t type{0U};
    std::uint64_t life{0U};
    std::chrono::steady_clock::time_point expires{};
  };

  std::vector<SkillUse> skill_uses_;
  std::map<domain::PlayerId,
           std::array<std::chrono::steady_clock::time_point, 5U>>
      skill_due_;
  // Client reports are observations, never unchecked reward/score increments.
  std::map<domain::PlayerId, std::array<std::uint32_t, 16U>> confirmed_units_;
  std::map<domain::PlayerId, std::array<std::uint32_t, 16U>> reported_units_;
  std::map<domain::PlayerId, std::uint32_t> reported_combo_;
  /** @brief
   * FighterConfigMode[20].respawnPoint=3；每次死亡完成扣一，不随普通复活重置。
   */
  std::map<domain::PlayerId, std::uint8_t> remaining_lives_;
  std::map<domain::PlayerId, std::uint64_t> authorized_;
  std::map<domain::PlayerId, std::array<std::optional<std::uint32_t>, 5U>>
      old_weapons_;
  bool boss_spawned_{false};
  bool protected_spawned_{false};
  std::optional<float>
      protected_gauge_;  ///< 32896 受击更新的共享 FLOAT，不是对象号。
  bool boss_claim_{false};
  bool success_claim_{false};
  bool failure_claim_{false};
  bool boss_defeated_{false};
  bool protected_defeated_{false};
  bool boss_final_{false};
  bool boss_zero_hp_{false};
  std::optional<std::chrono::steady_clock::time_point> boss_finish_due_;
  std::optional<std::chrono::steady_clock::time_point> guard_failure_due_;
  std::uint8_t winner_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ZOMBIE_PVE_MATCH_H_
