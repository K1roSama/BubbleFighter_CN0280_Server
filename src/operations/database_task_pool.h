#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_DATABASE_TASK_POOL_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_DATABASE_TASK_POOL_H_

/**
 * @file database_task_pool.h
 * @brief 定义将同步 MySQL/密码工作与 Asio I/O 线程隔离的有界任务池。
 */

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "application/background_task_executor.h"
#include "application/event_logger.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 拥有固定工作线程和有界等待队列的阻塞数据库任务执行器。
 * @note 任务可同步使用连接池，但不得直接触碰 Session 状态；完成结果应通过
 *       `PacketCompletion` 返回，由 TCP 层 post 回原
 * strand。析构会先拒绝新任务并排空已接受任务。
 */
class DatabaseTaskPool final : public application::BackgroundTaskExecutor {
 public:
  /** @brief 以正数线程数和等待任务上限启动工作池；无效参数抛出异常。 */
  DatabaseTaskPool(std::size_t worker_count, std::size_t queue_capacity,
                   application::EventLogger& logger);
  ~DatabaseTaskPool();

  DatabaseTaskPool(const DatabaseTaskPool&) = delete;
  DatabaseTaskPool& operator=(const DatabaseTaskPool&) = delete;
  DatabaseTaskPool(DatabaseTaskPool&&) = delete;
  DatabaseTaskPool& operator=(DatabaseTaskPool&&) = delete;

  /** @brief 非阻塞提交拥有型任务；队列满或停机后返回显式错误。 */
  [[nodiscard]] std::optional<application::ServiceError> Post(
      std::function<void()> task) override;

  /** @brief 幂等停止接收，排空队列并合并全部工作线程。 */
  void DrainAndStop() noexcept;

  /** @brief 返回已创建的固定工作线程数。 */
  [[nodiscard]] std::size_t WorkerCount() const noexcept;

  /** @brief 返回尚未开始执行的任务数快照。 */
  [[nodiscard]] std::size_t QueuedTaskCount() const noexcept;

 private:
  /** @brief 隔离线程、条件变量和有界任务队列的实现对象。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_DATABASE_TASK_POOL_H_
