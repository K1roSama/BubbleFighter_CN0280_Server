#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_DISPATCHER_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_DISPATCHER_H_

/**
 * @file match_settlement_dispatcher.h
 * @brief 定义房间执行器到阻塞数据库结算用例之间的有界异步边界。
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "application/error.h"
#include "application/event_logger.h"
#include "features/battle/match_settlement.h"

namespace bf::runtime {

/** @brief 结算调度器的生命周期；停止后不能重新启动或接收任务。 */
enum class MatchSettlementDispatcherState : std::uint8_t {
  kCreated,
  kRunning,
  kStopping,
  kStopped,
};

/** @brief 数据库工作线程完成后移交给调用侧执行器的命令与拥有型结果。 */
struct MatchSettlementCompletion final {
  application::MatchSettlementCommand
      command;  ///< 原命令完整副本；失败时仍可可靠重试。
  application::ServiceResult<application::MatchSettlementResult>
      result;  ///< 成功值或安全错误。
};

/**
 * @brief 接收数据库工作线程上的结算完成事件。
 * @note 实现不得直接修改 Session、房间或网络对象；应把拥有型 completion
 * 投递回对应执行器。
 * @note 方法必须有界且不抛异常，否则会阻塞数据库工作线程及有界停机。
 */
class MatchSettlementCompletionSink {
 public:
  virtual ~MatchSettlementCompletionSink() = default;

  /**
   * @brief 接管一份完成事件；调用发生在调度器拥有的数据库工作线程。
   * @note 失败事件仍携带原命令；sink
   * 必须转交重试/告警所有者，不能只记录错误后丢弃。
   */
  virtual void Publish(MatchSettlementCompletion completion) noexcept = 0;
};

/**
 * @brief 接收已耐久保存的终局命令并尝试交给异步执行器。
 * @note 失败不转移调用方所有权；上层可依靠 MatchSettlementJournal
 * 在稍后或重启后重投。
 */
class MatchSettlementSubmissionSink {
 public:
  virtual ~MatchSettlementSubmissionSink() = default;

  /** @brief 尝试接管命令副本；过载或未运行时返回错误且不得丢失输入。 */
  [[nodiscard]] virtual std::optional<application::ServiceError> Submit(
      const application::MatchSettlementCommand& command) = 0;
};

/** @brief 结算工作线程和等待队列的资源上限。 */
struct MatchSettlementDispatcherOptions final {
  std::size_t worker_count{0U};    ///< 并行执行结算事务的工作线程数。
  std::size_t queue_capacity{0U};  ///< 等待执行的命令上限，不含正在执行的命令。
};

/** @brief 线程安全的调度器观测快照。 */
struct MatchSettlementDispatcherMetrics final {
  std::uint64_t accepted{0U};  ///< 已取得命令副本并承诺执行的累计数量。
  std::uint64_t rejected_overload{0U};  ///< 因等待队列已满而拒绝的累计数量。
  std::uint64_t completed{0U};          ///< 已向 sink 发布完成事件的累计数量。
  std::uint64_t failed{0U};  ///< completed 中返回业务/存储错误的数量。
  std::size_t queued{0U};    ///< 当前等待执行的命令数。
  std::size_t active{0U};    ///< 当前正在执行事务的命令数。
};

/**
 * @brief 用固定工作线程排空拥有型终局命令的有界调度器。
 * @note 对象拥有线程和命令副本，只借用
 * executor、sink、logger；三者必须比本对象存活更久。
 * @note worker_count 大于一时 executor 及其 UnitOfWork::begin
 * 必须允许并发调用；每次调用仍须
 *       独占一个连接和完整事务。数据库锁负责不同对局涉及同一玩家时的最终串行化。
 * @note shutdown 停止接收新任务并排空已接收任务；数据库语句超时和 sink
 * 的有界实现共同保证 停机能够收敛。本类不会把未完成命令静默丢弃。
 * @warning 队列仅保证进程内所有权和正常停机排空，不是磁盘
 * outbox。进程崩溃恢复仍要求 MySQL adapter 在发奖前持久化
 * terminal_snapshot，并由恢复任务扫描未完成状态。
 */
class MatchSettlementDispatcher final : public MatchSettlementSubmissionSink {
 public:
  /**
   * @brief 创建尚未启动的调度器。
   * @throws std::invalid_argument worker_count/queue_capacity
   * 为零或队列小于工作线程数。
   */
  MatchSettlementDispatcher(
      MatchSettlementDispatcherOptions options,
      const application::MatchSettlementExecutor& executor,
      MatchSettlementCompletionSink& sink, application::EventLogger& logger);

  /** @brief 停止接收、排空队列并回收所有工作线程。 */
  ~MatchSettlementDispatcher();

  MatchSettlementDispatcher(const MatchSettlementDispatcher&) = delete;
  MatchSettlementDispatcher& operator=(const MatchSettlementDispatcher&) =
      delete;
  MatchSettlementDispatcher(MatchSettlementDispatcher&&) = delete;
  MatchSettlementDispatcher& operator=(MatchSettlementDispatcher&&) = delete;

  /**
   * @brief 创建固定工作线程并进入 running；只能调用一次。
   * @return 成功为空；状态错误或线程创建失败返回安全错误。
   */
  [[nodiscard]] std::optional<application::ServiceError> Start();

  /**
   * @brief 尝试复制并接管一份不可变终局命令，不在调用线程执行数据库工作。
   * @return 接受成功为空；未运行返回 invalid_state，队列已满返回可重试
   * overloaded。
   * @note
   * 失败时输入仍由调用方完整拥有，可写入可靠重试介质或稍后重投；成功后即使房间销毁，
   *       调度器持有的值副本仍有效。
   */
  [[nodiscard]] std::optional<application::ServiceError> Submit(
      const application::MatchSettlementCommand& command) override;

  /** @brief 幂等停止接收并同步排空所有已接受任务。 */
  void Shutdown() noexcept;

  /** @brief 返回当前生命周期状态。 */
  [[nodiscard]] MatchSettlementDispatcherState State() const noexcept;

  /** @brief 返回累计计数和当前队列/执行数量的一致快照。 */
  [[nodiscard]] MatchSettlementDispatcherMetrics Metrics() const noexcept;

 private:
  /** @brief 隐藏线程、条件变量与队列实现，保持公开头的依赖边界稳定。 */
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/** @brief 将调度器状态转换为稳定英文名称。 */
[[nodiscard]] std::string_view MatchSettlementDispatcherStateName(
    MatchSettlementDispatcherState state) noexcept;

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_SETTLEMENT_DISPATCHER_H_
