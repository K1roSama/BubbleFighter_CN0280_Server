#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_OWNERSHIP_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_OWNERSHIP_H_

/**
 * @file room_ownership.h
 * @brief 定义与进程和数据库实现无关的房间持久化所有权令牌。
 */
#include "features/battle/match_id.h"

namespace bf::application {
/** @brief 开局冻结的实例和代次；不能从客户端字段推导。 */
struct RoomOwnership final {
  domain::MatchId instance_id;
  std::uint64_t owner_epoch{0U};
  bool operator==(const RoomOwnership&) const = default;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_ROOM_OWNERSHIP_H_
