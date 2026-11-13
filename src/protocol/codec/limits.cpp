#include "protocol/codec/limits.h"

#include <utility>

namespace bf::protocol {
namespace {

constexpr std::size_t kMinimumPacketBytes = 8U;
constexpr std::size_t kMaximumAllowedPacketBytes =
    std::size_t{16} * 1024U * 1024U;
constexpr std::size_t kMaximumAllowedSendQueueBytes =
    std::size_t{256} * 1024U * 1024U;

application::ServiceError InvalidLimits(std::string message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = std::move(message),
      .correlation_id = {},
      .is_retryable = false,
  };
}

}  // namespace

std::optional<application::ServiceError> ValidateProtocolLimits(
    const ProtocolLimits& limits) {
  if (limits.maximum_packet_bytes < kMinimumPacketBytes ||
      limits.maximum_packet_bytes > kMaximumAllowedPacketBytes) {
    return InvalidLimits("network.max_packet_bytes 超出允许范围");
  }
  if (limits.maximum_send_queue_bytes < limits.maximum_packet_bytes ||
      limits.maximum_send_queue_bytes > kMaximumAllowedSendQueueBytes) {
    return InvalidLimits(
        "network.max_send_queue_bytes 必须容纳完整包且不超过全局上限");
  }
  return std::nullopt;
}

}  // namespace bf::protocol
