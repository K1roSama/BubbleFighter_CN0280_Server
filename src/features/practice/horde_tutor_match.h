#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_MATCH_H_

/**
 * @file horde_tutor_match.h
 * @brief 独立103单人波次账本；仅复用已证海盗wire，不复用任意房主脚本授权。
 */

#include <deque>

#include "features/battle/combat_relay.h"
#include "features/practice/horde_tutor_profile.h"

namespace bf::runtime {

/** @brief 仅rel_prologue_01第一波的19次构造、15个敌人和4个同队NPC。 */
class HordeTutorMatch final {
 public:
  HordeTutorMatch(domain::PlayerId player, std::uint32_t object);
  /** @brief wire格式与原HordeWaveMgr相同；状态授权是独立的103规则。 */
  [[nodiscard]] static bool Accepts(std::uint16_t opcode) noexcept;
  static void Validate(const protocol::Packet& packet);
  [[nodiscard]] CombatRelayResult Handle(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 只有已核准newSpawn的一次初始装配报告才允许绑定NPC对象。 */
  [[nodiscard]] bool PrepareAction(domain::PlayerId sender,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat);
  [[nodiscard]] std::optional<OutboundMessage> Advance(
      const CombatRelay& combat, std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline(
      const CombatRelay& combat) const;
  [[nodiscard]] std::uint8_t Winner() const noexcept;

 private:
  using Time = std::chrono::steady_clock::time_point;
  [[nodiscard]] bool ClearedActors(const CombatRelay& combat) const;
  [[nodiscard]] bool EnemiesTerminal(const CombatRelay& combat) const;
  domain::PlayerId player_;
  std::uint32_t player_object_;
  bool begun_{false};
  bool clear_requested_{false};
  bool cleared_{false};
  Time started_{};
  std::optional<Time> clear_ready_at_;
  std::optional<Time> clear_ack_at_;
  std::vector<std::uint8_t> plan_;
  std::set<std::uint8_t> spawned_;
  std::deque<std::uint8_t> pending_;
  std::map<std::uint32_t, std::uint8_t> actors_;
  std::uint8_t winner_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_HORDE_TUTOR_MATCH_H_
