#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_SHA256_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_SHA256_H_

/**
 * @file openssl_sha256.h
 * @brief 定义正式外部依赖构建使用的跨平台 SHA-256 适配器。
 */

#include "features/authentication/authentication.h"

namespace bf::infrastructure::security {

/** @brief 使用 OpenSSL EVP 一次性摘要接口计算 SHA-256。 */
class OpenSslSha256 final : public application::Sha256Hasher {
 public:
  /** @brief 计算固定 32 字节摘要；错误结果不包含原始输入。 */
  [[nodiscard]] application::ServiceResult<application::Sha256Digest> Hash(
      std::span<const std::uint8_t> input) const override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_SHA256_H_
