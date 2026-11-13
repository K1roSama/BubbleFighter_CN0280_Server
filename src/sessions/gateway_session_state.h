#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_SESSION_STATE_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_SESSION_STATE_H_

/**
 * @file gateway_session_state.h
 * @brief 定义网关连接从握手到大厅初始化和关闭的显式阶段状态机。
 */

#include <cstdint>
#include <optional>
#include <string_view>

#include "application/error.h"

namespace bf::runtime {

/** @brief 网关 Session 可持久观察的协议阶段。 */
enum class GatewaySessionPhase : std::uint8_t {
  kConnected,
  kPatchReady,
  kCreatingCharacter,
  kAuthenticated,
  kMenuReady,
  kClosing,
  kClosed,
};

/** @brief 登录用例成功后选择正常认证或首次角色创建分支。 */
enum class LoginDisposition : std::uint8_t {
  kAuthenticated,
  kCharacterCreationRequired,
};

/**
 * @brief 串行验证登录前后的网关阶段转换。
 * @note 本对象不加锁，未来必须由所属 Session strand 独占调用；网络 completion
 * 只能把事件 投递回该 executor，不能直接从其他线程修改阶段。
 * @note
 * 状态机只表达顺序，不验证票据、昵称或数据库事实；这些属于应用用例和事务。
 */
class GatewaySessionState final {
 public:
  /** @brief 建立处于 connected、尚未完成 C2S1 的会话状态。 */
  GatewaySessionState() = default;

  /**
   * @brief 在协议/应用层已验证 C2S1 build 后进入 patch_ready。
   * @note 本方法只记录阶段；支持的构建号由协议版本策略集中决定。
   */
  [[nodiscard]] std::optional<application::ServiceError> AcceptPatchHandshake();

  /**
   * @brief 提交登录用例的成功结果并选择首次角色或已认证分支。
   * @param disposition 数据库登录结果决定的后续阶段。
   */
  [[nodiscard]] std::optional<application::ServiceError> CompleteLogin(
      LoginDisposition disposition);

  /** @brief 在 creating_character 阶段原子创建角色成功后进入 authenticated。 */
  [[nodiscard]] std::optional<application::ServiceError>
  CompleteCharacterCreation();

  /** @brief 接受一次 C2S102 初始化并从 authenticated 进入 menu_ready。 */
  [[nodiscard]] std::optional<application::ServiceError>
  AcceptPostLoginBootstrap();

  /** @brief 幂等进入 closing；停止读取新业务包并等待写队列/异步任务收束。 */
  void BeginClose() noexcept;

  /** @brief 在 socket、计时器和回调均已失效后从 closing 进入 closed。 */
  void CompleteClose() noexcept;

  /** @brief 返回当前阶段；只能在所属 executor 上读取。 */
  [[nodiscard]] GatewaySessionPhase Phase() const noexcept;

 private:
  GatewaySessionPhase phase_{GatewaySessionPhase::kConnected};
};

/** @brief 返回用于日志、指标和测试的稳定网关阶段名。 */
[[nodiscard]] std::string_view GatewaySessionPhaseName(
    GatewaySessionPhase phase) noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_SESSION_STATE_H_
