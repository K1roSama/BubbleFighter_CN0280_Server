#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_TREASURE_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_TREASURE_MESSAGES_H_

/**
 * @file treasure_messages.h
 * @brief 定义 build 752 五泡抽取 C2S917～920 与 S2C918～921 消息。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/configuration/service_rules.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/treasure_service.h"

namespace bf::protocol {

/** @brief C2S917 空负载的泡泡面板状态请求。 */
struct TreasureStateRequest final {};

/** @brief C2S918 选择魔法针和两个泡泡格位的抽取请求。 */
struct TreasureOpenRequest final {
  std::uint32_t needle_item_id{
      0U};  ///< 客户端选择的针模板，业务层按发布规则复核。
  std::uint32_t primary_position{
      0U};  ///< 兼容旧客户端单格位字段，只允许 0..4。
  std::vector<std::uint32_t>
      positions;  ///< 空表示使用主格及相邻格；非空必须恰好两个。
};

/** @brief C2S919 从刚才两个格位中领取一个奖励。 */
struct TreasureClaimRequest final {
  std::uint32_t choice{0U};    ///< 固定二选一下标 0 或 1。
  std::uint32_t position{0U};  ///< 必须与服务端 pending 对应下标格位一致。
};

/** @brief C2S920 查询所选魔法针的奖励详情，不得重置抽取面板。 */
struct TreasurePreviewRequest final {
  std::uint32_t needle_item_id{0U};  ///< AADD60 / AB7290 发送当前所选针模板。
};

/** @brief 严格解析空负载 C2S917。 */
[[nodiscard]] TreasureStateRequest DecodeTreasureStateRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S918 的针、单格位及最多五项格位 vector。 */
[[nodiscard]] TreasureOpenRequest DecodeTreasureOpenRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S919 的 choice 和 position。 */
[[nodiscard]] TreasureClaimRequest DecodeTreasureClaimRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S920 的魔法针模板。 */
[[nodiscard]] TreasurePreviewRequest DecodeTreasurePreviewRequest(
    std::span<const std::uint8_t> body);

/** @brief S2C921 概率分组、魔法针及 TreasureInfo 详情；B990D0/B996A0。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasurePreviewResponse(
    std::uint32_t needle_item_id, const application::BubbleServiceRules& rules);

/**
 * @brief 编码 S2C918 面板状态、奖励预览和魔法针商城卡片。
 * @note 针计数与规则顺序必须相同；卡片必须是 item 100382 的已发布目录卡片。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasureStateResponse(
    const application::TreasureStateSnapshot& state,
    const application::BubbleServiceRules& rules,
    const application::ShopCardView& needle_card);

/** @brief 编码 S2C919 的固定五项奖励、两个格位、剩余资产和选择倒计时。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasureRevealResponse(
    const application::TreasureDrawSnapshot& draw);

/** @brief 编码 S2C920 领取成功动画及原顺序五项 stock。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasureClaimResponse(
    const application::TreasureClaimResult& result);

/** @brief 编码旧客户端可安全处理的 S2C919 失败结构。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasureOpenFailure();

/** @brief 编码旧客户端可安全处理的 S2C920 失败结构。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTreasureClaimFailure();

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_TREASURE_MESSAGES_H_
