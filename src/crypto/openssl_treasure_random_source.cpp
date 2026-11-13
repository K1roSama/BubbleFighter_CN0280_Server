#include "crypto/openssl_treasure_random_source.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace bf::infrastructure::security {
namespace {

application::ServiceError RandomError() {
  return {.code = application::ErrorCode::kInternal,
          .safe_message = "无法生成泡泡奖励随机序列"};
}

std::optional<std::uint64_t> UniformBelow(std::uint64_t bound) {
  if (bound == 0U) {
    return std::nullopt;
  }
  const auto threshold = (std::uint64_t{0} - bound) % bound;
  for (;;) {
    std::uint64_t value = 0U;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) !=
        1) {
      return std::nullopt;
    }
    if (value >= threshold) {
      return value % bound;
    }
  }
}

}  // namespace

application::ServiceResult<std::vector<std::size_t>>
OpenSslTreasureRandomSource::SampleWithoutReplacement(std::size_t upper_bound,
                                                      std::size_t count) {
  if (count > upper_bound || upper_bound == 0U ||
      upper_bound >
          static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "泡泡奖励抽样边界无效"};
  }
  std::vector<std::size_t> indices(upper_bound);
  std::iota(indices.begin(), indices.end(), 0U);
  for (std::size_t index = 0U; index < count; ++index) {
    const auto offset =
        UniformBelow(static_cast<std::uint64_t>(upper_bound - index));
    if (!offset.has_value()) {
      return RandomError();
    }
    std::swap(indices[index],
              indices[index + static_cast<std::size_t>(*offset)]);
  }
  indices.resize(count);
  return indices;
}

}  // namespace bf::infrastructure::security
