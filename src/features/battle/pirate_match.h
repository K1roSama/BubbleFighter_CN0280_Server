#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MATCH_H_

/**
 * @file pirate_match.h
 * @brief 恢复旧服务端的房主脚本波次和海盗对象。
 */
#include <deque>
#include <string>

#include "features/battle/combat_relay.h"

namespace bf::runtime {
/** @brief 一局海盗脚本状态；对象只能由已认证房主和已接受生成计划建立。 */
class PirateMatch final {
 public:
  /** @brief 冻结房主和原生对象范围，禁止中途迁移脚本所有权。 */
  PirateMatch(domain::PlayerId host, std::uint32_t host_object);
  /** @brief 判断波次、脚本和原始开火报文。 */
  [[nodiscard]] static bool Accepts(std::uint16_t opcode) noexcept;
  /** @brief 严格校验格式，阶段外也不接受畸形报文。 */
  static void Validate(const protocol::Packet& packet);
  /** @brief 波次和脚本回复含房主；其他效果只转发给同行。 */
  [[nodiscard]] CombatRelayResult Handle(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 根据首个切枪绑定 NPC；观察端预测动作只校验并丢弃。 */
  [[nodiscard]] bool PrepareAction(domain::PlayerId sender,
                                   const protocol::Packet& packet,
                                   CombatRelay& combat);
  /** @brief 返回确认终局队伍；零表示仍在比赛，1 海盗胜，2 玩家胜。 */
  [[nodiscard]] std::uint8_t Winner() const noexcept;
  /** @brief 最近确认生成的脚本波次/组，供地图关联生成器消费。 */
  [[nodiscard]] std::pair<std::uint16_t, std::uint8_t> LastSpawnGroup()
      const noexcept;
  /** @brief 保留原版只发送一次的清场请求，条件齐备时产生一次全房确认。 */
  [[nodiscard]] std::optional<OutboundMessage> Advance(
      const CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 返回清场剩余最短时间；仍有未生成或未死亡海盗时等待后续状态通知。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline(
      const CombatRelay& combat) const;

 private:
  using Time = std::chrono::steady_clock::time_point;
  [[nodiscard]] bool ClearedActors(const CombatRelay& combat) const;

  /** @brief 生成计划允许的组、序号和原生脚本参数。 */
  struct Spawn final {
    std::uint8_t group;
    std::uint8_t index;
    std::u16string settings;
    std::u16string mob_type;
  };

  domain::PlayerId host_;
  std::uint32_t host_object_;
  std::uint32_t wave_{0U};
  std::uint16_t script_wave_{0U};
  std::uint8_t last_group_{0U};
  std::uint32_t cleared_{0U};
  Time started_{};
  std::vector<std::uint8_t> plan_;
  std::vector<Spawn> spawns_;
  std::set<std::pair<std::uint8_t, std::uint8_t>> spawned_;

  /** @brief 尚未绑定对象号的生成计划及脚本能力。 */
  struct PendingSpawn final {
    std::uint32_t wave;
    bool storm;
  };

  std::deque<PendingSpawn> pending_;
  std::map<std::uint32_t, std::uint32_t> actors_;
  std::uint8_t winner_{0U};
  bool clear_requested_{false};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MATCH_H_
