#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ADMIN_MAINTENANCE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ADMIN_MAINTENANCE_H_

/**
 * @file admin_maintenance.h
 * @brief 定义需要显式授权与数据库审计的 MySQL 维护操作。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "application/error.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 执行到期战队解散请求所需的完整审计上下文。 */
struct ExecuteClanDisbandsCommand final {
  std::string actor;   ///< 经过外部认证的操作员或调度任务稳定标识。
  std::string reason;  ///< 非空处置原因；不得包含密码、票据或数据库连接串。
  std::string
      business_id;  ///< 调用方生成的规范 UUID；重复值只返回第一次执行结果。
  std::size_t limit{100U};  ///< 单事务最多处理的请求数，范围为 1 至 1000。
  bool native_requests_only{
      false};  ///< 启动维护只处理非导入、仍由当前会长本人授权的普通家族请求。
};

/** @brief 一次显式战队解散维护事务的结果。 */
struct ExecuteClanDisbandsReport final {
  std::vector<std::uint64_t>
      clan_ids;                 ///< 本业务 UUID 首次执行时选中的稳定战队编号。
  bool already_applied{false};  ///< 是否命中同一业务 UUID 的既有审计结果。
};

/**
 * @brief 原子执行已到 `execute_after` 的待解散战队，并写入战队与管理审计。
 * @param pool 管理命令专用的有界 MySQL 连接池。
 * @param command 含操作者、原因、幂等业务 UUID 和批量上限的命令。
 * @return 成功报告，或不含数据库 URI/SQL 明文的稳定错误。
 * @note 多个执行器可并行运行；事务通过 `FOR UPDATE SKIP LOCKED`
 * 避免重复处理。同一 business_id
 * 重试返回首次保存的战队列表，不再次改变成员关系。
 * @warning 命令不会主动断开在线 Nx
 * 会话；显式管理命令须排空服务，启动维护须在开放端口前运行。
 */
[[nodiscard]] application::ServiceResult<ExecuteClanDisbandsReport>
ExecuteDueClanDisbands(MySqlConnectionPool& pool,
                       const ExecuteClanDisbandsCommand& command);

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_ADMIN_MAINTENANCE_H_
