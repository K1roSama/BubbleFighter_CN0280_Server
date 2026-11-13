#include "crypto/openssl_credential_crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <climits>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace bf::infrastructure::security {
namespace {

application::ServiceError CryptoError(std::string message) {
  return {.code = application::ErrorCode::kInternal,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = false};
}

template <typename Bytes>
application::ServiceResult<Bytes> RandomBytes(
    std::string_view failure_message) {
  Bytes value{};
  static_assert(std::tuple_size_v<Bytes> <= static_cast<std::size_t>(INT_MAX));
  if (RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) {
    return CryptoError(std::string{failure_message});
  }
  return value;
}

}  // namespace

application::ServiceResult<application::PasswordDigest>
OpenSslCredentialCrypto::Derive(std::string_view password,
                                const application::CredentialSalt& salt,
                                std::uint32_t iterations) const {
  if (iterations == 0U || iterations > static_cast<std::uint32_t>(INT_MAX) ||
      password.size() > static_cast<std::size_t>(INT_MAX)) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "PBKDF2 输入超过 OpenSSL 支持范围",
        .correlation_id = {},
        .is_retryable = false};
  }
  application::PasswordDigest digest{};
  static_assert(application::PasswordDigest{}.size() <=
                static_cast<std::size_t>(INT_MAX));
  const auto result = PKCS5_PBKDF2_HMAC(
      password.data(), static_cast<int>(password.size()), salt.data(),
      static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
      static_cast<int>(digest.size()), digest.data());
  if (result != 1) {
    OPENSSL_cleanse(digest.data(), digest.size());
    return CryptoError("OpenSSL 无法派生密码摘要");
  }
  return digest;
}

application::ServiceResult<bool> OpenSslCredentialCrypto::Verify(
    std::string_view password, const application::CredentialSalt& salt,
    std::uint32_t iterations,
    const application::PasswordDigest& expected) const {
  auto derived_result = Derive(password, salt, iterations);
  if (const auto* error =
          std::get_if<application::ServiceError>(&derived_result)) {
    return *error;
  }
  auto derived = std::get<application::PasswordDigest>(derived_result);
  const bool equal =
      CRYPTO_memcmp(derived.data(), expected.data(), derived.size()) == 0;
  OPENSSL_cleanse(derived.data(), derived.size());
  return equal;
}

application::ServiceResult<application::CredentialSalt>
OpenSslCredentialCrypto::GenerateCredentialSalt() const {
  return RandomBytes<application::CredentialSalt>("OpenSSL 无法生成凭据盐");
}

application::ServiceResult<application::LoginTicketSecret>
OpenSslCredentialCrypto::GenerateLoginTicketSecret() const {
  return RandomBytes<application::LoginTicketSecret>(
      "OpenSSL 无法生成登录票据随机值");
}

}  // namespace bf::infrastructure::security
