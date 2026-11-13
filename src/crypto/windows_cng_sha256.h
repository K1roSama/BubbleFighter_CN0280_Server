#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_SHA256_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_SHA256_H_

/**
 * @file windows_cng_sha256.h
 * @brief 定义 Windows CNG 提供的受维护 SHA-256 基础设施适配器。
 */

#include "features/authentication/authentication.h"

#if defined(_WIN32)

namespace bf::infrastructure::security {

/**
 * @brief 使用 Windows BCrypt/CNG 计算 SHA-256。
 * @note 类本身无状态且可并发调用；每次调用拥有并关闭自己的算法和 hash handle。
 */
class WindowsCngSha256 final : public application::Sha256Hasher {
 public:
  /** @brief 计算摘要；CNG 失败时返回不包含输入内容的存储不可用错误。 */
  [[nodiscard]] application::ServiceResult<application::Sha256Digest> Hash(
      std::span<const std::uint8_t> input) const override;
};

}  // namespace bf::infrastructure::security

#endif

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_SHA256_H_
