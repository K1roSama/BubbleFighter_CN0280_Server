#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_CATALOG_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_CATALOG_H_

/**
 * @file shop_catalog.h
 * @brief 定义从不可变发布版本读取并校验商城目录的应用用例。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {

/** @brief 当前客户端已恢复的销售期限编码。 */
enum class ClientStockDurationType : std::uint8_t {
  kTimedDays = 1U,
  kPermanent = 9U,
};

/** @brief 一种客户端可选销售规格的不可变展示快照。 */
struct ShopPriceView final {
  domain::StockId stock_id;  ///< C2S809 回传的 stock ID。
  domain::ItemId item_id;    ///< 必须与所属商品卡片一致。
  ClientStockDurationType duration_type{
      ClientStockDurationType::kPermanent};  ///< 1 限时、9 永久。
  std::uint32_t duration_value{0U};  ///< 限时天数；永久固定 UINT32_MAX。
  std::uint32_t price{0U};           ///< 客户端同时显示并提交的整数价格。
};

/** @brief 商城中一个物品及其全部销售规格。 */
struct ShopCardView final {
  domain::ItemId item_id;             ///< 物品模板 ID。
  std::string item_name;              ///< 严格 UTF-8 显示名。
  std::uint32_t main_category{0U};    ///< 客户端主分类 ID。
  std::uint32_t subcategory{0U};      ///< 客户端子分类 ID。
  std::vector<ShopPriceView> prices;  ///< 非空；首项是卡片默认规格。
  bool is_bundle{false};  ///< 原版 DpStock kind=1；item_id 此时是 stockSetId。
};

/** @brief 主分类下的一项客户端子分类。 */
struct ShopSubcategoryView final {
  std::uint32_t category_id{0U};  ///< 客户端子分类 ID。
  std::string label;              ///< 严格 UTF-8 标签。
  std::uint32_t match_mode{
      0U};  ///< 已验证值 2 表示主类“全部”，0 表示精确子类。
};

/** @brief 商城主分类及其有序子分类。 */
struct ShopCategoryView final {
  std::uint32_t category_id{0U};                   ///< 客户端主分类 ID。
  std::string label;                               ///< 严格 UTF-8 标签。
  std::vector<ShopSubcategoryView> subcategories;  ///< 非空且 ID 唯一。
};

/** @brief 一次发布版本的两类目录及分类树。 */
struct PublishedShopCatalog final {
  std::uint64_t revision{0U};  ///< 非零、随内容发布固定的客户端缓存版本。
  std::vector<ShopCardView>
      standard_cards;  ///< 金币商品；S2C800 合并两种币种展示。
  std::vector<ShopCardView>
      cash_cards;  ///< 点券商品；也保留 S2C801 独立目录兼容。
  std::vector<ShopCategoryView> categories;  ///< 普通目录使用的有序分类树。
};

/** @brief 已发布目录的持久化读取端口。 */
class ShopCatalogReader {
 public:
  virtual ~ShopCatalogReader() = default;

  /** @brief 读取唯一 published 目录；尚未发布返回空值，存储故障返回错误。 */
  [[nodiscard]] virtual ServiceResult<std::optional<PublishedShopCatalog>>
  LoadPublished() = 0;
};

/** @brief 网关目录路由依赖的只读业务接口。 */
class ShopCatalogLoader {
 public:
  virtual ~ShopCatalogLoader() = default;

  /** @brief 返回已完成跨字段校验、可直接编码的不可变发布快照。 */
  [[nodiscard]] virtual ServiceResult<PublishedShopCatalog> Load() const = 0;
};

/**
 * @brief 在目录进入协议缓存前复核发布版本、ID、分类、UTF-8 和期限语义。
 * @note 本服务不读取 research 文件；正式 reader 必须从部署包或 MySQL 的
 * published 配置版本装载完整快照，使运行时输入可复现且可审计。
 */
class ShopCatalogService final : public ShopCatalogLoader {
 public:
  /** @brief 注入目录读取端口；其生命周期必须覆盖本服务。 */
  explicit ShopCatalogService(ShopCatalogReader& reader) noexcept;

  /** @brief 读取并严格校验当前发布目录；畸形发布内容按内部错误拒绝。 */
  [[nodiscard]] ServiceResult<PublishedShopCatalog> Load() const override;

 private:
  ShopCatalogReader& reader_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_SHOP_CATALOG_H_
