#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_PROFILE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_PROFILE_H_

/**
 * @file event_mole_profile.h
 * @brief 保存原版 EventMole 地图只读资源；缺对象登记链时不得据此开放玩法。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "protocol/scenes/scene_messages.h"

namespace bf::runtime {

inline constexpr std::uint32_t kEventMoleMode =
    17U;  ///< EventMole 模式，不是 NPC 的 AI 类型。
inline constexpr std::uint8_t kEventMoleNpcTeam =
    35U;  ///< DFD60B 返回的 MoleInEventMole 阵营。
inline constexpr std::string_view kEventMoleMap =
    "rel_event_lucci03";  ///< 唯一已核实的原版活动图。

/** @brief 原始 ACTION_POINT；未被有效 spawner 引用的点也只保留数据，不启用。 */
struct EventMoleActionPoint final {
  std::uint32_t id;
  std::array<float, 3U> position;
  float rotation;
  std::uint32_t hold_time;
  std::string_view action;
  std::string_view move_motion;
};

/** @brief 原 INI 属性及当前 AiFighterConfig 的名字索引模板；不是对象认证来源。
 */
struct EventMoleProperty final {
  std::uint32_t id;
  std::uint32_t hp;  ///< 地图 ChrProp 覆盖值，不能误用 FighterConfig 基础 HP。
  std::uint32_t lp;
  std::string_view resource_name;
  std::uint32_t character_type;  ///< 四个原始模板列确认为 306/307/306/306。
  std::uint32_t ai_type;  ///< 当前资源均为17，和模式编号属于不同命名空间。
  float scale;            ///< 当前模板原值 1.8/1.6/2/6，不是奖励或命中倍率。
  std::uint32_t fighter_base_hp;    ///< FighterConfig 基础 HP770，仅保存原值。
  std::uint32_t fighter_bubble_hp;  ///< FighterConfig 基础 bubbleHp0。
};

/** @brief 非注释 CHR_SPAWNER 原始字段；不推断 MaxCount 语义或重新排序
 * Interval。 */
struct EventMoleSpawner final {
  std::uint32_t id;
  std::uint32_t property;
  std::uint8_t team;
  std::uint32_t chr_count;
  std::uint32_t max_count;
  std::int32_t
      start_ms;  ///< 6BFF00 实际读取 SartTime，保留原资源拼写对应的数值。
  std::int32_t end_ms;  ///< -1 哨兵不转换为某个猜测时限。
  std::array<std::int32_t, 2U>
      raw_interval_ms;  ///< 原顺序；65 号是 40000,10000。
  std::uint32_t position_id;
};

/** @brief 原始资源键、值及原生查找键；拼写不一致不能静默补成运行值。 */
struct EventMoleResourceOption final {
  std::string_view resource_key;
  std::string_view raw_value;
  std::string_view native_lookup_key;
};

/** @brief 只读图谱，不是已授权的对象计划或本服替代玩法。 */
struct EventMoleProfile final {
  std::string_view map_key;
  std::string_view display_name;
  std::string_view map_resource_sha256;
  std::string_view game_option_sha256;
  std::string_view fighter_config_sha256;
  std::uint32_t minimum_players;
  std::uint32_t maximum_players;
  std::uint32_t default_duration_ms;
  bool individual;  ///< MapMeta.isSingle=1；不代表官方胜负/评级公式已恢复。
  std::uint32_t native_class;
  std::uint32_t native_ai_type;
  std::span<const EventMoleActionPoint> action_points;
  std::span<const EventMoleProperty> properties;
  std::span<const EventMoleSpawner> active_spawners;
  std::span<const EventMoleResourceOption> lifecycle_options;
  std::span<const EventMoleResourceOption>
      declared_game_options;  ///< on-pang 声明不是 on-hit 奖池。
};

/** @brief 返回固定原始资源视图；不登记对象、不消耗资源、不生成奖励。 */
[[nodiscard]] const EventMoleProfile& GetEventMoleProfile() noexcept;
/** @brief 比较原资源键，忽略 ASCII 大小写以防跨模式开局检查被大小写绕过。 */
[[nodiscard]] bool IsEventMoleMap(std::string_view key) noexcept;
/** @brief 修正 402/513/514 的活动地图预览；离开该模式时清掉缓存的专用图。 */
void NormalizeEventMoleConfiguration(
    protocol::SceneConfiguration& configuration);
/** @brief 当前缺可靠对象/cookie
 * 同步登记及原服终局规则；不提供绕过此保护的开关。 */
[[nodiscard]] std::string_view EventMoleStartBlocker() noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_PROFILE_H_
