#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_DUMMY_DATA_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_DUMMY_DATA_H_

// Generated from build-1302 TrainingRoomSetting.yaml; do not hand-edit.
// Source SHA256:
// e95d8202abf9b59f5964bf27ae2e8c4c451f0a3945ef904a5225e92bbf24c7e9
/**
 * @file training_dummy_data.h
 * @brief 原始训练数据只由生成器输出，不接受客户端提供的木桩或装备定义。
 */
#include <array>
#include <cstdint>

namespace bf::runtime {
/** @brief 位置、队伍和复位时间来自固定资源，不可由网络包改写。 */
struct TrainingDummySpec {
  std::array<float, 3> position;
  std::uint8_t team;
  std::uint32_t delay;
  bool locked;
};

inline constexpr std::array<TrainingDummySpec, 7> kTrainingDummies{{
    // HelpBubble1
    {{1103.08f, 270.23f, 1014.25f}, 1, 4000, true},
    // HelpBubble2
    {{1032.72f, 270.23f, 1100.68f}, 1, 4000, true},
    // HelpBubble3
    {{1122.45f, 270.17f, 1097.8f}, 1, 4000, true},
    // PangBubble1
    {{924.49f, 270.23f, 1008.33f}, 2, 4000, true},
    // PangBubble2
    {{872.08f, 270.17f, 1091.41f}, 2, 4000, true},
    // PangBubble3
    {{977.2f, 270.17f, 1102.87f}, 2, 4000, true},
    // TrainingDummy1
    {{1006.14f, 270.23f, 1003.13f}, 2, 1000, false},
}};

/** @brief 训练装备仅允许发布资源中的槽位与模板组合，不产生库存。 */
struct TrainingEquipmentSpec {
  std::uint32_t slot, item;
};

inline constexpr std::array<TrainingEquipmentSpec, 12> kTrainingEquipment{{
    {21, 205002},
    {21, 205080},
    {21, 205086},
    {24, 100171},
    {24, 100144},
    {24, 102640},
    {5, 101988},
    {5, 100431},
    {5, 101923},
    {5, 100986},
    {5, 102611},
    {5, 101046},
}};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_DUMMY_DATA_H_
