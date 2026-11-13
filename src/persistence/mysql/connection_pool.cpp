#include "persistence/mysql/connection_pool.h"

#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "persistence/mysql/sql_connection.h"

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

std::unique_ptr<Connection> OpenConnection(ConnectionSettings settings,
                                           std::uint32_t query_timeout_ms) {
  settings.query_timeout_ms = query_timeout_ms;
  return std::make_unique<Connection>(settings);
}

}  // namespace

class MySqlConnectionPool::SharedState final {
 public:
  explicit SharedState(ConnectionPoolOptions value)
      : options(std::move(value)) {}

  ConnectionPoolOptions options;
  mutable std::mutex mutex;
  std::condition_variable available_changed;
  std::vector<std::unique_ptr<bf::infrastructure::mysql::Connection>>
      connections;
  // 启动期预分配固定槽位；归还租约只覆盖元素，禁止在 noexcept
  // 析构路径申请内存。
  std::vector<std::size_t> available_indices;
  std::size_t available_count{0U};
  bool stopping{false};
};

MySqlConnectionPool::Lease::Lease(std::shared_ptr<SharedState> state,
                                  std::size_t index) noexcept
    : state_(std::move(state)), index_(index), active_(true) {}

MySqlConnectionPool::Lease::~Lease() { Release(); }

MySqlConnectionPool::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)),
      index_(other.index_),
      active_(other.active_) {
  other.active_ = false;
}

MySqlConnectionPool::Lease& MySqlConnectionPool::Lease::operator=(
    Lease&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::move(other.state_);
    index_ = other.index_;
    active_ = other.active_;
    other.active_ = false;
  }
  return *this;
}

bf::infrastructure::mysql::Connection& MySqlConnectionPool::Lease::Connection()
    const {
  if (!active_ || !state_) {
    throw std::logic_error{"MySQL connection lease is empty"};
  }
  std::scoped_lock lock{state_->mutex};
  if (index_ >= state_->connections.size() || !state_->connections[index_]) {
    throw std::logic_error{"MySQL connection lease has no connection"};
  }
  return *state_->connections[index_];
}

bool MySqlConnectionPool::Lease::Active() const noexcept {
  return active_ && state_ != nullptr;
}

void MySqlConnectionPool::Lease::Release() noexcept {
  if (!active_ || !state_) {
    return;
  }
  {
    std::scoped_lock lock{state_->mutex};
    state_->available_indices[state_->available_count] = index_;
    ++state_->available_count;
  }
  active_ = false;
  state_->available_changed.notify_one();
  state_.reset();
}

application::ServiceResult<std::unique_ptr<MySqlConnectionPool>>
MySqlConnectionPool::Create(ConnectionPoolOptions options) {
  if (options.connection.name.empty() || options.size == 0U ||
      options.acquire_timeout.count() <= 0 || options.query_timeout_ms == 0U ||
      options.size > 1024U) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "Invalid MySQL connection pool configuration");
  }
  try {
    auto state = std::make_shared<SharedState>(std::move(options));
    state->connections.reserve(state->options.size);
    state->available_indices.resize(state->options.size);
    for (std::size_t index = 0; index < state->options.size; ++index) {
      state->connections.push_back(OpenConnection(
          state->options.connection, state->options.query_timeout_ms));
      state->available_indices[state->available_count] = index;
      ++state->available_count;
    }
    return std::unique_ptr<MySqlConnectionPool>{
        new MySqlConnectionPool{std::move(state)}};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "Unable to initialize the MySQL connection pool", true);
  } catch (const bf::infrastructure::mysql::SqlError&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "Failed to configure a MySQL connection session", false);
  } catch (const std::exception&) {
    return MakeError(
        application::ErrorCode::kStorageUnavailable,
        "Unexpected error while initializing the MySQL connection pool", true);
  }
}

MySqlConnectionPool::MySqlConnectionPool(
    std::shared_ptr<SharedState> state) noexcept
    : state_(std::move(state)) {}

MySqlConnectionPool::~MySqlConnectionPool() { Stop(); }

application::ServiceResult<MySqlConnectionPool::Lease>
MySqlConnectionPool::Acquire() {
  std::unique_lock lock{state_->mutex};
  const auto ready = state_->available_changed.wait_for(
      lock, state_->options.acquire_timeout,
      [this] { return state_->stopping || state_->available_count != 0U; });
  if (!ready) {
    return MakeError(application::ErrorCode::kOverloaded,
                     "MySQL 连接池获取超时", true);
  }
  if (state_->stopping) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "MySQL 连接池正在停止");
  }
  --state_->available_count;
  const auto index = state_->available_indices[state_->available_count];
  lock.unlock();

  try {
    if (!state_->connections[index] || !state_->connections[index]->IsOpen()) {
      state_->connections[index] = OpenConnection(
          state_->options.connection, state_->options.query_timeout_ms);
    }
    return Lease{state_, index};
  } catch (const std::exception&) {
    {
      std::scoped_lock restore_lock{state_->mutex};
      state_->available_indices[state_->available_count] = index;
      ++state_->available_count;
    }
    state_->available_changed.notify_one();
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "MySQL 连接恢复失败", true);
  }
}

void MySqlConnectionPool::Stop() noexcept {
  if (!state_) {
    return;
  }
  {
    std::scoped_lock lock{state_->mutex};
    state_->stopping = true;
  }
  state_->available_changed.notify_all();
}

std::size_t MySqlConnectionPool::Size() const noexcept {
  std::scoped_lock lock{state_->mutex};
  return state_->connections.size();
}

std::size_t MySqlConnectionPool::Available() const noexcept {
  std::scoped_lock lock{state_->mutex};
  return state_->available_count;
}

}  // namespace bf::infrastructure::mysql
