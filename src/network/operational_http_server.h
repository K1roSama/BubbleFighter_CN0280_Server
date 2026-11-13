#ifndef BUBBLEFIGHTER_SERVER_NETWORK_OPERATIONAL_HTTP_SERVER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_OPERATIONAL_HTTP_SERVER_H_

/**
 * @file operational_http_server.h
 * @brief 定义仅供运维探针使用的有界 HTTP 健康、就绪和指标端点。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "application/error.h"
#include "application/event_logger.h"

namespace bf::infrastructure::network {

/** @brief 一次 `/ready` 或 `/metrics` 请求读取的纯数值进程快照。 */
struct OperationalSnapshot final {
  bool ready{false};                        ///< 所有正式监听和依赖是否已就绪。
  std::size_t gateway_connections{0U};      ///< 当前主网关 TCP 连接数。
  std::size_t clan_connections{0U};         ///< 当前 Nx 战队连接数。
  std::size_t database_pool_size{0U};       ///< 固定 MySQL 连接池大小。
  std::size_t database_pool_available{0U};  ///< 当前可立即租用的数据库连接数。
  std::size_t database_tasks_queued{0U};    ///< 等待数据库工作线程的任务数。
  std::size_t world_tasks_queued{0U};       ///< 等待世界串行线程的任务数。
  std::size_t active_rooms{0U};             ///< 当前活动房间数。
  std::size_t plaza_players{0U};            ///< 当前广场玩家数。
  std::size_t world_timers{0U};             ///< 当前有效房间计时器数。
  std::size_t pending_match_starts{0U};     ///< 正在持久化/安装的开局数。
  std::size_t pending_match_settlements{
      0U};  ///< 正在写入耐久终局日志的房间数。
  std::size_t pending_match_compensations{0U};  ///< 正在补偿失败开局的房间数。
  std::uint64_t movement_coalesced_total{0U};   ///< 出站移动状态累计替换数。
  std::uint64_t movement_dropped_total{0U};  ///< 出站移动状态累计主动丢弃数。
  std::uint64_t settlements_accepted{0U};    ///< 结算调度器累计接受数。
  std::uint64_t settlements_rejected_overload{
      0U};                                  ///< 结算调度器累计过载拒绝数。
  std::uint64_t settlements_completed{0U};  ///< 结算调度器累计完成数。
  std::uint64_t settlements_failed{0U};     ///< 结算调度器累计失败数。
  std::size_t settlements_queued{0U};       ///< 等待执行的结算数。
  std::size_t settlements_active{0U};       ///< 正在执行的结算数。
  std::size_t ds_healthy_nodes{0U};    ///< 已认证且持有有效租约的可分配节点。
  std::size_t ds_queued_commands{0U};  ///< 独立房间入口已接管的命令数。
  std::size_t ds_queued_bytes{0U};     ///< 独立房间入口计费字节数。
  std::uint64_t ds_rejected_commands{0U};  ///< 独立房间入口预算拒绝累计数。
  std::uint64_t ds_failed_rooms{0U};       ///< 已隔离失败的独立房间累计数。
  std::uint64_t ds_queue_samples{0U};      ///< 房间命令排队观测数。
  std::uint64_t ds_queue_p99_us{0U};       ///< 排队时间 P99 桶上界，微秒。
  std::uint64_t ds_immediate_rpc_p99_us{
      0U};  ///< 非移动命令 RPC 往返 P99 桶上界，微秒。
  std::uint64_t ds_worst_room_timer_p99_us{
      0U};                  ///< 活跃房间定时器迟到 P99 的最大值，微秒。
  bool ds_draining{false};  ///< 是否停止接纳新的建房、入房。
};

/** @brief 运维 HTTP 监听和单请求资源上限。 */
struct OperationalHttpServerOptions final {
  std::string bind_address;             ///< 建议仅绑定回环或受控管理网络。
  std::uint16_t port{0U};               ///< 运维端口；0 只供隔离测试自动分配。
  std::size_t maximum_connections{0U};  ///< 同时读取请求的连接硬上限。
  std::size_t maximum_request_bytes{0U};  ///< 请求头字节上限；不接受请求正文。
};

/** @brief 线程安全、无阻塞地生成当前运维快照。 */
using OperationalSnapshotProvider = std::function<OperationalSnapshot()>;

/**
 * @brief 提供 `/live`、`/ready` 和 Prometheus 文本 `/metrics` 的最小 HTTP/1.x
 * 服务。
 * @note 本端点不提供管理写操作、数据库查询或秘密配置；每连接只处理一个有界 GET
 * 后关闭。 snapshot provider 只可读取原子/加锁快照，不能执行阻塞 SQL。
 */
class OperationalHttpServer final {
 public:
  /** @brief 保存配置、快照回调和日志借用；尚不绑定端口。 */
  OperationalHttpServer(OperationalHttpServerOptions options,
                        OperationalSnapshotProvider snapshot_provider,
                        application::EventLogger& logger);
  ~OperationalHttpServer();

  OperationalHttpServer(const OperationalHttpServer&) = delete;
  OperationalHttpServer& operator=(const OperationalHttpServer&) = delete;
  OperationalHttpServer(OperationalHttpServer&&) = delete;
  OperationalHttpServer& operator=(OperationalHttpServer&&) = delete;

  /** @brief 校验参数、绑定端口并启动单一 I/O 线程。 */
  [[nodiscard]] std::optional<application::ServiceError> Start();

  /** @brief 幂等停止 accept、关闭活动探针连接并合并 I/O 线程。 */
  void Stop(std::chrono::milliseconds grace) noexcept;

  /** @brief 返回实际绑定端口；启动前为零。 */
  [[nodiscard]] std::uint16_t LocalPort() const noexcept;

  /** @brief 返回尚未关闭的运维连接数。 */
  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept;

 private:
  /** @brief 隐藏 Boost.Asio acceptor、Session 和线程所有权。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_OPERATIONAL_HTTP_SERVER_H_
