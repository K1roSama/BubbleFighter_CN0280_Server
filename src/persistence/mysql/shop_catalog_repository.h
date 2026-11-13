#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SHOP_CATALOG_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SHOP_CATALOG_REPOSITORY_H_

/**
 * @file shop_catalog_repository.h
 * @brief 定义不可变已发布商城目录的 MySQL 读取适配器。
 */

#include <mutex>
#include <optional>

#include "features/shop/shop_catalog.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从 published catalog 配置及其关系表重建原版客户端目录视图。
 * @note 分类顺序来自不可变 JSON
 * 发布载荷；商品、规格和可发放门禁来自同版本关系行。当前
 *       发布模型只允许一个不可变 published
 * 版本，因此首次完整读取后按值缓存，避免每个
 *       客户端目录请求重复扫描数千条规格；未来支持热切换时必须显式增加版本失效协议。
 */
class MySqlShopCatalogReader final : public application::ShopCatalogReader {
 public:
  /** @brief 注入生命周期长于本 reader 的连接池。 */
  explicit MySqlShopCatalogReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 首次在 repeatable-read 只读事务中读取，之后返回线程安全缓存快照。
   */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::PublishedShopCatalog>>
  LoadPublished() override;

 private:
  MySqlConnectionPool& pool_;
  std::mutex cache_mutex_;  ///< 串行首次加载并保护不可变已发布目录缓存。
  std::optional<application::PublishedShopCatalog>
      cache_;  ///< 首次成功读取后的完整值快照。
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SHOP_CATALOG_REPOSITORY_H_
