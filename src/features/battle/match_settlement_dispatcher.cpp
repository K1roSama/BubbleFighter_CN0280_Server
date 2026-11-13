#include "features/battle/match_settlement_dispatcher.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace bf::runtime {
namespace {

application::ServiceError InvalidStateError() {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidState,
      .safe_message = "结算调度器未运行或已经停止",
      .correlation_id = {},
      .is_retryable = false,
  };
}

application::ServiceError OverloadError() {
  return application::ServiceError{
      .code = application::ErrorCode::kOverloaded,
      .safe_message = "结算任务队列已满，命令仍由调用方持有",
      .correlation_id = {},
      .is_retryable = true,
  };
}

application::ServiceError AllocationError() {
  return application::ServiceError{
      .code = application::ErrorCode::kInternal,
      .safe_message = "复制结算命令失败",
      .correlation_id = {},
      .is_retryable = true,
  };
}

application::ServiceError ExecutorExceptionError() {
  return application::ServiceError{
      .code = application::ErrorCode::kStorageUnavailable,
      .safe_message = "结算执行器抛出未处理异常",
      .correlation_id = {},
      .is_retryable = true,
  };
}

application::ServiceError WorkerStartError() {
  return application::ServiceError{
      .code = application::ErrorCode::kInternal,
      .safe_message = "无法创建结算数据库工作线程",
      .correlation_id = {},
      .is_retryable = false,
  };
}

}  // namespace

class MatchSettlementDispatcher::Impl final {
 public:
  Impl(MatchSettlementDispatcherOptions options,
       const application::MatchSettlementExecutor& executor,
       MatchSettlementCompletionSink& sink, application::EventLogger& logger)
      : options_(options), executor_(executor), sink_(sink), logger_(logger) {
    if (options_.worker_count == 0U || options_.queue_capacity == 0U ||
        options_.queue_capacity < options_.worker_count) {
      throw std::invalid_argument(
          "invalid match settlement dispatcher options");
    }
  }

  ~Impl() { Shutdown(); }

  std::optional<application::ServiceError> Start() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    {
      std::lock_guard lock(mutex_);
      if (state_ != MatchSettlementDispatcherState::kCreated) {
        return InvalidStateError();
      }
      accepting_ = true;
      state_ = MatchSettlementDispatcherState::kRunning;
    }

