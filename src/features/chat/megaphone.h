#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_H_

/**
 * @file megaphone.h
 * @brief 定义喇叭库存事务、耐久待投递记录与重试边界。
 */
#include <cstdint>
#include <string>

#include "application/identifier.h"
#include "application/operation_id.h"

namespace bf::application {
/** @brief 经连接认证并在世界线程冻结频道后的喇叭请求。 */
struct MegaphoneCommand final {
  domain::OperationId operation_id;
  domain::PlayerId player_id;
  std::uint32_t channel{60U};
  std::uint32_t type{101U};
  std::uint32_t selection{10000U};
  std::u16string text;
};

/** @brief 与扣除一个道具同事务提交的广播记录；昵称与颜色均取自服务端。 */
struct MegaphoneReceipt final {
  std::string business_id;
  domain::PlayerId player_id;
  std::uint32_t channel{60U};
  std::uint32_t type{101U};
  std::uint32_t item_id{0U};
  std::uint32_t color{0U};
  std::u16string sender_name;
  std::u16string text;
  bool published{false};
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHAT_MEGAPHONE_H_
