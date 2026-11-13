#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LAUNCHER_AUTHENTICATION_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LAUNCHER_AUTHENTICATION_REPOSITORY_H_

/**
 * @file launcher_authentication_repository.h
 * @brief 定义启动器注册、凭据读取和登录票据签发的 MySQL 适配器。
 */

#include "features/authentication/launcher_authentication.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 从连接池创建账号注册短事务。
 * @note 一个事务原子创建账号、凭据、未建角资料、进度、三钱包及其期初流水。
 */
class MySqlRegisterAccountUnitOfWork final
    : public application::RegisterAccountUnitOfWork {
 public:
  /** @brief 注入比本适配器存活更久的连接池。 */
  explicit MySqlRegisterAccountUnitOfWork(MySqlConnectionPool& pool) noexcept;

  /** @brief 获取独占连接并开始注册事务；未提交析构会回滚。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::RegisterAccountTransaction>>
  Begin() override;

 private:
  MySqlConnectionPool& pool_;  ///< 由 composition root 拥有的有界连接池。
};

/** @brief 在一个只读事务中按规范登录键读取账号和 PBKDF2 凭据。 */
class MySqlAccountCredentialReader final
    : public application::AccountCredentialReader {
 public:
  /** @brief 注入比本 reader 存活更久的连接池。 */
  explicit MySqlAccountCredentialReader(MySqlConnectionPool& pool) noexcept;

  /** @brief 不存在返回空值；损坏字节长度由 adapter 拒绝，不进入密码计算。 */
  [[nodiscard]] application::ServiceResult<
      std::optional<application::StoredAccountCredential>>
  FindByLoginKey(std::string_view login_key) override;

 private:
  MySqlConnectionPool& pool_;  ///< 每次读取只借用一条连接。
};

/**
 * @brief 创建“锁账号、撤销旧票据并签发新摘要”的 MySQL 短事务。
 * @note 只接收 SHA-256 摘要；原始票据永不进入数据库适配器或日志。
 */
class MySqlIssueLoginTicketUnitOfWork final
    : public application::IssueLoginTicketUnitOfWork {
 public:
  /** @brief 注入比本适配器存活更久的连接池。 */
  explicit MySqlIssueLoginTicketUnitOfWork(
      MySqlConnectionPool& pool, std::uint32_t region_id = 1U) noexcept;

  /** @brief 获取独占连接并开始签发事务；未提交析构会回滚。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::IssueLoginTicketTransaction>>
  Begin() override;

 private:
  std::uint32_t region_id_;    ///< 票据仅能进入指定大区。
  MySqlConnectionPool& pool_;  ///< 由 composition root 拥有的有界连接池。
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LAUNCHER_AUTHENTICATION_REPOSITORY_H_
