#include "crypto/openssl_sha256.h"

#include <openssl/evp.h>

#include <cstdint>

namespace bf::infrastructure::security {

application::ServiceResult<application::Sha256Digest> OpenSslSha256::Hash(
    std::span<const std::uint8_t> input) const {
  application::Sha256Digest digest{};
  unsigned int length = 0U;
  if (EVP_Digest(input.data(), input.size(), digest.data(), &length,
                 EVP_sha256(), nullptr) != 1 ||
      length != static_cast<unsigned int>(digest.size())) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "OpenSSL 计算 SHA-256 失败",
        .correlation_id = {},
        .is_retryable = true};
  }
  return digest;
}

}  // namespace bf::infrastructure::security
