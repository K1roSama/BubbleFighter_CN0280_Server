#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SURVIVAL_LOOT_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SURVIVAL_LOOT_H_

/** @file survival_loot.h
 * @brief 生存地图原始掉落表生成与 758 显式对象编码。 */

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sessions/outbound_message.h"

namespace bf::runtime {
/** @brief 一件服务器持有的原生 GoGenItem；物品类型与武器模板不是同一编号。 */
struct SurvivalLoot final {
  std::uint32_t id{0U};
  std::uint32_t type{0U};
  std::uint32_t quantity{0U};
  std::array<float, 3U> position{};
  std::array<float, 3U> velocity{};
  std::chrono::steady_clock::time_point
      active_at{};  ///< 每个原生对象独立的一秒激活时间。
  std::optional<std::chrono::steady_clock::time_point>
      expires;  ///< 主动丢弃十五秒后消失。
};

/** @brief 为整批物品预留跨房间/比赛不重复的原生正整数对象编号。 */
void AllocateSurvivalLootIds(std::span<SurvivalLoot> loot);
/** @brief 解码原生 750 的物品向量；尺寸、数量、向量和十五秒标志严格验证。 */
[[nodiscard]] std::vector<SurvivalLoot> DecodeSurvivalDrop(
    std::span<const std::uint8_t> body);
/** @brief 按原表生成显式对象；未知地图返回空，不声称与 752 的哈希遍历顺序相同。
 */
[[nodiscard]] std::vector<SurvivalLoot> GenerateSurvivalLoot(
    std::string_view map, std::uint32_t seed);
/** @brief 将完整生成列表编码为最多 128 项一包的原生
 * 758；初始物品不启用十五秒过期。 */
[[nodiscard]] std::vector<OutboundMessage> EncodeSurvivalLoot(
    std::span<const SurvivalLoot> loot);
/** @brief 查询原始 GenItemSpec 的武器模板；非武器物资返回零。 */
[[nodiscard]] std::uint32_t SurvivalLootWeapon(std::uint32_t type) noexcept;

/** @brief 原生 BackpackItem 的三个 DWORD，模板不是物品类型。 */
struct SurvivalBackpackItem final {
  std::uint32_t type{0U};
  std::uint32_t weapon{0U};
  std::uint32_t quantity{0U};
};

/** @brief 原生死亡背包，提交时 id 为零，广播时由服务器分配。 */
struct SurvivalBackpack final {
  std::uint32_t id{0U};
  std::uint32_t actor{0U};
  std::array<float, 3U> position{};
  std::array<float, 3U> velocity{};
  std::u16string nickname;
  std::vector<SurvivalBackpackItem> items;
};

/** @brief 严格解码 748，最多三把主武器和十一个物资槽。 */
[[nodiscard]] SurvivalBackpack DecodeSurvivalBackpack(
    std::span<const std::uint8_t> body);
/** @brief 编码包含真实对象、坐标、姓名及库存的原生 754。 */
[[nodiscard]] OutboundMessage EncodeSurvivalBackpack(
    const SurvivalBackpack& bag);
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_SURVIVAL_LOOT_H_
