#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_RANKING_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_RANKING_MESSAGES_H_

/**
 * @file fighter_ranking_messages.h
 * @brief 定义战士分项等级 C2S935 与 S2C939 的原生线格式。
 */

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "application/identifier.h"

namespace bf::protocol {

/** @brief 排名查询只包含目标身份，不是称号编号。 */
struct FighterRankingRequest final {
  domain::PlayerId target;
};

/** @brief 等级编号、数值和显示名称的映射表；不是玩家排名列表。 */
struct FighterRankGrade final {
  std::uint32_t grade_id{8U};
  std::int32_t rank_value{-1};
  std::u16string label;
  // 原对象+36的业务含义仍未恢复，保留线格式而不猜测。
  std::uint32_t field_36{0U};
};

/** @brief
 * 模式/地图/角色类型的等级；character_type是ChrTypeIdx，不是商城物品号。 */
struct FighterCharacterRankEntry final {
  std::uint32_t mode_id{0U};
  std::uint32_t map_id{0U};  ///< MapMeta.MapIdx，不是地图名称的散列。
  std::uint32_t character_type{0U};
  std::int32_t rank_value{-1};  ///< -1表示未取得评价，不是次数或玩家名次。
};

/** @brief 武器分项使用类别2/3/4（步枪/散弹/狙击），不能填武器模板号。 */
struct FighterWeaponRankEntry final {
  std::uint32_t mode_id{0U};
  std::uint32_t map_id{0U};
  std::uint32_t weapon_category{0U};
  std::int32_t rank_value{-1};
};

/** @brief 模式/地图的等级；原客户端按地图计算完成率及模式综合等级。 */
struct FighterMapRankEntry final {
  std::uint32_t mode_id{0U};
  std::uint32_t map_id{0U};
  std::int32_t rank_value{-1};
};

/** @brief 四组列表先于结果字段；原生客户端只在结果为零时更新排名缓存。 */
struct FighterRankingResponse final {
  std::span<const FighterRankGrade> grades;
  std::span<const FighterCharacterRankEntry> characters;
  std::span<const FighterMapRankEntry> maps;
  std::span<const FighterWeaponRankEntry> weapons;
  std::uint32_t result{1U};
};

[[nodiscard]] FighterRankingRequest DecodeFighterRankingRequest(
    std::span<const std::uint8_t> body);

[[nodiscard]] std::vector<std::uint8_t> EncodeFighterRankingResponse(
    const FighterRankingResponse& response);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CHARACTERS_FIGHTER_RANKING_MESSAGES_H_