    try {
      workers_.reserve(options_.worker_count);
      for (std::size_t index = 0U; index < options_.worker_count; ++index) {
        workers_.emplace_back([this] { Consume(); });
      }
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        state_ = MatchSettlementDispatcherState::kStopping;
      }
      condition_.notify_all();
      JoinWorkers();
      {
        std::lock_guard lock(mutex_);
        state_ = MatchSettlementDispatcherState::kStopped;
      }
      logger_.Log(application::LogLevel::kCritical,
                  "match settlement dispatcher failed to start workers");
      return WorkerStartError();
    }

    logger_.Log(application::LogLevel::kDebug,
                "match settlement dispatcher started");
    return std::nullopt;
  }

  std::optional<application::ServiceError> Submit(
      const application::MatchSettlementCommand& command) {
    try {
      application::MatchSettlementCommand owned_command = command;
      {
        std::lock_guard lock(mutex_);
        if (state_ != MatchSettlementDispatcherState::kRunning || !accepting_) {
          return InvalidStateError();
        }
        if (queue_.size() >= options_.queue_capacity) {
          ++metrics_.rejected_overload;
          return OverloadError();
        }
        queue_.push_back(std::move(owned_command));
        ++metrics_.accepted;
      }
      condition_.notify_one();
      return std::nullopt;
    } catch (const std::exception&) {
      logger_.Log(
          application::LogLevel::kError,
          "match settlement dispatcher could not own submitted command");
      return AllocationError();
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "match settlement dispatcher caught unknown submit failure");
      return AllocationError();
    }
  }

  void Shutdown() noexcept {
    try {
      std::lock_guard lifecycle_lock(lifecycle_mutex_);
      {
        std::lock_guard lock(mutex_);
        if (state_ == MatchSettlementDispatcherState::kStopped) {
          return;
        }
        if (state_ == MatchSettlementDispatcherState::kCreated) {
          state_ = MatchSettlementDispatcherState::kStopped;
          accepting_ = false;
          return;
        }
        accepting_ = false;
        state_ = MatchSettlementDispatcherState::kStopping;
      }
      condition_.notify_all();
      JoinWorkers();
      {
        std::lock_guard lock(mutex_);
        state_ = MatchSettlementDispatcherState::kStopped;
      }
      logger_.Log(application::LogLevel::kDebug,
                  "match settlement dispatcher stopped");
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "match settlement dispatcher shutdown caught an exception");
    }
  }

  MatchSettlementDispatcherState State() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
  }

  MatchSettlementDispatcherMetrics Metrics() const noexcept {
    std::lock_guard lock(mutex_);
    auto snapshot = metrics_;
    snapshot.queued = queue_.size();
    snapshot.active = active_;
    return snapshot;
  }

 private:
  void Consume() noexcept {
    while (true) {
      auto command = TakeNext();
      if (!command.has_value()) {
        return;
      }
      Execute(std::move(*command));
    }
  }

  std::optional<application::MatchSettlementCommand> TakeNext() noexcept {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return !queue_.empty() || !accepting_; });
    if (queue_.empty()) {
      return std::nullopt;
    }
    auto command = std::move(queue_.front());
    queue_.pop_front();
    ++active_;
    return std::optional<application::MatchSettlementCommand>{
        std::move(command)};
  }

  void Execute(application::MatchSettlementCommand command) noexcept {
    application::ServiceResult<application::MatchSettlementResult> result =
        ExecutorExceptionError();
    try {
      result = executor_.Settle(command);
    } catch (const std::exception&) {
      logger_.Log(application::LogLevel::kError,
                  "match settlement executor threw an exception");
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "match settlement executor threw an unknown exception");
    }

    const bool failed =
        std::holds_alternative<application::ServiceError>(result);
    sink_.Publish(MatchSettlementCompletion{.command = std::move(command),
                                            .result = std::move(result)});
    {
      std::lock_guard lock(mutex_);
      --active_;
      ++metrics_.completed;
      if (failed) {
        ++metrics_.failed;
      }
    }
  }

  void JoinWorkers() noexcept {
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  MatchSettlementDispatcherOptions options_;
  const application::MatchSettlementExecutor& executor_;
  MatchSettlementCompletionSink& sink_;
  application::EventLogger& logger_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<application::MatchSettlementCommand> queue_;
  std::vector<std::thread> workers_;
  MatchSettlementDispatcherState state_{
      MatchSettlementDispatcherState::kCreated};
  MatchSettlementDispatcherMetrics metrics_;
  std::size_t active_{0U};
  bool accepting_{false};
};

MatchSettlementDispatcher::MatchSettlementDispatcher(
    MatchSettlementDispatcherOptions options,
    const application::MatchSettlementExecutor& executor,
    MatchSettlementCompletionSink& sink, application::EventLogger& logger)
    : impl_(std::make_unique<Impl>(options, executor, sink, logger)) {}

MatchSettlementDispatcher::~MatchSettlementDispatcher() { Shutdown(); }

std::optional<application::ServiceError> MatchSettlementDispatcher::Start() {
  return impl_->Start();
}

std::optional<application::ServiceError> MatchSettlementDispatcher::Submit(
    const application::MatchSettlementCommand& command) {
  return impl_->Submit(command);
}

void MatchSettlementDispatcher::Shutdown() noexcept {
  if (impl_) {
    impl_->Shutdown();
  }
}

MatchSettlementDispatcherState MatchSettlementDispatcher::State()
    const noexcept {
  return impl_->State();
}

MatchSettlementDispatcherMetrics MatchSettlementDispatcher::Metrics()
    const noexcept {
  return impl_->Metrics();
}

std::string_view MatchSettlementDispatcherStateName(
    MatchSettlementDispatcherState state) noexcept {
  switch (state) {
    case MatchSettlementDispatcherState::kCreated:
      return "created";
    case MatchSettlementDispatcherState::kRunning:
      return "running";
    case MatchSettlementDispatcherState::kStopping:
      return "stopping";
    case MatchSettlementDispatcherState::kStopped:
      return "stopped";
  }
  return "unknown";
}

}  // namespace bf::runtime
