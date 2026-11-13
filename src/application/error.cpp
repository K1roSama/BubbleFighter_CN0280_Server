#include "application/error.h"

namespace bf::application {

std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kInvalidInput:
      return "invalid_input";
    case ErrorCode::kUnauthenticated:
      return "unauthenticated";
    case ErrorCode::kPermissionDenied:
      return "permission_denied";
    case ErrorCode::kInvalidState:
      return "invalid_state";
    case ErrorCode::kInsufficientBalance:
      return "insufficient_balance";
    case ErrorCode::kConflict:
      return "conflict";
    case ErrorCode::kOverloaded:
      return "overloaded";
    case ErrorCode::kStorageUnavailable:
      return "storage_unavailable";
    case ErrorCode::kInternal:
      return "internal";
    case ErrorCode::kUnsupportedOpcode:
      return "unsupported_opcode";
  }
  return "internal";
}

int ProcessExitCode(const ServiceError& error) noexcept {
  if (error.code == ErrorCode::kInvalidInput) {
    return 2;
  }
  if (error.code == ErrorCode::kStorageUnavailable) {
    return 3;
  }
  return 1;
}

}  // namespace bf::application
