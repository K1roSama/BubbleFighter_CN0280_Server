#include "crypto/openssl_match_id_source.h"

#include <openssl/rand.h>

#include <cstdint>

namespace bf::infrastructure::security {

application::ServiceResult<domain::MatchId> OpenSslMatchIdSource::Next() {
  domain::MatchId::ByteArray bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "OpenSSL 生成对局标识失败",
        .correlation_id = {},
        .is_retryable = true};
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  const auto match_id = domain::MatchId::FromBytes(bytes);
  if (!match_id.has_value()) {
    return application::ServiceError{.code = application::ErrorCode::kInternal,
                                     .safe_message = "对局标识生成了无效零值",
                                     .correlation_id = {},
                                     .is_retryable = false};
  }
  return *match_id;
}

}  // namespace bf::infrastructure::security
