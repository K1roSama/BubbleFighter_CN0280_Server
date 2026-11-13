#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_SERVICE_RUNTIME_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_SERVICE_RUNTIME_H_

/**
 * @file service_runtime.h
 * @brief 定义服务进程的最小、可测试启动与有界停止状态机。
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

#include "application/error.h"
#include "application/event_logger.h"

namespace bf::runtime {

/**
 * @brief 服务装配层可观察的生命周期状态。
 * @note P2/P3 的网络、数据库池和房间执行器必须由同一状态机按逆依赖顺序停止。
 */
enum class RuntimeState : std::uint8_t {
  kCreated,
  kRunning,
  kStopping,
  kStopped,
};

/**
 * @brief 协调服务组件的启动、就绪和停止边界。
 * @note 入口拥有本对象及 logger；本对象只借用 logger，logger
 * 必须比本对象存活更久。
 * @note 所有方法可由不同控制线程调用，内部状态转换使用互斥量串行化。
 */
class ServiceRuntime final {
 public:
  /** @brief 使用外部拥有的日志端口创建尚未启动的运行时。 */
  explicit ServiceRuntime(application::EventLogger& logger) noexcept;

  /**
   * @brief 将运行时从 created 转换为 running。
   * @return 首次启动成功时为空；重复或越序调用返回 invalid_state。
   */
  [[nodiscard]] std::optional<application::ServiceError> Start();

  /**
   * @brief 停止接受新工作并进入 stopping。
   * @param reason 可记录的脱敏停止原因；重复请求保持幂等。
   */
  void RequestStop(std::string_view reason) noexcept;

  /**
   * @brief 在外部组件已按顺序排空后完成 stopping 到 stopped 的转换。
   * @note P1 尚无网络和数据库组件，入口可在 request_stop 后立即调用。
   */
  void CompleteStop() noexcept;

  /** @brief 返回当前生命周期状态的线程安全快照。 */
  [[nodiscard]] RuntimeState State() const noexcept;

  /** @brief 仅当状态为 running 时返回 true。 */
  [[nodiscard]] bool IsReady() const noexcept;

 private:
  application::EventLogger& logger_;
  mutable std::mutex mutex_;
  RuntimeState state_{RuntimeState::kCreated};
};

/** @brief 将运行时状态转换为稳定英文名称。 */
[[nodiscard]] std::string_view RuntimeStateName(RuntimeState state) noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_SERVICE_RUNTIME_H_
