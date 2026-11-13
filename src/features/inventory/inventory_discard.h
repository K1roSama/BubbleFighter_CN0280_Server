#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_INVENTORY_DISCARD_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_INVENTORY_DISCARD_H_

/** @file inventory_discard.h
 * @brief 原808只销毁已过期库存；本人物品映射、实例冻结与首次审计。
 */
#include <cstdint>

#include "application/identifier.h"
#include "application/operation_id.h"

namespace bf::application {
/** @brief 原808只有一个库存原生键；实例主键由认证玩家的唯一库存映射确定。 */
struct ExpiredItemDiscardCommand final {
  domain::OperationId operation_id;
  domain::PlayerId player_id;
  std::uint32_t native_item_key{0U};
};

/** @brief 已提交删除或同一已删除原生键的幂等回执，不能授权活跃库存。 */
struct ExpiredItemDiscardResult final {
  std::uint32_t native_item_key{0U};
  domain::ItemInstanceId item_instance_id;
  std::uint32_t removed_quantity{0U};
  bool replay{false};
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_INVENTORY_DISCARD_H_
