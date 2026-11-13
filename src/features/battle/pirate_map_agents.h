#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAP_AGENTS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAP_AGENTS_H_

/**
 * @file pirate_map_agents.h
 * @brief 保存海盗地图角色的原生 64 槽生成、动画、移动和移除状态。
 */
#include <array>
#include <optional>
#include <set>
#include <string_view>

#include "features/battle/combat_relay.h"
#include "protocol/codec/frame_codec.h"

namespace bf::runtime {
/** @brief 地图角色与海盗波次 Mob 分离；调用者必须先验证房主和加载阶段。 */
class PirateMapAgents final {
 public:
  /** @brief 仅两张原生 Horde 守护地图安装关联生成器规则。 */
  void Configure(std::uint32_t mode, std::string_view map,
                 std::uint32_t difficulty);
  /** @brief 741/740 原生召唤物生成和拾取，与地图角色的 peer 消息分开。 */
  [[nodiscard]] static bool AcceptsLink(std::uint16_t opcode) noexcept;
  static void ValidateLink(const protocol::Packet& packet);
  /** @brief 已接受的海盗生成组触发一次原始地图中的 LinkID 列表。 */
  void ObserveGroup(std::uint16_t wave, std::uint8_t group,
                    std::chrono::steady_clock::time_point now);
  [[nodiscard]] CombatRelayResult HandleLink(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 有值表示守护地图角色；false 暂存绑定前动作，空值交给普通海盗账本。
   */
  [[nodiscard]] std::optional<bool> PrepareAction(
      domain::PlayerId sender, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 按原始守护者寿命退休已绑定角色；Angel 与地面召唤物不使用此期限。 */
  [[nodiscard]] std::vector<OutboundMessage> Tick(
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const;
  [[nodiscard]] std::vector<protocol::Packet> TakeDeferredActions(
      std::uint32_t object);
  /** @brief 判断已恢复的地图角色协议。 */
  [[nodiscard]] static bool Accepts(std::uint16_t opcode) noexcept;
  /** @brief 校验原生字段宽度、有限坐标和 64 个槽位上限。 */
  static void Validate(const protocol::Packet& packet);
  /** @brief 只接纳有效状态转换；true
   * 时向其他成员原样转发，不回送本机预测的房主。 */
  [[nodiscard]] bool Apply(const protocol::Packet& packet,
                           std::uint32_t host_object,
                           CombatRelay* combat = nullptr,
                           std::chrono::steady_clock::time_point now = {});

 private:
  /** @brief 原生角色槽的存活状态、生成指纹和已确认对象号。 */
  struct Agent final {
    bool active{false};
    std::vector<std::uint8_t> spawn;
    std::optional<std::uint32_t> object;
    std::uint32_t spawner{0U};
    std::chrono::steady_clock::time_point created{};
  };

  std::array<Agent, 64U> agents_{};
  std::set<std::uint32_t> animated_objects_;
  using Time = std::chrono::steady_clock::time_point;

  /** @brief 单个关联生成器的地面物品与守护者许可。 */
  struct Link final {
    std::optional<Time> permitted;
    std::uint32_t object{0U};
    Time created{};
    bool spawn_permitted{false};
    bool guardian_active{false};
  };

  /** @brief 零为普通地图，1 为公主，2 为小屋；原始资源最多五个关联生成器。 */
  std::uint8_t guardian_map_{0U};
  std::uint32_t difficulty_{1U};
  std::array<Link, 5U> links_{};
  std::set<std::pair<std::uint16_t, std::uint8_t>> groups_;
  bool angel_spawned_{false};
  std::uint32_t link_serial_{70'000'000U};
  std::set<std::uint32_t> retired_;
  std::map<std::uint32_t, std::vector<protocol::Packet>> deferred_;
  [[nodiscard]] bool ApplyGuardian(const protocol::Packet& packet,
                                   std::uint32_t host_object,
                                   CombatRelay* combat, Time now);
  void Retire(Agent& agent, CombatRelay& combat);
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_PIRATE_MAP_AGENTS_H_
