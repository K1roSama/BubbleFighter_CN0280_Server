#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_OPERATION_ID_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_OPERATION_ID_SOURCE_H_

/**
 * @file windows_cng_operation_id_source.h
 * @brief 定义使用 Windows CNG 生成购买内部 UUID 操作标识的适配器。
 */

#include "sessions/gateway_login_flow.h"

#if defined(_WIN32)

namespace bf::infrastructure::security {

/**
 * @brief 用系统首选 CSPRNG 生成 RFC 4122 variant 的随机 UUID v4。
 * @note
 * 类无状态且可并发调用；随机失败返回可重试存储错误，不返回全零或可预测计数 ID。
 * @note 该 ID 只支持一次 C2S809
 * 在服务端内部的重试/提交查询；协议没有跨断线客户端键。
 */
class WindowsCngOperationIdSource final
    : public runtime::PurchaseOperationIdSource {
 public:
  /** @brief 生成非零 UUID v4，并以强类型 OperationId 返回。 */
  [[nodiscard]] application::ServiceResult<domain::OperationId> Next() override;
};

}  // namespace bf::infrastructure::security

#endif

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_OPERATION_ID_SOURCE_H_
