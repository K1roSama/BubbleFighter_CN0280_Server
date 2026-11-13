#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_DS_AGENT_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_DS_AGENT_H_

/**
 * @file ds_agent.h
 * @brief 定义单节点房间进程监管、容量准入和优雅排空。
 */
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace bf::infrastructure::process {
/** @brief 在指定事件循环运行节点控制与子进程监管。 */
class DsAgent final {
 public:
  /** @brief 读取有界私有配置并校验证书与容量预算。 */
  DsAgent(boost::asio::io_context& io,
          const std::filesystem::path& config_path);
  ~DsAgent();
  DsAgent(const DsAgent&) = delete;
  DsAgent& operator=(const DsAgent&) = delete;
  /** @brief 返回实际绑定的控制端口，支持自动分配端口。 */
  [[nodiscard]] std::uint16_t Port() const;
  /** @brief 启动子进程预热、异步控制和资源采样。 */
  void Start();
  /** @brief 关闭节点控制并回收本 Agent 创建的子进程。 */
  void Stop();

 private:
  /** @brief 隐藏节点通信、容量和子进程生命周期状态。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::process

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_PROCESS_DS_AGENT_H_
