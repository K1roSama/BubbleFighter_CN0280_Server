#include "features/battle/match_lifecycle_repair.h"

#include <chrono>

namespace bf::application {
namespace {

std::optional<ServiceError> ValidateTime(
    std::chrono::system_clock::time_point value) {
  if (value.time_since_epoch() <= std::chrono::system_clock::duration::zero()) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "对局中断时间必须是有效 UTC 时间"};
  }
  return std::nullopt;
}

}  // namespace

MatchLifecycleRepairService::MatchLifecycleRepairService(
    MatchLifecycleRepairRepository& repository) noexcept
    : repository_(repository) {}

std::optional<ServiceError> MatchLifecycleRepairService::InterruptFailedStart(
    const domain::MatchId& match_id,
    std::chrono::system_clock::time_point interrupted_at) const {
  if (const auto error = ValidateTime(interrupted_at)) {
    return error;
  }
  return repository_.Interrupt(match_id, interrupted_at);
}

ServiceResult<MatchLifecycleRecoveryReport>
MatchLifecycleRepairService::RecoverAbandoned(
    std::chrono::system_clock::time_point interrupted_at) const {
  if (const auto error = ValidateTime(interrupted_at)) {
    return *error;
  }
  return repository_.RecoverAbandoned(interrupted_at);
}

}  // namespace bf::application
