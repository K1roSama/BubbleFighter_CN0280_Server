#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_TRACKER_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_TRACKER_H_

/** @file life_attendance_tracker.h
 * @brief 记录认证连接的在线日期，不在socket线程执行数据库操作。
 */
#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include "application/background_task_executor.h"
#include "application/event_logger.h"
#include "application/system_clock.h"
#include "features/inventory/life_attendance.h"
#include "features/inventory/life_attendance_journal.h"
#include "sessions/session_registry.h"

namespace bf::runtime {
/** @brief SQL暂时失败时独立耐久保存出席，重启先恢复再开放玩家登录。
 * @note Owned by the composition root until all accepted database tasks are
 * drained.
 */
class LifeAttendanceTracker final {
 public:
  LifeAttendanceTracker(
      application::LifeAttendanceStore& store,
      const application::SystemClock& clock, application::EventLogger& logger,
      application::LifeAttendanceJournal* journal = nullptr) noexcept;
  /** @brief 监听启动前恢复并提交耐久日志；失败不得开放缺席结算。 */
  [[nodiscard]] std::optional<application::ServiceError> Recover();
  /** @brief 本地耐久写入异常后要求停止服务，不继续依赖内存记录。 */
  [[nodiscard]] bool Healthy() const noexcept;
  /** @brief
   * 认证会话登记并同步确认本地日志；不执行SQL，保存失败不得发布登录成功。 */
  [[nodiscard]] std::optional<application::ServiceError> Connect(
      SessionHandle handle, domain::PlayerId player,
      std::chrono::system_clock::time_point at);
  /** @brief
   * 按进入关闭回调时的时刻登记并保存日期；返回前不等待下一次轮询或数据库清理。
   */
  [[nodiscard]] std::optional<application::ServiceError> Disconnect(
      SessionHandle handle, std::chrono::system_clock::time_point at);
  /** @brief Database-worker operation required before publishing successful
   * login. */
  [[nodiscard]] std::optional<application::ServiceError> SettleLogin(
      domain::PlayerId player,
      std::chrono::system_clock::time_point observed_at);
  /** @brief Observe idle connections too; schedule at most one background
   * writer. */
  void Poll(application::BackgroundTaskExecutor& executor);
  /** @brief Drain known observations; also called after socket/database
   * shutdown, before pool stop.
   */
  void Flush();

 private:
  /** @brief 精确连接的认证身份与最近一次在线自然日。 */
  struct Presence {
    domain::PlayerId player;
    std::chrono::sys_days last_day;
  };

  void ObserveLocked(domain::PlayerId player, std::chrono::sys_days first,
                     std::chrono::sys_days last);
  [[nodiscard]] std::optional<application::ServiceError> Acknowledge(
      domain::PlayerId player, std::span<const std::chrono::sys_days> days);
  [[nodiscard]] std::optional<application::ServiceError> StagePending();
  void MarkDurabilityFailure() noexcept;
  application::LifeAttendanceStore& store_;
  const application::SystemClock& clock_;
  application::EventLogger& logger_;
  application::LifeAttendanceJournal* journal_;
  std::mutex mutex_;
  std::map<SessionHandle, Presence> online_;
  std::map<domain::PlayerId, std::set<std::chrono::sys_days>> pending_;
  std::map<domain::PlayerId, std::set<std::chrono::sys_days>> confirmed_;
  std::atomic<bool> flushing_{false};
  std::atomic<bool> durability_failed_{false};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_LIFE_ATTENDANCE_TRACKER_H_
