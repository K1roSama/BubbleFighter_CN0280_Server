#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_INVENTORY_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_INVENTORY_MESSAGES_H_

/**
 * @file inventory_messages.h
 * @brief 定义 0.2.80.1 商城购买与仓库装备入口的已验证小消息结构。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "features/lobby/player_menu_snapshot.h"

namespace bf::protocol {

/** @brief C2S801 普通商城目录请求。 */
struct ShopCatalogRequest final {
  std::uint64_t cached_revision;  ///< 客户端当前商品缓存版本。
  std::uint32_t selection;        ///< 页面或分类选择；业务层只接受已发布分类。
};

/** @brief C2S802 第二类商城目录请求。 */
struct CashShopCatalogRequest final {
  std::uint64_t cached_revision;  ///< 客户端当前第二类商品缓存版本。
};

/** @brief C2S803 空负载的完整仓库快照请求。 */
struct InventorySnapshotRequest final {};

/** @brief C2S827 空负载的点券余额刷新请求。 */
struct CashBalanceRequest final {};

/** @brief C2S804/C2S805 共用的装备或卸下物品请求体。 */
struct EquipmentChangeRequest final {
  std::uint32_t
      item_id;  ///< 客户端物品模板 ID；业务层必须复核本人有效库存和槽位。
};

/** @brief 原808只携带802行的有符号正原生键，不接收数量、玩家或SQL实例号。 */
struct ExpiredItemDiscardRequest final {
  std::uint32_t native_item_key{0U};
};

[[nodiscard]] ExpiredItemDiscardRequest DecodeExpiredItemDiscardRequest(
    std::span<const std::uint8_t> body);
/** @brief B72310先读库存键再读结果，零才会移除客户端该行。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExpiredItemDiscardResponse(
    std::uint32_t native_key, bool success);

/** @brief C2S809 普通商城购买请求。 */
struct PurchaseOfferRequest final {
  std::uint32_t stock_id;  ///< 销售规格 ID；不能当作物品模板 ID。
  std::uint32_t
      gift_flag;  ///< 普通购买为 0；非零赠送分支当前必须明确拒绝而不能扣款。
};

/** @brief S2C809 装备结果，零表示成功。 */
struct EquipmentAppliedResponse final {
  std::uint32_t result;   ///< 原版客户端约定 0 成功、非零失败。
  std::uint32_t item_id;  ///< 与 C2S804 对应的物品模板 ID。
};

/** @brief S2C810 卸下结果；客户端只读取一个 DWORD。 */
struct EquipmentRemovedResponse final {
  std::uint32_t result;  ///< 原版客户端约定 0 成功、非零失败。
};

/** @brief S2C814 商城购买结果。 */
struct PurchaseOfferResponse final {
  std::uint32_t stock_id;  ///< 与请求对应的销售规格 ID。
  std::uint32_t result;    ///< B72710 已确认 1 成功、0 失败。
};

/** @brief C2S832 选择武器预设；仓库 object_id 为 UINT32_MAX。 */
struct WeaponPresetRequest final {
  std::uint32_t object_id{0U};  ///< 原版场景对象标识，不用于推断玩家身份。
  std::uint8_t preset{0U};      ///< 选择页号，合法性由用例检查。
};

/** @brief 严格解析 C2S832 的对象 DWORD 与预设 BYTE。 */
[[nodiscard]] WeaponPresetRequest DecodeWeaponPresetRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C834 的页号 BYTE 与结果 DWORD，失败结果非零。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWeaponPresetResult(
    std::uint8_t preset, bool success);

/** @brief 编码 S2C835 的当前页及固定五条预设；未开放页面保持禁用。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWeaponPresetsResponse(
    const application::PlayerMenuSnapshot& snapshot);

/** @brief 严格解析 C2S801 的 u64 revision 和 u32 selection。 */
[[nodiscard]] ShopCatalogRequest DecodeShopCatalogRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S802 的单个 u64 revision。 */
[[nodiscard]] CashShopCatalogRequest DecodeCashShopCatalogRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格要求 C2S803 负载为空。 */
[[nodiscard]] InventorySnapshotRequest DecodeInventorySnapshotRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格要求 C2S827 负载为空。 */
[[nodiscard]] CashBalanceRequest DecodeCashBalanceRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S804/C2S805 的单个 u32 item_id。 */
[[nodiscard]] EquipmentChangeRequest DecodeEquipmentChangeRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S809 的 stock_id 和原始 gift DWORD。 */
[[nodiscard]] PurchaseOfferRequest DecodePurchaseOfferRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C809 的 result 和 item_id 两个 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentAppliedResponse(
    const EquipmentAppliedResponse& response);

