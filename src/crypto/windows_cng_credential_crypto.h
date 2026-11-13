#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_CREDENTIAL_CRYPTO_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_CREDENTIAL_CRYPTO_H_

/**
 * @file windows_cng_credential_crypto.h
 * @brief 定义 Windows CNG 提供的 PBKDF2 和密码学随机源适配器。
 */

#include "features/authentication/credential_crypto.h"

#if defined(_WIN32)

namespace bf::infrastructure::security {

/**
 * @brief 使用 Windows BCrypt/CNG 实现兼容 PBKDF2-HMAC-SHA256 和系统随机源。
 * @note 类本身无状态且可并发调用；每次派生拥有并关闭自己的算法 handle。
 */
class WindowsCngCredentialCrypto final
    : public application::Pbkdf2PasswordHasher,
      public application::CryptographicRandomSource {
 public:
  /** @brief 使用 CNG 派生 32 字节密码摘要；失败信息不包含密码或盐。 */
  [[nodiscard]] application::ServiceResult<application::PasswordDigest> Derive(
      std::string_view password, const application::CredentialSalt& salt,
      std::uint32_t iterations) const override;

  /** @brief 使用 CNG 重新派生、恒定时间比较并清理临时摘要。 */
  [[nodiscard]] application::ServiceResult<bool> Verify(
      std::string_view password, const application::CredentialSalt& salt,
      std::uint32_t iterations,
      const application::PasswordDigest& expected) const override;

  /** @brief 使用系统首选 CSPRNG 生成 16 字节密码盐。 */
  [[nodiscard]] application::ServiceResult<application::CredentialSalt>
  GenerateCredentialSalt() const override;

  /** @brief 使用系统首选 CSPRNG 生成 128 位票据随机量。 */
  [[nodiscard]] application::ServiceResult<application::LoginTicketSecret>
  GenerateLoginTicketSecret() const override;
};

}  // namespace bf::infrastructure::security

#endif

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_CREDENTIAL_CRYPTO_H_
