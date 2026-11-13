#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_KEY_MAP_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_KEY_MAP_H_

/** @file key_map.h
 *  @brief 原版按键覆盖表的持久化边界。
 */
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {
inline constexpr std::size_t kMaximumKeyBindings =
    41U;  ///< 当前原协议键表含37基础和4已发布通信/标点动作。

/** @brief 一项相对客户端 KeyMapConfig.csv 默认值的覆盖。 */
struct KeyBinding final {
  std::string action;     ///< 原版 codeName，不能是脚本或任意配置路径。
  std::uint32_t key{0U};  ///< 原DWORD0为明确未绑定；1..255为Windows虚拟键码。
};

/** @brief 2016 是完整覆盖表；reset 为真时清空覆盖并使用客户端默认。 */
struct KeyMapSettings final {
  bool reset{false};  ///< 原客户端 v3=所有按键均等于默认值。
  std::vector<KeyBinding>
      bindings;  ///< 当前键表最多41项可修改按键，动作不得重复。
};

/** @brief 校验原版可修改动作、键码和完整替换/恢复默认契约。 */
[[nodiscard]] bool ValidKeyMap(const KeyMapSettings& settings) noexcept;

/** @brief 提供跨连接和重启保存的玩家按键表；所有方法由数据库工作池调用。 */
class KeyMapStore {
 public:
  virtual ~KeyMapStore() = default;
  /** @brief 加载当前玩家的覆盖表；无记录等价于客户端默认。 */
  [[nodiscard]] virtual ServiceResult<std::vector<KeyBinding>> Load(
      domain::PlayerId player) = 0;
  /** @brief 原子替换完整按键覆盖表，不能把本次缺省的旧绑定保留下来。 */
  [[nodiscard]] virtual ServiceResult<bool> Save(
      domain::PlayerId player, const KeyMapSettings& settings) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_KEY_MAP_H_
