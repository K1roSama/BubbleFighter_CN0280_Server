#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_WORKSHOP_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_WORKSHOP_MESSAGES_H_

/**
 * @file workshop_messages.h
 * @brief 定义科学工坊 C2S132～139、动态配置 1500 及对应响应。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/shop/workshop_service.h"

namespace bf::protocol {

/** @brief C2S132～137 共用的物品 vector 与选项。 */
struct WorkshopRequest final {
  std::vector<std::uint32_t> input_item_ids;  ///< 最多三十二个客户端物品模板。
  std::uint32_t option{0U};                   ///< 动作相关选项。
};

/** @brief
 * 原617380的136请求；唯一输入是SetItem.setId，不是商城销售键或库存实例号。 */
struct AdvancedClothingRequest final {
  std::uint32_t set_id{0U};  ///< 原仓库套装编号，后续必须检查真实组件库存。
};

/** @brief 解析136的单个非零setId及固定option0，拒绝多输入、截断与尾随。 */
[[nodiscard]] AdvancedClothingRequest DecodeAdvancedClothingRequest(
    std::span<const std::uint8_t> body);

/** @brief C2S1500 动态配置校验报告中的十六位 section mask。 */
struct DynamicConfigurationRequest final {
  std::uint16_t mask{
      0U};  ///< 每个置位 section 在负载中另带一个客户端校验 DWORD。
};

/** @brief 严格解析工坊 vector 和选项并拒绝尾随字段。 */
[[nodiscard]] WorkshopRequest DecodeWorkshopRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析动态配置 mask 及每个置位的校验 DWORD。 */
[[nodiscard]] DynamicConfigurationRequest DecodeDynamicConfigurationRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C143～148 共用结果：状态、奖励、消耗和天数。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWorkshopResult(
    const application::WorkshopResult& result);

/** @brief 编码不含任何虚构资产变化的工坊失败结果。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWorkshopFailure();

/** @brief 编码 S2C151 原版科学工坊材料与功能开关配置。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWorkshopScienceState();

/** @brief
 * 依已发布政策编码合成卡、保护试剂与最低剩余日数；零配置表示合成未发布。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWorkshopScienceState(
    const application::WorkshopCompoundConfiguration& configuration);

/** @brief 只回送客户端请求且服务端已恢复的 section 4 和 7。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask);

/** @brief Same response; a non-empty `lobby_minigame_section` answers section 3
 * (`xxLMG.dch`). */
[[nodiscard]] std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section);

/** @brief 有完整44字节原活动区配置时，按置位顺序增加section6 xxEZM。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section,
    std::span<const std::uint8_t> event_zone_section);

/** @brief 增加同一已发布科学配置到bit7，保持3/4/6/7原顺序。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section,
    std::span<const std::uint8_t> event_zone_section,
    std::span<const std::uint8_t> science_section);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SHOP_WORKSHOP_MESSAGES_H_
