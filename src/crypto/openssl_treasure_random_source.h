#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_TREASURE_RANDOM_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_TREASURE_RANDOM_SOURCE_H_

/**
 * @file openssl_treasure_random_source.h
 * @brief 定义泡泡奖励无放回抽样的 OpenSSL CSPRNG 实现。
 */

#include "features/shop/treasure_service.h"

namespace bf::infrastructure::security {

/** @brief 使用 OpenSSL RAND_bytes 和拒绝采样生成无模偏差的不同奖励索引。 */
class OpenSslTreasureRandomSource final
    : public application::TreasureRandomSource {
 public:
  /** @brief 从有限索引域无放回抽样；熵源失败时不返回部分结果。 */
  [[nodiscard]] application::ServiceResult<std::vector<std::size_t>>
  SampleWithoutReplacement(std::size_t upper_bound, std::size_t count) override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_TREASURE_RANDOM_SOURCE_H_
