#include "crypto/windows_cng_credential_crypto.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bcrypt.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace bf::infrastructure::security {
namespace {

application::ServiceError CryptoError(std::string operation) {
  return application::ServiceError{
      .code = application::ErrorCode::kStorageUnavailable,
      .safe_message = "Windows CNG " + std::move(operation) + " 失败",
      .is_retryable = true};
}

class AlgorithmHandle final {
 public:
  ~AlgorithmHandle() {
    if (value_ != nullptr) {
      static_cast<void>(BCryptCloseAlgorithmProvider(value_, 0U));
    }
  }

  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&&) = delete;
  AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

  AlgorithmHandle() = default;

  [[nodiscard]] BCRYPT_ALG_HANDLE* Address() noexcept { return &value_; }

  [[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return value_; }

 private:
  BCRYPT_ALG_HANDLE value_{nullptr};
};

template <typename Bytes>
application::ServiceResult<Bytes> GenerateRandomBytes() {
  Bytes output{};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, output.data(),
                                      static_cast<ULONG>(output.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return CryptoError("生成密码学随机字节");
  }
  return output;
}

bool ConstantTimeEqual(const application::PasswordDigest& left,
                       const application::PasswordDigest& right) noexcept {
  volatile std::uint8_t difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference =
        static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
  }
  return difference == 0U;
}

}  // namespace

application::ServiceResult<application::PasswordDigest>
WindowsCngCredentialCrypto::Derive(std::string_view password,
                                   const application::CredentialSalt& salt,
                                   std::uint32_t iterations) const {
  if (iterations == 0U) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "PBKDF2 迭代次数必须大于零"};
  }
  if (password.size() > std::numeric_limits<ULONG>::max()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "密码 UTF-8 字节数超过 CNG 输入上限"};
  }

  AlgorithmHandle algorithm;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr,
          BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
    return CryptoError("打开 PBKDF2 SHA-256 provider");
  }

  application::PasswordDigest digest{};
  const auto status = BCryptDeriveKeyPBKDF2(
      algorithm.Get(),
      reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
      static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
      static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iterations),
      digest.data(), static_cast<ULONG>(digest.size()), 0U);
  if (!BCRYPT_SUCCESS(status)) {
    return CryptoError("派生 PBKDF2-SHA256 摘要");
  }
  return digest;
}

application::ServiceResult<bool> WindowsCngCredentialCrypto::Verify(
    std::string_view password, const application::CredentialSalt& salt,
    std::uint32_t iterations,
    const application::PasswordDigest& expected) const {
  auto derived_result = Derive(password, salt, iterations);
  if (const auto* error =
          std::get_if<application::ServiceError>(&derived_result)) {
    return *error;
  }
  auto derived = std::get<application::PasswordDigest>(derived_result);
  const bool matches = ConstantTimeEqual(derived, expected);
  static_cast<void>(SecureZeroMemory(derived.data(), derived.size()));
  return matches;
}

application::ServiceResult<application::CredentialSalt>
WindowsCngCredentialCrypto::GenerateCredentialSalt() const {
  return GenerateRandomBytes<application::CredentialSalt>();
}

application::ServiceResult<application::LoginTicketSecret>
WindowsCngCredentialCrypto::GenerateLoginTicketSecret() const {
  return GenerateRandomBytes<application::LoginTicketSecret>();
}

}  // namespace bf::infrastructure::security

#endif
