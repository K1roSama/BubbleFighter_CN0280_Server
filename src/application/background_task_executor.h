#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_BACKGROUND_TASK_EXECUTOR_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_BACKGROUND_TASK_EXECUTOR_H_

/**
 * @file background_task_executor.h
 * @brief 定义运行时把阻塞持久化工作移出串行世界线程的有界执行端口。
 */

#include <functional>
#include <optional>

#include "application/error.h"

namespace bf::application {

/**
 * @brief 接受拥有型阻塞任务的非阻塞、有界后台执行接口。
 * @note 实现不得在调用线程执行任务；停机时必须排空已受理任务，使其 completion
 * 能投回拥有者。
 */
class BackgroundTaskExecutor {
 public:
  virtual ~BackgroundTaskExecutor() = default;

  /** @brief 接收任务；过载或停机返回错误且调用方仍拥有未执行语义。 */
  [[nodiscard]] virtual std::optional<ServiceError> Post(
      std::function<void()> task) = 0;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_BACKGROUND_TASK_EXECUTOR_H_
