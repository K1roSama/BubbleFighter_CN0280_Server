#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MYSTERY_SHOP_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MYSTERY_SHOP_REPOSITORY_H_

/**
 * @file mystery_shop_repository.h
 * @brief 定义神秘商店 MySQL 固定选品和资产事务适配器。
 */

#include "features/shop/mystery_shop_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 以玩家父行串行化选品、刷新和购买，并保存不可变成交快照与资产流水。 */
class MySqlMysteryShopRepository final
    : public application::MysteryShopRepository {
 public:
  /** @brief 注入生命周期覆盖全部调用的有界连接池。 */
  explicit MySqlMysteryShopRepository(MySqlConnectionPool& pool) noexcept;

  [[nodiscard]] application::ServiceResult<
      application::MysterySelectionSnapshot>
  LoadOrCreate(domain::PlayerId player_id, std::uint64_t rules_version_id,
               const application::MysteryShopServiceRules& rules,
               std::chrono::system_clock::time_point now) override;

  [[nodiscard]] application::ServiceResult<
      application::MysterySelectionSnapshot>
  Refresh(domain::OperationId operation_id, domain::PlayerId player_id,
          std::uint64_t expected_generation,
          const application::MysteryShopServiceRules& rules,
          std::uint64_t rules_version_id,
          std::chrono::system_clock::time_point now) override;

  [[nodiscard]] application::ServiceResult<application::MysteryPurchaseResult>
  Purchase(domain::OperationId operation_id, domain::PlayerId player_id,
           std::uint64_t expected_generation, domain::StockId stock_id,
           std::uint64_t rules_version_id,
           const application::MysteryShopServiceRules& rules,
           std::chrono::system_clock::time_point now) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MYSTERY_SHOP_REPOSITORY_H_
