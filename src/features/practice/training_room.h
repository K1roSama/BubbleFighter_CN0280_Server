#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_H_

/**
 * @file training_room.h
 * @brief 训练场兼容入口避开旧客户端108冲突，不授予任意场景或资产权限。
 */

#include <cstdint>
#include <string_view>

namespace bf::runtime {
// Compatibility entry only: original build-1302 C2S108 collides with the old
// client's plaza user list. The authenticated server owns the private scene.
inline constexpr std::uint16_t kTrainingEntryOpcode = 65033U;
inline constexpr std::uint16_t kTrainingResetOpcode = 65034U;
// Temporary training loadout only; category/item DWORDs, with (0,0) restoring
// the authenticated loadout. The acknowledgement echoes an accepted pair.
inline constexpr std::uint16_t kTrainingEquipmentOpcode = 65035U;
inline constexpr std::uint32_t kTrainingMode = 40U;
inline constexpr std::string_view kTrainingMap = "rel_trainingRoom01";
inline constexpr std::string_view kTrainingName = "训练场";
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_H_
