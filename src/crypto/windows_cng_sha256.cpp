#include "crypto/windows_cng_sha256.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bcrypt.h>

#include <limits>
#include <string>

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

class HashHandle final {
 public:
  ~HashHandle() {
    if (value_ != nullptr) {
      static_cast<void>(BCryptDestroyHash(value_));
    }
  }

  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  HashHandle(HashHandle&&) = delete;
  HashHandle& operator=(HashHandle&&) = delete;

  HashHandle() = default;

  [[nodiscard]] BCRYPT_HASH_HANDLE* Address() noexcept { return &value_; }

  [[nodiscard]] BCRYPT_HASH_HANDLE Get() const noexcept { return value_; }

 private:
  BCRYPT_HASH_HANDLE value_{nullptr};
};

}  // namespace

application::ServiceResult<application::Sha256Digest> WindowsCngSha256::Hash(
    std::span<const std::uint8_t> input) const {
  if (input.size() > std::numeric_limits<ULONG>::max()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "SHA-256 输入超过 CNG 单次长度上限"};
  }

  AlgorithmHandle algorithm;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U))) {
    return CryptoError("打开 SHA-256 provider");
  }
  HashHandle hash;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm.Get(), hash.Address(), nullptr,
                                       0U, nullptr, 0U, 0U))) {
    return CryptoError("创建 SHA-256 handle");
  }
  if (!input.empty() && !BCRYPT_SUCCESS(BCryptHashData(
                            hash.Get(), const_cast<PUCHAR>(input.data()),
                            static_cast<ULONG>(input.size()), 0U))) {
    return CryptoError("写入 SHA-256 数据");
  }

  application::Sha256Digest digest{};
  if (!BCRYPT_SUCCESS(BCryptFinishHash(
          hash.Get(), digest.data(), static_cast<ULONG>(digest.size()), 0U))) {
    return CryptoError("完成 SHA-256 摘要");
  }
  return digest;
}

}  // namespace bf::infrastructure::security

#endif
