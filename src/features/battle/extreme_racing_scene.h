#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_SCENE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_SCENE_H_

/**
 * @file extreme_racing_scene.h
 * @brief 将原图对象与明确的本服计分配置冻结为竞速初始化和危险物计划。
 */

#include <chrono>

#include "features/battle/extreme_racing_maps.h"
#include "protocol/battle/extreme_racing_messages.h"

namespace bf::runtime {
/** @brief 已验证地图位置的金币；对象和位置不能来自客户端请求。 */
struct ExtremeRacingCoinPlacement final {
  std::uint32_t object{0U};
  std::uint32_t stage{0U};
  std::array<float, 3U> position{};
  std::int32_t points{0};  ///< 本服局内分，不直接变更账号金币。
};

/** @brief 每种中立对象的起始编号；区间必须互斥，开局另与玩家对象核对。 */
struct ExtremeRacingObjectBases final {
  std::uint32_t cannons{50000U};
  std::uint32_t coins{50100U};
  std::uint32_t disposers{50200U};
};

/** @brief 炮台计划中的一个武器批次；原图二使用
 * cannon.ini，原图一采用明示本服配置。 */
struct ExtremeRacingCannonWeapon final {
  std::uint32_t weapon{0U};
  std::uint32_t count{0U};
  float speed_multiplier{1.0F};
};

/** @brief 冻结的真实炮台及出口方向；计划不等于已发放的炮弹权限。 */
struct ExtremeRacingCannonPlacement final {
  std::uint32_t object{0U};
  std::uint32_t exit_object{0U};
  std::uint32_t group{0U};
  std::uint32_t stage{0U};
  std::array<float, 3U> position{};
  std::array<float, 3U> direction{};
  std::chrono::milliseconds interval{0};
  std::vector<ExtremeRacingCannonWeapon> weapons;
};

/**
 * @brief
 * 已检查的地图初始化，公开只读访问，避免调用者篡改对象和原生字典后启动核心。
 * @note 金币类型、分值及阶段分配是明确的本服规则，并非恢复的原服私有数据。
 */
class ExtremeRacingScene final {
 public:
  /** @brief 从已编译原图和本服策略生成完整对象字典；seed 冻结本场危险物选择。
   */
  [[nodiscard]] static ExtremeRacingScene Load(
      std::string_view map, std::uint32_t seed,
      ExtremeRacingObjectBases bases = {});

  /** @brief 原生地图的规范键名。 */
  [[nodiscard]] std::string_view Map() const noexcept { return map_; }

  /** @brief 763 的真实对象数量和完整金币双字典。 */
  [[nodiscard]] const protocol::ExtremeRacingInitialization& Initialization()
      const noexcept {
    return initialization_;
  }

  /** @brief 768 的图案、种子和合法消失地板路线。 */
  [[nodiscard]] const protocol::ExtremeRacingHazards& Hazards() const noexcept {
    return hazards_;
  }

  /** @brief 以原生加载顺序排列的服务端金币位置及局内分值。 */
  [[nodiscard]] const std::vector<ExtremeRacingCoinPlacement>& Coins()
      const noexcept {
    return coins_;
  }

  /** @brief 与 763 炮台区间对应的发射计划。 */
  [[nodiscard]] const std::vector<ExtremeRacingCannonPlacement>& Cannons()
      const noexcept {
    return cannons_;
  }

  /** @brief 投石消失装置坐标；不是投石发射器数量。 */
  [[nodiscard]] const std::vector<std::array<float, 3U>>& Disposers()
      const noexcept {
    return disposers_;
  }

  /** @brief 检查是否占用当前场景的任一中立对象编号。 */
  [[nodiscard]] bool OwnsObject(std::uint32_t object) const noexcept;

 private:
  ExtremeRacingScene() = default;
  std::string_view map_;
  protocol::ExtremeRacingInitialization initialization_;
  protocol::ExtremeRacingHazards hazards_;
  std::vector<ExtremeRacingCoinPlacement> coins_;
  std::vector<ExtremeRacingCannonPlacement> cannons_;
  std::vector<std::array<float, 3U>> disposers_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EXTREME_RACING_SCENE_H_
