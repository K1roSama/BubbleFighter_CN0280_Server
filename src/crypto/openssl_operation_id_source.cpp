#include "crypto/openssl_operation_id_source.h"

#include <openssl/rand.h>

#include <cstdint>

namespace bf::infrastructure::security {

application::ServiceResult<domain::OperationId>
OpenSslOperationIdSource::Next() {
  domain::OperationId::ByteArray bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "OpenSSL 生成购买操作标识失败",
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
