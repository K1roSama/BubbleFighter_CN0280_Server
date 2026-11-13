/** @file life_attendance_tracker.cpp
 * @brief Connection-bound attendance, closed-date retry and bounded background
 * scheduling.
 */
#include "features/inventory/life_attendance_tracker.h"

#include <algorithm>
#include <vector>

namespace bf::runtime {
namespace {
application::ServiceError DurabilityError() {
  return {.code = application::ErrorCode::kStorageUnavailable,
          .safe_message = "出席观察尚未确认耐久保存，已停止Life结算"};
}
}  // namespace

LifeAttendanceTracker::LifeAttendanceTracker(
    application::LifeAttendanceStore& store,
    const application::SystemClock& clock, application::EventLogger& logger,
    application::LifeAttendanceJournal* journal) noexcept
    : store_(store), clock_(clock), logger_(logger), journal_(journal) {}

bool LifeAttendanceTracker::Healthy() const noexcept {
  return !durability_failed_.load();
}

void LifeAttendanceTracker::MarkDurabilityFailure() noexcept {
  durability_failed_.store(true);
  logger_.Log(application::LogLevel::kCritical,
              "Life durable observation failed; settlement stopped");
}

std::optional<application::ServiceError> LifeAttendanceTracker::Recover() {
  if (!journal_) return std::nullopt;
  auto loaded = journal_->Pending();
  if (const auto* error = std::get_if<application::ServiceError>(&loaded)) {
    durability_failed_.store(true);
    return *error;
  }
  {
    std::lock_guard lock{mutex_};
    for (const auto& row :
         std::get<std::vector<application::LifeAttendanceObservation>>(loaded))
      pending_[row.player].insert(row.day);
  }
  Flush();
  std::lock_guard lock{mutex_};
  if (!pending_.empty() || !Healthy())
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "出席日志尚未恢复，暂不开放玩家登录",
        .is_retryable = true};
  return std::nullopt;
}

std::optional<application::ServiceError> LifeAttendanceTracker::StagePending() {
  try {
    if (!Healthy()) return DurabilityError();
    if (!journal_) return std::nullopt;
    std::vector<application::LifeAttendanceObservation> observations;
    {
      std::lock_guard lock{mutex_};
      for (const auto& [player, days] : pending_)
        for (const auto day : days) observations.push_back({player, day});
    }
    if (auto error = journal_->Append(observations)) {
      MarkDurabilityFailure();
      return error;
    }
    if (!Healthy()) return DurabilityError();
    return std::nullopt;
  } catch (...) {
    MarkDurabilityFailure();
    return DurabilityError();
  }
}

void LifeAttendanceTracker::ObserveLocked(domain::PlayerId player,
                                          std::chrono::sys_days first,
                                          std::chrono::sys_days last) {
  for (auto day = first; day <= last; day += std::chrono::days{1})
    if (!confirmed_[player].contains(day)) pending_[player].insert(day);
}

std::optional<application::ServiceError> LifeAttendanceTracker::Connect(
    SessionHandle handle, domain::PlayerId player,
    std::chrono::system_clock::time_point at) {
  try {
    if (!Healthy()) return DurabilityError();
    const auto day = application::LifeCalendarDay(at);
    {
      std::lock_guard lock{mutex_};
      if (!handle.IsValid() || !player.IsValid() || online_.contains(handle))
        return std::nullopt;
      online_.emplace(handle, Presence{player, day});
      ObserveLocked(player, day, day);
    }
    return StagePending();
  } catch (...) {
    MarkDurabilityFailure();
    return DurabilityError();
  }
}

std::optional<application::ServiceError> LifeAttendanceTracker::Disconnect(
    SessionHandle handle, std::chrono::system_clock::time_point at) {
  try {
    const auto day = application::LifeCalendarDay(at);
    {
      std::lock_guard lock{mutex_};
      const auto found = online_.find(handle);
      if (found == online_.end()) return std::nullopt;
      ObserveLocked(found->second.player, found->second.last_day, day);
      const auto player = found->second.player;
      online_.erase(found);
      if (std::ranges::none_of(online_, [&](const auto& entry) {
            return entry.second.player == player;
          }))
        confirmed_.erase(player);
    }
    return StagePending();
  } catch (...) {
    MarkDurabilityFailure();
    return DurabilityError();
  }
}

