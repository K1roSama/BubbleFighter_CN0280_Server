#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SERVICE_RULES_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SERVICE_RULES_REPOSITORY_H_

/**
 * @file service_rules_repository.h
 * @brief 定义扩展业务规则 published JSON 的 MySQL 读取器。
 */

#include <mutex>
#include <optional>

#include "features/configuration/service_rules.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从唯一 published service_rules
 * 配置重建强类型视图，并在首次成功读取后按值缓存。
 * @note
 * 当前发布模型拒绝原地热替换；部署新版本需要显式重启，因此缓存不会掩盖合法更新。
 */
class MySqlServiceRulesReader final : public application::ServiceRulesReader {
 public:
  /** @brief 注入生命周期长于本读取器的有界连接池。 */
  explicit MySqlServiceRulesReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 读取、严格解析并返回唯一已发布规则；同时存在多个版本按损坏拒绝。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::PublishedServiceRules>>
  LoadPublished() override;

 private:
  MySqlConnectionPool& pool_;
  std::mutex cache_mutex_;
  std::optional<application::PublishedServiceRules> cache_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SERVICE_RULES_REPOSITORY_H_
