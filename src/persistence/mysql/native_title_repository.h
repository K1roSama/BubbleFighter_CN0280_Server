#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_NATIVE_TITLE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_NATIVE_TITLE_REPOSITORY_H_

/** @file native_title_repository.h @brief
 * 独立称号兑换扣勋章、发货、穿戴及不可变审计。 */
#include "application/system_clock.h"
#include "features/characters/native_title.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 短事务锁本人资料及库存，累计原事件与兑换扣减分开保存。 */
class MySqlNativeTitleRepository final : public application::NativeTitleStore {
 public:
  MySqlNativeTitleRepository(MySqlConnectionPool& pool,
                             const application::SystemClock& clock) noexcept;
  [[nodiscard]] application::ServiceResult<application::NativeTitleResult>
  Claim(domain::PlayerId player, std::uint32_t title,
        const domain::OperationId& operation) override;
  [[nodiscard]] application::ServiceResult<application::NativeTitleResult> Set(
      domain::PlayerId player, std::uint32_t title,
      std::uint32_t action) override;

 private:
  MySqlConnectionPool& pool_;
  const application::SystemClock& clock_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_NATIVE_TITLE_REPOSITORY_H_
