#include "operations/database_task_pool.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = retryable};
}

}  // namespace

class DatabaseTaskPool::Impl final {
 public:
  Impl(std::size_t worker_count, std::size_t queue_capacity,
       application::EventLogger& logger)
      : kQueueCapacity(queue_capacity), logger_(logger) {
    if (worker_count == 0U || queue_capacity == 0U || worker_count > 1024U) {
      throw std::invalid_argument{"invalid database task pool limits"};
    }
    workers_.reserve(worker_count);
    try {
      for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] { WorkerLoop(); });
      }
    } catch (...) {
      {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
        stopping_ = true;
      }
      work_available_.notify_all();
      JoinWorkers();
      throw;
    }
  }

  ~Impl() { DrainAndStop(); }

  std::optional<application::ServiceError> Post(std::function<void()> task) {
    if (!task) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "数据库任务不能为空");
    }
    {
      std::scoped_lock lock{mutex_};
      if (!accepting_) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "数据库任务池正在停止");
      }
      if (tasks_.size() >= kQueueCapacity) {
        return MakeError(application::ErrorCode::kOverloaded,
                         "数据库任务队列已满", true);
      }
      tasks_.push_back(std::move(task));
    }
    work_available_.notify_one();
    return std::nullopt;
  }

  void DrainAndStop() noexcept {
    {
      std::scoped_lock lock{mutex_};
      if (stopping_) {
        return;
      }
      accepting_ = false;
      stopping_ = true;
    }
    work_available_.notify_all();
    JoinWorkers();
  }

  [[nodiscard]] std::size_t WorkerCount() const noexcept {
    return workers_.size();
  }

  [[nodiscard]] std::size_t QueuedTaskCount() const noexcept {
    std::scoped_lock lock{mutex_};
    return tasks_.size();
  }

 private:
  void WorkerLoop() noexcept {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock{mutex_};
        work_available_.wait(lock,
                             [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          if (stopping_) {
            return;
          }
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (...) {
        logger_.Log(application::LogLevel::kError,
                    "database task escaped its completion/error boundary");
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

  const std::size_t kQueueCapacity;
  application::EventLogger& logger_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool accepting_{true};
  bool stopping_{false};
};

DatabaseTaskPool::DatabaseTaskPool(std::size_t worker_count,
                                   std::size_t queue_capacity,
                                   application::EventLogger& logger)
    : impl_(std::make_unique<Impl>(worker_count, queue_capacity, logger)) {}

DatabaseTaskPool::~DatabaseTaskPool() = default;

std::optional<application::ServiceError> DatabaseTaskPool::Post(
    std::function<void()> task) {
  return impl_->Post(std::move(task));
}

void DatabaseTaskPool::DrainAndStop() noexcept { impl_->DrainAndStop(); }

std::size_t DatabaseTaskPool::WorkerCount() const noexcept {
  return impl_->WorkerCount();
}

std::size_t DatabaseTaskPool::QueuedTaskCount() const noexcept {
  return impl_->QueuedTaskCount();
}

}  // namespace bf::infrastructure::mysql
