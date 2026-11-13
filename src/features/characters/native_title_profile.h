#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_PROFILE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_PROFILE_H_

/** @file native_title_profile.h
 * @brief 关系变化后只同步称号穿戴标记，保留当前武器、库存与其他世界缓存字段。
 */
#include "features/lobby/player_menu_snapshot.h"

namespace bf::application {
/** @brief
 * 仅按同主人、同实例、同原称号键同步缓存equipped；不替换装备页或其他资产。 */
void SyncNativeTitleEquipmentFlags(PlayerMenuSnapshot& cached,
                                   const PlayerMenuSnapshot& fresh) noexcept;
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_PROFILE_H_
