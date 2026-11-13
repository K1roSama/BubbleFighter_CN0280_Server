#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_SESSION_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_SESSION_MESSAGES_H_

/**
 * @file scene_session_messages.h
 * @brief 定义 S2C112 菜单、广场和房间共用的 GameSession/UserSlot 快照编码。
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "features/characters/level_icon.h"
#include "features/lobby/player_menu_snapshot.h"
#include "protocol/scenes/scene_messages.h"

namespace bf::protocol {

/** @brief 原版 UserSlot +128/+132/+136：队长、广场队伍图标索引、公开招募状态。
 */
struct ScenePartyState final {
  bool leader{false};
  std::uint32_t party_id{
      0xFFFFFFFFU};  ///< 0..7；无图标为 -1。绝不能写入玩家 UID。
  bool recruiting{false};
  bool operator==(const ScenePartyState&) const = default;
};

/**
 * @brief S2C112 UserSlot vector 中一个确定索引的席位值。
 * @note 空席位仍必须完整编码，房间锁更新会直接解引用对应索引；空席位 team 使用
 * 255。
 */
struct SceneSessionSlot final {
  std::uint8_t slot{0U};    ///< 零基席位索引，必须与 vector 位置一致。
  std::uint8_t team{255U};  ///< 菜单0，团队1/2，个人seat+1，观战36，空席255。
  bool occupied{false};     ///< 是否有已认证玩家占用。
  bool locked{false};       ///< 房主是否锁定该空席位。
  bool ready{false};        ///< 非房主准备状态。
  std::optional<application::PlayerMenuSnapshot>
      player;             ///< 占用席位的权威资料/装备快照。
  ScenePartyState party;  ///< 广场队伍状态，不取客户端上报身份。
};

/** @brief 一次 S2C112 所需的场景、主客席位和完整席位快照。 */
struct SceneSessionSnapshot final {
  SceneInfo scene;                  ///< 完整场景及配置。
  std::uint32_t host_slot{0U};      ///< 客户端用于显示房主权限的席位。
  std::uint32_t selected_slot{0U};  ///< 当前连接本地选中的席位。
  std::vector<SceneSessionSlot>
      slots;  ///< 普通房间含 capacity 个席位；赏金房间固定 16 槽含观战席。
  bool include_equipment_records{
      true};  ///< 房间/广场为 true；主菜单兼容包必须为 false。
};

/**
 * @brief 编码 S2C112 完整 GameSession。
 * @note 占用席位携带各自一致性菜单快照；空席位仍写完整
 * UserSlot。启用装备记录时只为 占用席位生成，顺序与 slot vector
 * 中占用者的顺序一致。主菜单是已验证的兼容分支， 必须显式关闭记录并写空
 * vector。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSceneSessionResponse(
    const SceneSessionSnapshot& snapshot);

/**
 * @brief 编码 S2C503：外层席位、完整占用 UserSlot 与该玩家的装备记录。
 * @note 外层席位始终取 `slot.slot`，避免广播索引与内嵌 UserSlot 索引分裂。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSceneMemberUpdateResponse(
    const SceneSessionSlot& slot);

/** @brief S2C511 的占用席位更新；与 503 不同，不附加装备记录。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSceneMemberStateResponse(
    const SceneSessionSlot& slot);

/** @brief S2C516：原生有符号短席位后接招募 UTF16 文字。 */
[[nodiscard]] std::vector<std::uint8_t> EncodePartyRecruitmentTextResponse(
    std::uint8_t slot, std::u16string_view text);

/** @brief 编码 S2C511：外层席位及完整未锁定空 UserSlot，用于移除旧场景对象。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSceneMemberRemovedResponse(
    std::uint8_t slot);

/** @brief 严格读取 C2S2008 的三个 DWORD，不接受附加数据。 */
[[nodiscard]] application::LevelIconSelection DecodeLevelIconRequest(
    std::span<const std::uint8_t> body);
/** @brief S2C973 只有完整 UserSlot，没有外层席位、状态字或装备记录。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLevelIconResponse(
    const SceneSessionSlot& slot);

[[nodiscard]] std::vector<std::uint8_t> EncodeCoupleSlotResponse(
    const SceneSessionSlot& slot);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_SESSION_MESSAGES_H_
