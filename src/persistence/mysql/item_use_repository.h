#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_USE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_USE_REPOSITORY_H_

/**
 * @file item_use_repository.h
 * @brief 提供角色染色、宠物成长与道具消耗的 MySQL 原子事务。
 */
#include "features/inventory/item_use_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 使用连接池锁定玩家资料、宠物与消耗品资产。 */
class MySqlItemUseRepository final : public application::ItemUseRepository {
 public:
  /** @brief 借用生命周期覆盖本仓储的连接池。 */
  explicit MySqlItemUseRepository(MySqlConnectionPool& pool) noexcept;
  [[nodiscard]] application::ServiceResult<
      application::ExpiredItemDiscardResult>
  DiscardExpired(const application::ExpiredItemDiscardCommand& command,
                 std::chrono::system_clock::time_point now) override;
  /** @copydoc application::ItemUseRepository::replenish_life */
  [[nodiscard]] application::ServiceResult<application::UseItemResult>
  ReplenishLife(domain::PlayerId player,
                std::chrono::system_clock::time_point now) override;
  /** @copydoc application::ItemUseRepository::rename */
  [[nodiscard]] application::ServiceResult<application::UseItemResult> Rename(
      domain::PlayerId player, const domain::Nickname& nickname,
      std::chrono::system_clock::time_point now) override;
  /** @copydoc application::ItemUseRepository::dye */
  [[nodiscard]] application::ServiceResult<application::UseItemResult> Dye(
      domain::PlayerId player, domain::ItemId item, std::uint16_t color,
      std::chrono::system_clock::time_point now) override;
  /** @copydoc application::ItemUseRepository::grow_pet */
  [[nodiscard]] application::ServiceResult<application::UseItemResult> GrowPet(
      const application::PetGrowthCommand& command,
      std::chrono::system_clock::time_point now) override;
  /** @copydoc application::ItemUseRepository::expand_clan */
  [[nodiscard]] application::ServiceResult<application::UseItemResult>
  ExpandClan(domain::PlayerId player,
             std::chrono::system_clock::time_point now) override;
  [[nodiscard]] application::ServiceResult<application::UseItemResult>
  InstantRespawn(const application::UseItemCommand& command,
                 std::chrono::system_clock::time_point now) override;
  [[nodiscard]] application::ServiceResult<application::MegaphoneReceipt>
  Megaphone(const application::MegaphoneCommand& command,
            std::chrono::system_clock::time_point now) override;
  [[nodiscard]] application::ServiceResult<
      std::vector<application::MegaphoneReceipt>>
  PendingMegaphones() override;
  [[nodiscard]] std::optional<application::ServiceError> AcknowledgeMegaphone(
      std::string_view id) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ITEM_USE_REPOSITORY_H_
