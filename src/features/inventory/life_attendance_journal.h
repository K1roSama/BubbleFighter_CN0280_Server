#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_JOURNAL_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_JOURNAL_H_

/** @file life_attendance_journal.h
 * @brief 出席观察的耐久日志端口，数据库确认前不能删除。
 */
#include <compare>
#include <vector>

#include "features/inventory/life_attendance.h"

namespace bf::application {
/** @brief 已认证连接观察到的玩家与自然日，不包含密码或票据。 */
struct LifeAttendanceObservation final {
  domain::PlayerId player;
  std::chrono::sys_days day;
  auto operator<=>(const LifeAttendanceObservation&) const = default;
};

/** @brief 可在SQL不可用时独立保存和恢复出席；保存成功代表完成耐久写入。 */
class LifeAttendanceJournal {
 public:
  virtual ~LifeAttendanceJournal() = default;
  /** @brief 返回全部尚未得到SQL确认的日期。 */
  [[nodiscard]] virtual ServiceResult<std::vector<LifeAttendanceObservation>>
  Pending() = 0;
  /** @brief 幂等增加观察并耐久提交；失败必须保留已有文件。 */
  [[nodiscard]] virtual std::optional<ServiceError> Append(
      std::span<const LifeAttendanceObservation> rows) = 0;
  /** @brief 仅在SQL提交确认后清除日期；失败可安全重放原日期。 */
  [[nodiscard]] virtual std::optional<ServiceError> Acknowledge(
      std::span<const LifeAttendanceObservation> rows) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_JOURNAL_H_
