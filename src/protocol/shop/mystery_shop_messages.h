#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_MYSTERY_SHOP_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_MYSTERY_SHOP_MESSAGES_H_

/**
 * @file mystery_shop_messages.h
 * @brief 定义 build 752 神秘商店 C2S129 与 S2C140 的严格消息模型。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/configuration/service_rules.h"
#include "features/shop/mystery_shop_service.h"

namespace bf::protocol {

/** @brief 神秘商店动作种类；数值必须保持客户端原生枚举。 */
enum class MysteryAction : std::uint8_t {
  kOpen = 1U,
  kQuery = 2U,
  kRefresh = 3U,
  kPurchase = 4U,
};

/** @brief C2S129 请求；仅购买携带 stock，查询/刷新携带 value。 */
struct MysteryShopRequest final {
  MysteryAction action{MysteryAction::kOpen};  ///< 客户端动作。
  std::uint32_t generation{0U};                ///< 客户端看到的选品代次。
  std::uint32_t stock_id{0U};                  ///< 购买目标；其他动作固定为零。
  std::uint16_t value{0U};                     ///< 刷新固定为一、购买固定为零。
};

/** @brief 严格解析四种 C2S129 形状并拒绝尾随字节。 */
[[nodiscard]] MysteryShopRequest DecodeMysteryShopRequest(
    std::span<const std::uint8_t> body);

/** @brief 后台查询同代次只确认购买位图，避免重置原生倒计时查询阈值。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMysteryShopQueryResponse(
    const MysteryShopRequest& request,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now);

/**
 * @brief 编码成功 S2C140，包含配置、剩余秒数、六项选品、购买位图及可选奖励。
 * @param action 服务端已经执行成功的动作，决定响应中的客户端分支。
 * @param selection 同一事务返回的当前选品、代次和已购买状态快照。
 * @param rules 与 selection 配套的已发布规则，用于编码刷新成本及有效期。
 * @param now 生成响应时的 UTC 时间，用于计算非负剩余秒数。
 * @param reward_stock_id 仅购买成功时为成交 Stock
 * ID，供原版商品弹窗解析；其他动作为零。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMysteryShopResponse(
    MysteryAction action,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now,
    std::uint32_t reward_stock_id = 0U);

/**
 * @brief 编码状态为一的完整 S2C140 失败响应，仍返回服务端当前真实选品。
 * @param action 被拒绝的客户端动作。
 * @param selection 失败后重新读取的服务端权威选品快照。
 * @param rules 与 selection 配套的已发布规则。
 * @param now 生成响应时的 UTC 时间，用于计算非负剩余秒数。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMysteryShopFailure(
    MysteryAction action,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_MYSTERY_SHOP_MESSAGES_H_