/** @brief 编码 S2C810 的单个 result DWORD，不追加物品 ID。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquipmentRemovedResponse(
    const EquipmentRemovedResponse& response);

/** @brief 编码 S2C814 的 stock_id 和 result 两个 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodePurchaseOfferResponse(
    const PurchaseOfferResponse& response);

/**
 * @brief 编码 S2C802/S2C803 共用的完整有效库存 vector。
 * @note 永久物品写入 36500 天；限时物品以快照 observed_at
 * 计算，拒绝过期、溢出和无效值。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeInventorySnapshotResponse(
    const application::PlayerMenuSnapshot& snapshot);

/**
 * @brief 编码 S2C806 的玩家标识、当前角色与四个装备 vector。
 * @note 当前客户端只验证第一个物品 vector；其余三个 vector
 * 保持空，不猜测扩展配装语义。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEquippedItemsResponse(
    const application::PlayerMenuSnapshot& snapshot);

/** @brief 编码 S2C832 的状态、点券余额和保留字段。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCashBalanceResponse(
    const application::PlayerMenuSnapshot& snapshot);

/**
 * @brief 编码 S2C105 当前客户端完整读取的资料与 FighterInfo。
 * @note 已确认的团队/团队夺星战绩读取实际快照；其他统计分支继续待恢复。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodePlayerProfileResponse(
    const application::PlayerMenuSnapshot& snapshot);

/** @brief C2S119 的目标玩家与请求者标识；请求者必须与已认证身份相同。 */
struct FighterInfoRequest final {
  domain::PlayerId target;     ///< 被查看玩家。
  domain::PlayerId requester;  ///< 原生客户端自动附加的本人标识。
};

/** @brief C2S120 查询目标玩家的称号/勋章列表。 */
struct FighterTitleRequest final {
  domain::PlayerId target;  ///< 被查看玩家。
};

/** @brief 严格解析 9D1070 的两个 DWORD，拒绝零 ID 与尾随数据。 */
[[nodiscard]] FighterInfoRequest DecodeFighterInfoRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S120 的单个非零目标玩家 DWORD。 */
[[nodiscard]] FighterTitleRequest DecodeFighterTitleRequest(
    std::span<const std::uint8_t> body);

/** @brief S2C125：B955C0 的完整资料包；空指针只用于原生明确失败状态。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeFighterInfoResponse(
    const application::PlayerMenuSnapshot* snapshot,
    const application::PlayerMenuSnapshot* partner = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeFighterRecord(
    const application::PlayerMenuSnapshot& snapshot);

/** @brief 原生勋章累计进度；不是称号物品ID或持有标志。 */
struct FighterMedalProgress final {
  std::uint32_t medal_id{0U};  ///< BF_title.xml 的 mode*10000+medalid。
  std::uint32_t count{0U};     ///< 已确认的累计勋章次数，BE8200用它比较门槛。
};

/** @brief S2C126成功结构；调用方必须提供可信的勋章数据，禁止从称号库存推测。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeFighterTitleResponse(
    domain::PlayerId target, std::span<const FighterMedalProgress> medals);

/** @brief B78640识别的原生失败状态。 */
enum class FighterTitleFailure : std::uint32_t {
  kUnavailable = 0U,    ///< 勋章数据源尚不可用。
  kTargetMissing = 2U,  ///< 查询目标不存在。
};

/** @brief S2C126明确失败，不打开空的成功称号窗口。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeFighterTitleFailure(
    domain::PlayerId target, FighterTitleFailure failure);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_INVENTORY_INVENTORY_MESSAGES_H_
