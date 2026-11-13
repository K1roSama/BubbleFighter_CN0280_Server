#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LIFE_ATTENDANCE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LIFE_ATTENDANCE_REPOSITORY_H_

/** @file life_attendance_repository.h
 * @brief MySQL出席、守护者历史及Life缺席结算适配器。
 */
#include "features/inventory/life_attendance.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 与该玩家其他资产事务串行执行出席和缺席结算。 */
class MySqlLifeAttendanceStore final : public application::LifeAttendanceStore {
 public:
  explicit MySqlLifeAttendanceStore(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  /** @brief 返回本数据库迁移产生的稳定纪元，不用连接URI或凭据标识日志。 */
  [[nodiscard]] application::ServiceResult<std::string> JournalEpoch();
  [[nodiscard]] std::optional<application::ServiceError> Record(
      domain::PlayerId player, std::span<const std::chrono::sys_days> days,
      std::chrono::system_clock::time_point now, bool settle) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LIFE_ATTENDANCE_REPOSITORY_H_
