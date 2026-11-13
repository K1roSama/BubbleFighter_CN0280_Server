#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_COUPLE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_COUPLE_REPOSITORY_H_

/**
 * @file couple_repository.h
 * @brief MySQL 情侣关系及双方戒指事务。
 */
#include "features/social/couple_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 按编号锁定双方资料并维护完整情侣关系。 */
class MySqlCoupleRepository final : public application::CoupleStore {
 public:
  explicit MySqlCoupleRepository(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  [[nodiscard]] std::optional<application::ServiceError> CheckInvitation(
      domain::PlayerId actor, domain::PlayerId partner) override;
  [[nodiscard]] application::ServiceResult<application::CoupleChange> Form(
      const application::CoupleFormCommand& command) override;
  [[nodiscard]] application::ServiceResult<application::CoupleChange> Breakup(
      domain::PlayerId actor, domain::PlayerId expected_partner) override;
  [[nodiscard]] application::ServiceResult<application::CoupleChange>
  ChangeRing(const application::CoupleFormCommand& command) override;

 private:
  application::ServiceResult<application::CoupleChange> Apply(
      const application::CoupleFormCommand& command, bool change);
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_COUPLE_REPOSITORY_H_
