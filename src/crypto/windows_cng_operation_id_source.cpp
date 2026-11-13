#include "crypto/windows_cng_operation_id_source.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bcrypt.h>

namespace bf::infrastructure::security {

application::ServiceResult<domain::OperationId>
WindowsCngOperationIdSource::Next() {
  domain::OperationId::ByteArray bytes{};
  const auto status =
      BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "Windows CNG 生成购买操作标识失败",
        .correlation_id = {},
        .is_retryable = true};
  }

  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  auto operation_id = domain::OperationId::FromBytes(bytes);
  if (!operation_id.has_value()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInternal,
        .safe_message = "购买操作标识生成了无效零值",
        .correlation_id = {},
        .is_retryable = false};
  }
  return *operation_id;
}

}  // namespace bf::infrastructure::security

#endif
