#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_SHOP_CATALOG_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_SHOP_CATALOG_MESSAGES_H_

/**
 * @file shop_catalog_messages.h
 * @brief 定义 0.2.80.1 商城目录响应的完整已恢复编码。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/shop/shop_catalog.h"

namespace bf::protocol {

/**
 * @brief 编码 C2S801 对应的 S2C800 普通商城目录、分类树和当前选择。
 * @note 无效选择兼容旧服务回退到已发布分类 300；若 300 未发布则回退首个主分类。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeShopCatalogResponse(
    const application::PublishedShopCatalog& catalog,
    std::uint32_t requested_selection);

/** @brief 编码 C2S802 对应的 S2C801 第二类商城目录；该消息不携带分类树。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCashShopCatalogResponse(
    const application::PublishedShopCatalog& catalog);

/**
 * @brief 编码不带外层 vector 数量的单个商城卡片。
 * @note 泡泡 S2C918 复用客户端 ShopCard
 * 布局；调用方必须提供已校验且至少含一个价格的卡片。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeShopCard(
    const application::ShopCardView& card);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_SHOP_CATALOG_MESSAGES_H_