std::optional<application::ServiceError> LifeAttendanceTracker::Acknowledge(
    domain::PlayerId player, std::span<const std::chrono::sys_days> days) {
  try {
    if (journal_) {
      std::vector<application::LifeAttendanceObservation> observations;
      for (const auto day : days) observations.push_back({player, day});
      if (auto error = journal_->Acknowledge(observations)) {
        durability_failed_.store(true);
        logger_.Log(
            application::LogLevel::kCritical,
            "Life durable acknowledgement failed; observations retained");
        return error;
      }
    }
    std::lock_guard lock{mutex_};
    const bool connected = std::ranges::any_of(online_, [&](const auto& entry) {
      return entry.second.player == player;
    });
    for (const auto day : days) {
      if (connected) confirmed_[player].insert(day);
      if (auto found = pending_.find(player); found != pending_.end()) {
        found->second.erase(day);
        if (found->second.empty()) pending_.erase(found);
      }
    }
    return std::nullopt;
  } catch (...) {
    MarkDurabilityFailure();
    return DurabilityError();
  }
}

std::optional<application::ServiceError> LifeAttendanceTracker::SettleLogin(
    domain::PlayerId player,
    std::chrono::system_clock::time_point observed_at) {
  const auto now = observed_at;
  const auto today = application::LifeCalendarDay(now);
  std::vector<std::chrono::sys_days> days;
  {
    std::lock_guard lock{mutex_};
    // Pending previous-session observations must be included before absence
    // settlement.
    ObserveLocked(player, today, today);
    const auto found = pending_.find(player);
    if (found != pending_.end())
      for (const auto day : found->second)
        if (day <= today) days.push_back(day);
    if (std::ranges::find(days, today) == days.end()) days.push_back(today);
  }
  if (auto error = StagePending()) return error;
  if (auto error = store_.Record(player, days, now, true)) return error;
  return Acknowledge(player, days);
}

void LifeAttendanceTracker::Poll(
    application::BackgroundTaskExecutor& executor) {
  const auto today = application::LifeCalendarDay(clock_.Now());
  {
    std::lock_guard lock{mutex_};
    for (auto& [handle, presence] : online_) {
      static_cast<void>(handle);
      ObserveLocked(presence.player, presence.last_day, today);
      presence.last_day = std::max(presence.last_day, today);
    }
    for (auto& [player, days] : confirmed_) {
      static_cast<void>(player);
      std::erase_if(
          days, [&](auto day) { return day < today - std::chrono::days{2}; });
    }
    if (pending_.empty()) return;
  }
  // The local journal must not depend on database worker availability or SQL
  // timeout.
  if (StagePending()) return;
  if (flushing_.exchange(true)) return;
  const auto error = executor.Post([this] {
    try {
      Flush();
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "Life attendance writer failed; observations retained");
    }
    flushing_.store(false);
  });
  if (error) {
    flushing_.store(false);
    logger_.Log(application::LogLevel::kWarning,
                "Life attendance writer busy; observations retained");
  }
}

void LifeAttendanceTracker::Flush() {
  if (StagePending()) return;
  std::map<domain::PlayerId, std::set<std::chrono::sys_days>> work;
  {
    std::lock_guard lock{mutex_};
    work = pending_;
  }
  for (const auto& [player, known] : work) {
    const std::vector<std::chrono::sys_days> days{known.begin(), known.end()};
    if (store_.Record(player, days, clock_.Now(), false))
      logger_.Log(
          application::LogLevel::kWarning,
          "Life attendance persistence unavailable; observations retained");
    else if (Acknowledge(player, days))
      return;
  }
}
}  // namespace bf::runtime
