#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_H_

/** @file native_title.h @brief 原931勋章兑换及932本人称号穿戴的原子业务端口。
 */
#include <cstdint>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"

namespace bf::application {
/** @brief 原生称号操作回执；提交前不能发出成功。 */
struct NativeTitleResult final {
  std::int32_t result{-1};
  std::uint32_t title_id{0U};
  bool changed{false};
};

/** @brief 只接受认证本人、原始称号键及内部幂等UUID，绝不由总战绩回填勋章。 */
class NativeTitleStore {
 public:
  virtual ~NativeTitleStore() = default;
  [[nodiscard]] virtual ServiceResult<NativeTitleResult> Claim(
      domain::PlayerId player, std::uint32_t title,
      const domain::OperationId& operation) = 0;
  [[nodiscard]] virtual ServiceResult<NativeTitleResult> Set(
      domain::PlayerId player, std::uint32_t title, std::uint32_t action) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_NATIVE_TITLE_H_
