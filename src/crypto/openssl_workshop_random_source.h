#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_WORKSHOP_RANDOM_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_WORKSHOP_RANDOM_SOURCE_H_

/** @file openssl_workshop_random_source.h @brief 定义基于 OpenSSL CSPRNG
 * 的工坊奖励选择器。 */

#include "features/shop/workshop_service.h"

namespace bf::infrastructure::security {

/** @brief 使用拒绝采样消除取模偏差的工坊随机源。 */
class OpenSslWorkshopRandomSource final
    : public application::WorkshopRandomSource {
 public:
  /** @brief 从非空候选范围选择一个无偏索引；熵源失败返回内部错误。 */
  [[nodiscard]] application::ServiceResult<std::size_t> Choose(
      std::size_t upper_bound) override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_WORKSHOP_RANDOM_SOURCE_H_
