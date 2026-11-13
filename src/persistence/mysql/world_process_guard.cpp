#include "persistence/mysql/world_process_guard.h"

#include <utility>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
constexpr std::string_view kLockName =
    "CONCAT('bf.r42.world.',LEFT(SHA2(DATABASE(),256),40))";
}  // namespace

class WorldProcessGuard::Impl final {
 public:
  explicit Impl(const ConnectionSettings& settings) : connection(settings) {}

  Connection connection;
};

WorldProcessGuard::WorldProcessGuard(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WorldProcessGuard::~WorldProcessGuard() = default;

application::ServiceResult<std::unique_ptr<WorldProcessGuard>>
WorldProcessGuard::Acquire(const ConnectionSettings& settings,
                           std::uint32_t timeout_ms) {
  try {
    auto connection_settings = settings;
    connection_settings.query_timeout_ms = timeout_ms;
    auto impl = std::make_unique<Impl>(connection_settings);
    const auto lock =
        impl->connection
            .Execute("SELECT GET_LOCK(" + std::string(kLockName) + ",0)")
            .OneField();
    if (lock.IsNull() || !lock.As<bool>()) {
      return application::ServiceError{
          .code = application::ErrorCode::kConflict,
          .safe_message =
              "Another game server process already owns this database",
          .correlation_id = {}};
    }
    return std::unique_ptr<WorldProcessGuard>(
        new WorldProcessGuard(std::move(impl)));
  } catch (const std::exception&) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message =
            "Unable to acquire ownership of the game world database",
        .correlation_id = {},
        .is_retryable = true};
  }
}

bool WorldProcessGuard::Healthy() noexcept {
  try {
    const auto owner =
        impl_->connection
            .Execute("SELECT COALESCE(IS_USED_LOCK(" + std::string(kLockName) +
                     ") = CONNECTION_ID(),FALSE)")
            .OneField();
    return owner.As<bool>();
  } catch (...) {
    return false;
  }
}

}  // namespace bf::infrastructure::mysql
