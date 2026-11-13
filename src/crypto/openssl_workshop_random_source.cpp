#include "crypto/openssl_workshop_random_source.h"

#include <openssl/rand.h>

#include <cstdint>
#include <limits>

namespace bf::infrastructure::security {

application::ServiceResult<std::size_t> OpenSslWorkshopRandomSource::Choose(
    std::size_t upper_bound) {
  if (upper_bound == 0U ||
      upper_bound > std::numeric_limits<std::uint32_t>::max()) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "工坊随机候选数量无效"};
  }
  const auto bound = static_cast<std::uint32_t>(upper_bound);
  const auto limit = std::numeric_limits<std::uint32_t>::max() -
                     (std::numeric_limits<std::uint32_t>::max() % bound);
  std::uint32_t value = 0U;
  do {
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) !=
        1) {
      return application::ServiceError{
          .code = application::ErrorCode::kInternal,
          .safe_message = "工坊安全随机源不可用"};
    }
  } while (value >= limit);
  return static_cast<std::size_t>(value % bound);
}

}  // namespace bf::infrastructure::security
