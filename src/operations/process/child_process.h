#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_CHILD_PROCESS_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_CHILD_PROCESS_H_

/**
 * @file child_process.h
 * @brief 定义仅监管本节点创建的子进程及其资源采样接口。
 */
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf::infrastructure::process {
/** @brief 一次非阻塞子进程资源快照。 */
struct ChildProcessMetrics final {
  std::uint64_t process_id{0U};
  std::uint64_t private_bytes{0U};
  std::uint64_t handles{0U};
  std::uint64_t cpu_microseconds{0U};
  std::optional<int> exit_code;
};

/** @brief 移动所有权的进程监管句柄，析构只结束此对象创建的进程。 */
class ChildProcess final {
 public:
  /** @brief 无 shell 创建隐藏进程；Windows 先绑定 Job Object 再恢复主线程。 */
  static std::unique_ptr<ChildProcess> Launch(
      const std::filesystem::path& executable,
      const std::vector<std::string>& arguments,
      std::uint64_t memory_limit_bytes);
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  /** @brief 非阻塞读取有上限的标准输出；仅用于启动握手。 */
  [[nodiscard]] std::string ReadOutput();
  /** @brief 采样存活、内存和句柄，不等待子进程结束。 */
  [[nodiscard]] ChildProcessMetrics Metrics();
  /** @brief 终止所属进程并回收系统句柄，幂等。 */
  void Terminate() noexcept;

 private:
  /** @brief 隐藏平台专属进程、管道及 Job Object 句柄。 */
  class Impl;
  explicit ChildProcess(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::process

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_CHILD_PROCESS_H_
