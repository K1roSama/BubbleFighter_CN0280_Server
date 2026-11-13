#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CRAZY_RACING_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CRAZY_RACING_MATCH_H_

/** @file crazy_racing_match.h @brief
 * 原BFRunner跑道终点、检查点及房主报告的独立生命周期。 */
#include <map>
#include <random>
#include <string_view>

#include "features/battle/combat_relay.h"
#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {
/** @brief 当前原始平面网格中check_point编号与精确三轴包围盒。 */
struct CrazyRacingRegion final {
  std::string_view map;
  std::uint16_t index;
  std::uint8_t kind;
  std::array<float, 6> bounds;
};

/** @brief 当前原WB的终点最小/最大标记，不由客户端选择任意坐标。 */
struct CrazyRacingMap final {
  std::string_view key;
  std::array<float, 3> goal_min, goal_max;
  std::array<std::array<float, 3>, 12> spawns;
};

/** @brief 原同队冻结序号的T11..16/T21..26出生点及原WB位置，不用检查点中心代替。
 */
struct CrazyRacingSpawn final {
  std::uint32_t point;
  std::array<float, 3> position;
};

[[nodiscard]] const CrazyRacingMap* GetCrazyRacingMap(
    std::string_view key) noexcept;
/** @brief 固定原资源区域；booster可能存在同编号的多张网格。 */
[[nodiscard]] const CrazyRacingRegion* GetCrazyRacingRegion(
    std::string_view key, std::uint8_t kind, std::uint16_t index) noexcept;
void NormalizeCrazyRacingConfiguration(
    protocol::SceneConfiguration& configuration);

/** @brief 原BFRunner终点/检查点；758只作观察，不授权客户端任意分数或奖励。 */
class CrazyRacingMatch final {
 public:
  using Time = std::chrono::steady_clock::time_point;
  CrazyRacingMatch(std::string_view map,
                   std::vector<CombatParticipant> participants,
                   std::uint32_t random_seed = std::random_device{}());
  static void Validate(const protocol::Packet& packet);
  void Observe(domain::PlayerId player, const protocol::Packet& packet,
               const CombatRelay& combat, Time now);
  /** @brief
   * 仅调用方已经认证的移动或原地图portal完成；不从任意目标报告登记位置。 */
  void ObservePosition(domain::PlayerId player,
                       const std::array<float, 3>& position,
                       const CombatRelay& combat, Time now);
  [[nodiscard]] bool Checkpoint(domain::PlayerId player, std::uint16_t region,
                                const std::array<float, 3>& position,
                                const CombatRelay& combat, Time now);
  [[nodiscard]] bool Handle(domain::PlayerId player, domain::PlayerId host,
                            const protocol::Packet& packet,
                            const CombatRelay& combat, Time now);
  /** @brief
   * 原757检查点/759拾取/708消耗/32894效果链，不授予任意生成或客户端分值。 */
  [[nodiscard]] std::vector<OutboundMessage> Interact(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);
  [[nodiscard]] bool Finished(Time now) const noexcept;
  [[nodiscard]] std::optional<Time> Deadline() const noexcept;
  [[nodiscard]] std::uint8_t Winner() const noexcept;
  [[nodiscard]] std::uint32_t Goals(domain::PlayerId player) const noexcept;
  [[nodiscard]] std::optional<CrazyRacingSpawn> Spawn(
      domain::PlayerId player) const noexcept;

 private:
  /** @brief 开局冻结的真人、队色棒与已认证移动/生命代次。 */
  struct Runner final {
    CombatParticipant participant;
    std::optional<Time> moved;
    std::optional<Time> goal_entered;
    std::uint64_t life{0};
    std::optional<std::uint16_t> checkpoint;
    std::optional<std::uint16_t> box;
    std::uint32_t held{0U};
    std::optional<Time> pickup;
    std::optional<Time> boost;
    Time usable{};
    std::map<std::uint32_t, std::pair<std::uint64_t, Time>> effects;
  };

  /** @brief
   * 房主确认的首个合法终点事实；离场保留；本服兼容2秒终局，原2秒是统计延迟。 */
  struct Goal final {
    domain::PlayerId runner;
    std::uint8_t team;
    Time due;
  };

  const CrazyRacingMap* map_;
  std::map<domain::PlayerId, Runner> runners_;
  std::optional<Goal> goal_;
  std::mt19937 random_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CRAZY_RACING_MATCH_H_
