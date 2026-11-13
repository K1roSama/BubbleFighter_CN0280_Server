#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_OPERATION_ID_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_OPERATION_ID_SOURCE_H_

/**
 * @file openssl_operation_id_source.h
 * @brief 定义使用 OpenSSL CSPRNG 生成购买内部 UUID 的跨平台适配器。
 */

#include "sessions/gateway_login_flow.h"

namespace bf::infrastructure::security {

/** @brief 使用 `RAND_bytes` 生成 RFC 4122 variant 的随机 UUID v4。 */
class OpenSslOperationIdSource final
    : public runtime::PurchaseOperationIdSource {
 public:
  /** @brief 返回不可预测的非零操作标识；随机源失败返回可重试错误。 */
  [[nodiscard]] application::ServiceResult<domain::OperationId> Next() override;
};

}  // namespace bf::infrastructure::security

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_OPENSSL_OPERATION_ID_SOURCE_H_
