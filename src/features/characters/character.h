#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_H_

/**
 * @file character.h
 * @brief 定义原版首次角色选项及其初始物品模板映射。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "application/identifier.h"

namespace bf::domain {

/**
 * @brief 当前 0.2.80.1 客户端真实发送的四个首次角色编号。
 * @note 编号并不连续；11 和 12 已由原版 UI 与客户端 SecurityInt
 * 字段互证，不能改写成 3/4。
 */
enum class InitialFighter : std::uint8_t {
  kDao = 1U,
  kDizni = 2U,
  kReo = 11U,
  kRabi = 12U,
};

/**
 * @brief 一项首次建角时必须发放的物品及其装备策略。
 * @note `equip` 为 false 的模式兼容武器只进入库存，不得替换玩家正常主武器。
 */
struct StarterItemDefinition final {
  ItemId item_id;     ///< 客户端物品模板编号。
  bool equip{false};  ///< 是否在零号配装页按模板槽位立即装备。
};

/** @brief 建角基本装备及原教程自动换枪所需的七项物品。 */
using StarterItemDefinitions = std::array<StarterItemDefinition, 7>;

/** @brief 将不可信协议 DWORD 映射为已支持角色；其他编号返回空。 */
[[nodiscard]] std::optional<InitialFighter> InitialFighterFromProtocol(
    std::uint32_t protocol_value) noexcept;

/** @brief 返回首次角色随创建事务发放的角色物品模板 ID。 */
[[nodiscard]] ItemId InitialFighterItem(InitialFighter fighter) noexcept;

/** @brief 返回用于日志、指标和测试的稳定英文角色代码。 */
[[nodiscard]] std::string_view InitialFighterName(
    InitialFighter fighter) noexcept;

/**
 * @brief 返回基本装备及原客户端教程自动换枪所需的练习武器集合。
 * @note 首项随所选角色变化；202004/203004/204004用于原804教程换枪，不自动装备。
 */
[[nodiscard]] StarterItemDefinitions GetStarterItemDefinitions(
    InitialFighter fighter) noexcept;

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_H_
