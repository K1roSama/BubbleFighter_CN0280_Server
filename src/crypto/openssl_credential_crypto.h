#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_CREDENTIAL_CRYPTO_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_CREDENTIAL_CRYPTO_H_

/**
 * @file openssl_credential_crypto.h
 * @brief 定义跨平台 OpenSSL 密码派生与安全随机数实现。
 */

#include "features/authentication/credential_crypto.h"

namespace bf::infrastructure::security {

/**
 * @brief 使用 OpenSSL 提供 PBKDF2-HMAC-SHA256、恒定时间比较和系统安全随机数。
 * @note
 * 本对象无可变状态，可由有界认证工作池并发调用；密码、盐和派生摘要均不写日志。
 */
class OpenSslCredentialCrypto final
    : public application::Pbkdf2PasswordHasher,
      public application::CryptographicRandomSource {
 public:
  /** @brief 派生固定 32 字节密码摘要；迭代次数必须满足 OpenSSL
   * 的有符号整数上限。 */
  [[nodiscard]] application::ServiceResult<application::PasswordDigest> Derive(
      std::string_view password, const application::CredentialSalt& salt,
      std::uint32_t iterations) const override;

  /** @brief 重新派生后使用恒定时间比较，并清理临时摘要。 */
  [[nodiscard]] application::ServiceResult<bool> Verify(
      std::string_view password, const application::CredentialSalt& salt,
      std::uint32_t iterations,
      const application::PasswordDigest& expected) const override;

  /** @brief 从 OpenSSL 配置的系统熵源生成 16 字节凭据盐。 */
  [[nodiscard]] application::ServiceResult<application::CredentialSalt>
  GenerateCredentialSalt() const override;

  /** @brief 从 OpenSSL 配置的系统熵源生成 16 字节一次性登录票据秘密。 */
  [[nodiscard]] application::ServiceResult<application::LoginTicketSecret>
  GenerateLoginTicketSecret() const override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_CREDENTIAL_CRYPTO_H_
