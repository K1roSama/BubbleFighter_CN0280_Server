#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_WORLD_ACCESS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_WORLD_ACCESS_H_

/**
 * @file world_access.h
 * @brief 定义进入世界命令前所需的战队权限与双向屏蔽只读快照。
 */

#include <optional>
#include <string_view>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {

/** @brief 同一数据库快照中取得的世界访问权限与聊天过滤事实。 */
struct WorldAccessSnapshot final {
  domain::PlayerId player_id;       ///< 查询所属玩家。
  bool has_clan_membership{false};  ///< 是否存在当前战队成员关系。
  std::vector<domain::PlayerId>
      chat_blocked_peers;  ///< 任一方向屏蔽的对端，按 ID 严格升序。
  bool muted{false};
  bool banned{false};
  bool has_chat_penalty_history{
      false};  ///< 经管理员确认且未撤销的聊天处罚事实。
};

/** @brief 世界访问事实的数据库读取端口。 */
class WorldAccessReader {
 public:
  virtual ~WorldAccessReader() = default;

  [[nodiscard]] virtual std::optional<ServiceError> ActivateDevice(
      domain::PlayerId, std::string_view) {
    return ServiceError{.code = ErrorCode::kInternal,
                        .safe_message = "设备绑定服务不可用"};
  }

  /** @brief 玩家不存在返回空值；战队与屏蔽必须来自同一只读事务。 */
  [[nodiscard]] virtual ServiceResult<std::optional<WorldAccessSnapshot>> Load(
      domain::PlayerId player_id) = 0;
};

/** @brief Session handler 使用的已校验世界访问快照加载端口。 */
class WorldAccessLoader {
 public:
  virtual ~WorldAccessLoader() = default;

  [[nodiscard]] virtual std::optional<ServiceError> ActivateDevice(
      domain::PlayerId, std::string_view) const {
    return ServiceError{.code = ErrorCode::kInternal,
                        .safe_message = "设备绑定服务不可用"};
  }

  /** @brief 返回身份一致、排序唯一且可安全复制进 world executor 的值快照。 */
  [[nodiscard]] virtual ServiceResult<WorldAccessSnapshot> Load(
      domain::PlayerId player_id) const = 0;
};

/** @brief 校验数据库返回的世界访问事实，不向运行时泄露数据库行。 */
class WorldAccessService final : public WorldAccessLoader {
 public:
  /** @brief 注入生命周期覆盖本服务的只读端口。 */
  explicit WorldAccessService(WorldAccessReader& reader) noexcept;

  [[nodiscard]] std::optional<ServiceError> ActivateDevice(
      domain::PlayerId player, std::string_view code) const override {
    return reader_.ActivateDevice(player, code);
  }

  /** @brief 读取并验证玩家、屏蔽 ID、排序和唯一性。 */
  [[nodiscard]] ServiceResult<WorldAccessSnapshot> Load(
      domain::PlayerId player_id) const override;

 private:
  WorldAccessReader& reader_;  ///< 由 composition root 拥有的数据库 adapter。
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_WORLD_ACCESS_H_
