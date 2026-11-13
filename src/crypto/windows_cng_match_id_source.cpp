#include "crypto/windows_cng_match_id_source.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bcrypt.h>

namespace bf::infrastructure::security {

application::ServiceResult<domain::MatchId> WindowsCngMatchIdSource::Next() {
  domain::MatchId::ByteArray bytes{};
  const auto status =
      BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "Windows CNG 生成对局标识失败",
        .correlation_id = {},
        .is_retryable = true};
  }
  // RFC UUID v4 版本与 variant 位；其余随机内容由系统 CSPRNG 提供。
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  const auto value = domain::MatchId::FromBytes(bytes);
  if (!value.has_value()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInternal,
        .safe_message = "Windows CNG 返回了非法对局标识"};
  }
  return *value;
}

}  // namespace bf::infrastructure::security

#endif
