#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOGIN_TICKET_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOGIN_TICKET_REPOSITORY_H_

/**
 * @file login_ticket_repository.h
 * @brief 定义使用 MySQL 实现的网关一次性登录票据事务工厂。
 */

#include "application/event_logger.h"
#include "features/authentication/authentication.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/** @brief 持久化每日领取记录，原子增加泡泡点和金币各 99999。 */
class MySqlDailyLoginRewardGranter final
    : public application::DailyLoginRewardGranter {
 public:
  explicit MySqlDailyLoginRewardGranter(
      MySqlConnectionPool& pool,
      application::EventLogger* logger = nullptr) noexcept
      : pool_(pool), logger_(logger) {}

  [[nodiscard]] std::optional<application::ServiceError> Grant(
      domain::PlayerId player_id,
      std::chrono::system_clock::time_point now) override;
  /** @brief 事务成功后返回本次新插入的出勤贡献，用于原生S2C25通知。 */
  [[nodiscard]] application::ServiceResult<application::DailyLoginRewardResult>
  GrantWithResult(domain::PlayerId player_id,
                  std::chrono::system_clock::time_point now) override;

 private:
  void Log(bool success, const std::string& message) const;
  MySqlConnectionPool& pool_;
  application::EventLogger* logger_;
};

/**
 * @brief 从有界连接池取得独占连接，并创建票据消费短事务。
 * @note 本类只借用连接池；所有已开始事务必须在连接池停止/析构前销毁。
 */
class MySqlLoginTicketUnitOfWork final
    : public application::LoginTicketUnitOfWork {
 public:
  /** @brief 注入比本适配器存活更久的连接池。 */
  explicit MySqlLoginTicketUnitOfWork(MySqlConnectionPool& pool,
                                      std::uint32_t region_id = 1U) noexcept;

  /** @brief 获取连接并开始可回滚事务；过载/存储失败返回脱敏错误。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<application::LoginTicketTransaction>>
  Begin() override;

 private:
  std::uint32_t region_id_;  ///< 票据仅能进入指定大区。
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOGIN_TICKET_REPOSITORY_H_
