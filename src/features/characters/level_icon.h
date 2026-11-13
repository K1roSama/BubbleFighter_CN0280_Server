#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_LEVEL_ICON_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_LEVEL_ICON_H_

/** @file level_icon.h
 *  @brief 定义原生等级图标偏好的持久化边界。
 */
#include <cstdint>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {
/** @brief C2S2008 的三个 DWORD；家族参数只是客户端缓存，不能授予身份。 */
struct LevelIconSelection final {
  std::uint32_t display_type{0U};  ///< 0 普通等级、1 天梯、2 竞技、3 家族徽章。
  std::uint32_t clan_id{0U};       ///< 发起操作时客户端缓存的家族号。
  std::uint32_t clan_mark{0U};     ///< 发起操作时客户端缓存的徽章。
};

/** @brief 在玩家锁和家族成员锁下提交显示偏好；重复选择无写入副作用。 */
class LevelIconStore {
 public:
  virtual ~LevelIconStore() = default;
  /** @brief 成功返回是否改变；不具备真实阶级数据的类型必须明确拒绝。 */
  [[nodiscard]] virtual ServiceResult<bool> Save(
      domain::PlayerId player, const LevelIconSelection& selection) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_LEVEL_ICON_H_
