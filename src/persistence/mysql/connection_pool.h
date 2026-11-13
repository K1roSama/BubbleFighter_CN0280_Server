#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CONNECTION_POOL_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CONNECTION_POOL_H_

/**
 * @file connection_pool.h
 * @brief 定义有界 MySQL 连接池和不可复制的 RAII 连接租约。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "application/error.h"
#include "features/configuration/mysql_settings.h"

namespace bf::infrastructure::mysql {
class Connection;

/** @brief 连接池容量、获取超时和会话级查询超时。 */
struct ConnectionPoolOptions final {
  ConnectionSettings connection;
  std::size_t size{0U};  ///< 预先建立的专用工作连接数。
  std::chrono::milliseconds acquire_timeout{
      0};                              ///< 队列等待空闲连接的最长时间。
  std::uint32_t query_timeout_ms{0U};  ///< SELECT、连接和锁等待超时基线。
};

/**
 * @brief 有界、线程安全的 MySQL 连接池。
 * @note 每个 Lease 只能在一个数据库工作线程上使用；归还前不得将 connection、
 *       transaction、row 或 field 引用逃出。池析构前组装层必须先排空工作池。
 */
class MySqlConnectionPool final {
 public:
  /** @brief 供移动租约共享存活状态的私有池实现。 */
  class SharedState;

  /** @brief 独占一条 MySQL 连接的移动型 RAII 租约。 */
  class Lease final {
   public:
    /** @brief 构造不持有连接的空租约。 */
    Lease() noexcept = default;

    /** @brief 归还仍持有的连接；空租约不执行操作。 */
    ~Lease();

    /** @brief 转移连接所有权，并将来源租约置空。 */
    Lease(Lease&& other) noexcept;

    /** @brief 先归还当前连接，再转移来源租约的连接所有权。 */
    Lease& operator=(Lease&& other) noexcept;

    /** @brief 租约具有独占所有权，因此禁止复制构造。 */
    Lease(const Lease&) = delete;

    /** @brief 租约具有独占所有权，因此禁止复制赋值。 */
    Lease& operator=(const Lease&) = delete;

    /** @brief 返回本租约独占的连接；空/已移动租约抛 `logic_error`。 */
    [[nodiscard]] bf::infrastructure::mysql::Connection& Connection() const;

    /** @brief 返回租约是否仍持有连接。 */
    [[nodiscard]] bool Active() const noexcept;

   private:
    friend class MySqlConnectionPool;
    Lease(std::shared_ptr<SharedState> state, std::size_t index) noexcept;
    void Release() noexcept;

    std::shared_ptr<SharedState> state_;
    std::size_t index_{0U};
    bool active_{false};
  };

  /**
   * @brief 校验参数并预先打开全部连接。
   * @return 任一连接或会话超时配置失败时整池创建失败，不返回部分池。
   */
  [[nodiscard]] static application::ServiceResult<
      std::unique_ptr<MySqlConnectionPool>>
  Create(ConnectionPoolOptions options);

  /** @brief 停止连接池，并在共享状态最后一个引用释放后关闭连接。 */
  ~MySqlConnectionPool();

  /** @brief 连接池管理固定共享状态，因此禁止复制构造。 */
  MySqlConnectionPool(const MySqlConnectionPool&) = delete;

  /** @brief 连接池管理固定共享状态，因此禁止复制赋值。 */
  MySqlConnectionPool& operator=(const MySqlConnectionPool&) = delete;

  /** @brief 连接池地址由租约引用，生命周期内禁止移动构造。 */
  MySqlConnectionPool(MySqlConnectionPool&&) = delete;

  /** @brief 连接池地址由租约引用，生命周期内禁止移动赋值。 */
  MySqlConnectionPool& operator=(MySqlConnectionPool&&) = delete;

  /** @brief 在配置时限内取得独占租约；耗尽返回可重试过载错误。 */
  [[nodiscard]] application::ServiceResult<Lease> Acquire();

  /** @brief 停止新获取并唤醒等待者；已发出租约析构时仍会安全释放。 */
  void Stop() noexcept;

  /** @brief 返回创建时固定的连接数。 */
  [[nodiscard]] std::size_t Size() const noexcept;

  /** @brief 返回当前可立即获取的连接数快照。 */
  [[nodiscard]] std::size_t Available() const noexcept;

 private:
  explicit MySqlConnectionPool(std::shared_ptr<SharedState> state) noexcept;
  std::shared_ptr<SharedState> state_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CONNECTION_POOL_H_
