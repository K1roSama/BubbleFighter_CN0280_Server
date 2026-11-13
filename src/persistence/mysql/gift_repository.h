#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_GIFT_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_GIFT_REPOSITORY_H_

/** @file gift_repository.h @brief MySQL 礼物与兑换券资产仓储。 */
#include "features/gifts/gift_store.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 以行锁和耐久回执实现礼物与兑换券资产事务。 */
class MySqlGiftStore final : public application::GiftStore {
 public:
  explicit MySqlGiftStore(MySqlConnectionPool& pool) noexcept : pool_(pool) {}

  [[nodiscard]] application::ServiceResult<application::GiftPage> List(
      domain::PlayerId player, bool sent, std::uint32_t offset,
      std::uint32_t count) override;
  [[nodiscard]] application::ServiceResult<application::GiftView> Send(
      const application::SendGiftCommand& command) override;
  [[nodiscard]] application::ServiceResult<application::GiftView> Detail(
      domain::PlayerId player, std::uint32_t gift) override;
  [[nodiscard]] application::ServiceResult<application::GiftActionResult> Act(
      domain::PlayerId player, std::uint32_t gift,
      application::GiftAction action, const std::string& thanks,
      std::uint16_t thank_kind = 0U) override;
  [[nodiscard]] application::ServiceResult<application::CouponResult> Redeem(
      domain::PlayerId player, const std::string& code) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_GIFT_REPOSITORY_H_
