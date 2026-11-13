#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_PROCESS_GUARD_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_PROCESS_GUARD_H_

/**
 * @file world_process_guard.h
 * @brief 在世界状态尚未分片时阻止两个游戏进程同时恢复同一个数据库。
 */
#include <cstdint>
#include <memory>
#include <string>

#include "application/error.h"
#include "features/configuration/mysql_settings.h"

namespace bf::infrastructure::mysql {
/** @brief 独立连接持有数据库会话锁；登录进程不领取世界所有权。 */
class WorldProcessGuard final {
 public:
  /** @brief 获取唯一世界所有权；已有游戏进程时失败，错误不包含连接秘密。 */
  [[nodiscard]] static application::ServiceResult<
      std::unique_ptr<WorldProcessGuard>>
  Acquire(const ConnectionSettings& settings, std::uint32_t timeout_ms);
  ~WorldProcessGuard();
  WorldProcessGuard(const WorldProcessGuard&) = delete;
  WorldProcessGuard& operator=(const WorldProcessGuard&) = delete;
  /** @brief 验证持锁连接仍有效；失效后不会自动重连，调用方必须停止游戏服务。 */
  [[nodiscard]] bool Healthy() noexcept;

 private:
  /** @brief 隐藏仅由守卫持有的 MySQL 会话连接。 */
  class Impl;
  explicit WorldProcessGuard(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_WORLD_PROCESS_GUARD_H_
