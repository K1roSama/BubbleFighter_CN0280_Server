#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_MATCH_H_

/**
 * @file training_room_match.h
 * @brief 私人训练绑定固定木桩；客户端不能指定任意角色、队伍或资产变更。
 */
#include "features/battle/combat_relay.h"
#include "features/practice/training_room.h"

namespace bf::runtime {
// The original client owns the dummy animations and its reset timer. The
// private server binds only the seven fixed IDs; no client-supplied characters,
// teams, equipment, arbitrary actors, rewards or inventory mutations are used.
/** @brief 仅场景所有者可重置固定对象或临时换装，训练不产生持久化库存。 */
class TrainingRoomMatch final {
 public:
  TrainingRoomMatch(domain::PlayerId owner, std::uint32_t actor,
                    CombatRelay& combat);
  [[nodiscard]] CombatRelayResult Reset(domain::PlayerId sender,
                                        std::uint32_t object,
                                        CombatRelay& combat);
  [[nodiscard]] CombatRelayResult Equip(domain::PlayerId sender,
                                        std::uint32_t slot, std::uint32_t item,
                                        CombatRelay& combat);

 private:
  void Bind(std::size_t index, CombatRelay& combat, bool force_bubble = false);
  domain::PlayerId owner_;
  std::uint32_t actor_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TRAINING_ROOM_MATCH_H_
