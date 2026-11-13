#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_ASYNC_FILE_LOGGER_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_ASYNC_FILE_LOGGER_H_

/**
 * @file async_file_logger.h
 * @brief 声明具有有界队列、轮转和日志注入防护的异步文件日志实现。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "application/event_logger.h"
#include "application/system_clock.h"

namespace bf::infrastructure::logging {

/** @brief 在文件日志尚未就绪时，以统一的本地时间格式输出控制台事件。 */
void WriteConsoleLog(application::LogLevel level, std::string_view message,
                     std::string_view correlation_id = {}) noexcept;

/** @brief 异步日志线程所需的不可变资源配置。 */
struct AsyncLoggerOptions final {
  std::filesystem::path
      path;  ///< 日志路径；文件名中的 {date} 按记录的本地日期展开。
  application::LogLevel minimum_level{
      application::LogLevel::kInfo};  ///< 最低记录级别。
  std::size_t queue_capacity{0};      ///< 内存中最多等待的记录条数。
  std::uint64_t rotate_bytes{0};      ///< 当前文件达到该大小后轮转。
  std::size_t retained_files{0};      ///< 最多保留的历史文件数量。
  bool echo_console{false};           ///< 是否同时写入标准错误流。
  const application::SystemClock* clock{
      nullptr};  ///< 可选借用时钟，空指针使用系统时间。
};

/**
 * @brief 单工作线程消费有界队列的日志端口实现。
 * @note
 * 对象独占队列、文件和工作线程；析构会停止接收、排空已接收记录并关闭文件。
 * @note 队列满时 log 不阻塞业务线程，而是增加 dropped_records 计数。
 */
class AsyncFileLogger final : public application::EventLogger {
 public:
  /**
   * @brief 创建日志目录、打开文件并启动唯一消费线程。
   * @param options 已通过服务配置校验的日志选项。
   * @throws std::invalid_argument 资源上限无效。
   * @throws std::runtime_error 目录或日志文件无法创建。
   */
  explicit AsyncFileLogger(AsyncLoggerOptions options);

  /** @brief 排空已接收日志并释放线程和文件。 */
  ~AsyncFileLogger() override;

  AsyncFileLogger(const AsyncFileLogger&) = delete;
  AsyncFileLogger& operator=(const AsyncFileLogger&) = delete;
  AsyncFileLogger(AsyncFileLogger&&) = delete;
  AsyncFileLogger& operator=(AsyncFileLogger&&) = delete;

  /**
   * @brief 尝试把事件复制到有界队列，过滤低级别并转义控制字符。
   * @note 此方法不抛异常；内存不足、停止中或队列满均记入丢弃计数。
   */
  void Log(application::LogLevel level, std::string_view message,
           std::string_view correlation_id = {}) noexcept override;

  /** @brief 返回因过载、停止或分配失败而未入队的累计记录数。 */
  [[nodiscard]] std::uint64_t DroppedRecords() const noexcept;

  /** @brief 幂等停止接收、排空队列并等待工作线程退出。 */
  void Shutdown() noexcept;

 private:
  /** @brief 隐藏日志队列、文件和线程的具体实现，避免向调用方暴露同步原语。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<std::uint64_t> dropped_records_{0};
};

}  // namespace bf::infrastructure::logging

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_ASYNC_FILE_LOGGER_H_
