#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_H_

/** @file life_attendance.h
 * @brief 认证会话出席及已结束日期的Life结算，独立于每日奖励。
 */
#include <chrono>
#include <optional>
#include <span>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {
/** @brief The existing service calendar uses UTC+08:00 natural days. */
[[nodiscard]] inline std::chrono::sys_days LifeCalendarDay(
    std::chrono::system_clock::time_point at) noexcept {
  return std::chrono::floor<std::chrono::days>(at + std::chrono::hours{8});
}

/** @brief 只接收认证连接产生的出席观察，不接受客户端自报或他人资料查询。 */
class LifeAttendanceStore {
 public:
  virtual ~LifeAttendanceStore() = default;
  /** @brief Record observations and optionally settle all closed days in the
   * same transaction.
   * @note Idempotent after an unknown commit. Never derives attendance from
   * wallets or profile reads.
   */
  [[nodiscard]] virtual std::optional<ServiceError> Record(
      domain::PlayerId player, std::span<const std::chrono::sys_days> days,
      std::chrono::system_clock::time_point now, bool settle) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_H_
