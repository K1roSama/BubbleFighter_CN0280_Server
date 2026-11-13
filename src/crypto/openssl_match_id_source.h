#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_MATCH_ID_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_MATCH_ID_SOURCE_H_

/**
 * @file openssl_match_id_source.h
 * @brief 定义使用 OpenSSL CSPRNG 生成持久化对局 UUID 的跨平台适配器。
 */

#include "features/battle/match_start.h"

namespace bf::infrastructure::security {

/** @brief 使用 `RAND_bytes` 生成 RFC 4122 variant 的不可预测 UUID v4。 */
class OpenSslMatchIdSource final : public application::MatchIdSource {
 public:
  /** @brief 返回非零对局标识；系统随机源失败时返回可重试错误。 */
  [[nodiscard]] application::ServiceResult<domain::MatchId> Next() override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_MATCH_ID_SOURCE_H_
