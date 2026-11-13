#ifndef BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_MATCH_ID_SOURCE_H_
#define BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_MATCH_ID_SOURCE_H_

/**
 * @file windows_cng_match_id_source.h
 * @brief 声明 Windows 系统随机源驱动的对局 UUID v4 生成器。
 */

#ifdef _WIN32

#include "features/battle/match_start.h"

namespace bf::infrastructure::security {

/**
 * @brief 用 Windows 系统首选 CSPRNG 生成 RFC 4122 variant 的随机对局 UUID v4。
 * @note
 * 类无状态且可并发调用；随机源失败返回可重试错误，不退化到时间或计数标识。
 */
class WindowsCngMatchIdSource final : public application::MatchIdSource {
 public:
  /** @brief 生成非零 UUID v4，并以强类型 MatchId 返回。 */
  [[nodiscard]] application::ServiceResult<domain::MatchId> Next() override;
};

}  // namespace bf::infrastructure::security

#endif

#endif  // BUBBLEFIGHTER_SERVER_CRYPTO_WINDOWS_CNG_MATCH_ID_SOURCE_H_
