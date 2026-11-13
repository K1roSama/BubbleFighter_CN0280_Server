#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_AUTHENTICATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_AUTHENTICATION_H_

/**
 * @file authentication.h
 * @brief 定义启动器票据摘要和网关一次性消费用例的端口与结果。
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"
#include "features/characters/character.h"

namespace bf::application {

/** @brief SHA-256 的固定 32 字节摘要值。 */
using Sha256Digest = std::array<std::uint8_t, 32U>;

/**
 * @brief 密码学 SHA-256 端口。
 * @note 正式实现必须调用操作系统或受维护密码库，不得以协议
 * cipher、自制散列或假摘要替代。
 */
class Sha256Hasher {
 public:
  virtual ~Sha256Hasher() = default;

  /** @brief 对完整输入计算 SHA-256；错误内容不得复制输入字节。 */
  [[nodiscard]] virtual ServiceResult<Sha256Digest> Hash(
      std::span<const std::uint8_t> input) const = 0;
};

/** @brief 票据成功消费后从数据库返回的账号和玩家资料快照。 */
struct AuthenticatedPlayerSnapshot final {
  domain::AccountId account_id;      ///< 票据绑定账号。
  domain::PlayerId player_id;        ///< 一对一关联的游戏玩家。
  std::string account_name;          ///< 登录成功结构使用的账号展示名。
  std::string nickname;              ///< 当前玩家展示昵称。
  bool is_character_created{false};  ///< 是否进入首次建角分支。
  std::optional<domain::InitialFighter>
      initial_fighter;  ///< 已建角时的 HUD 回退角色。
  std::string machine_code;
};

/**
 * @brief 一次登录票据消费的数据库事务端口。
 * @note
 * 实现应通过条件更新或行锁保证“存在、未过期、未消费、标记消费”是一个原子操作。
 * @note 原始票据不得进入 SQL、日志或返回值；只有 32 字节摘要允许进入此接口。
 */
class LoginTicketTransaction {
 public:
  virtual ~LoginTicketTransaction() = default;

  /**
   * @brief 原子标记票据已消费并读取账号/玩家快照。
   * @param ticket_digest 原始票据正文的 SHA-256。
   * @param now 当前 UTC；expires_at 必须严格晚于此时间。
   */
  [[nodiscard]] virtual ServiceResult<AuthenticatedPlayerSnapshot>
  ConsumeTicket(const Sha256Digest& ticket_digest,
                std::chrono::system_clock::time_point now) = 0;

  /** @brief 提交消费结果；提交失败时调用方不能假定票据仍可用。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 登录票据事务工厂，由数据库工作线程上的基础设施实现。 */
class LoginTicketUnitOfWork {
 public:
  virtual ~LoginTicketUnitOfWork() = default;

  /** @brief 开始独占连接的短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<std::unique_ptr<LoginTicketTransaction>>
  Begin() = 0;
};

/** @brief 网关 C2S3 选出的启动器票据登录命令。 */
struct ConsumeLoginTicketCommand final {
  std::string
      ticket;  ///< 仅在同步调用期存在的敏感票据正文，禁止写入日志和关联 ID。
};

/** @brief 网关运行时依赖的票据消费用例接口，便于隔离协议/会话测试。 */
class LoginTicketConsumer {
 public:
  virtual ~LoginTicketConsumer() = default;

  /** @brief 消费一次启动器票据并返回账号/玩家快照。 */
  [[nodiscard]] virtual ServiceResult<AuthenticatedPlayerSnapshot> Consume(
      const ConsumeLoginTicketCommand& command) const = 0;
};

/** @brief 本次登录新提交的贡献；重复领取或无普通战队时均为零。 */
struct DailyLoginRewardResult final {
  std::uint32_t clan_id{0U};      ///< 领取时冻结的普通战队。
  std::uint32_t clan_points{0U};  ///< 本次新提交的出勤贡献。
};

/** @brief 成功取得在线身份后，按北京时间自然日幂等发放登录奖励。 */
class DailyLoginRewardGranter {
 public:
  virtual ~DailyLoginRewardGranter() = default;
  /** @brief
   * 两种余额、流水和领取记录必须原子提交；失败由实现记录，不能阻止登录。 */
  [[nodiscard]] virtual std::optional<ServiceError> Grant(
      domain::PlayerId player_id,
      std::chrono::system_clock::time_point now) = 0;

  /** @brief 返回已提交的新贡献；兼容未提供战队积分的奖励实现。 */
  [[nodiscard]] virtual ServiceResult<DailyLoginRewardResult> GrantWithResult(
      domain::PlayerId player_id, std::chrono::system_clock::time_point now) {
    if (auto error = Grant(player_id, now)) return *error;
    return DailyLoginRewardResult{};
  }
};

/**
 * @brief 校验、摘要并原子消费五分钟单次登录票据。
 * @note SHA-256 在打开数据库事务前计算，事务持锁期间不执行密码学或等待网络。
 */
class LoginTicketService final : public LoginTicketConsumer {
 public:
  /** @brief 注入摘要、时钟和事务端口；三者生命周期必须覆盖本服务。 */
  LoginTicketService(const Sha256Hasher& hasher, const SystemClock& clock,
                     LoginTicketUnitOfWork& unit_of_work) noexcept;

  /**
   * @brief 消费格式为 `launcher-v2:` 加 32 个小写十六进制字符的票据。
   * @return 成功返回资料快照；无效、过期和已消费票据统一返回未认证错误。
   */
  [[nodiscard]] ServiceResult<AuthenticatedPlayerSnapshot> Consume(
      const ConsumeLoginTicketCommand& command) const override;

 private:
  const Sha256Hasher& hasher_;
  const SystemClock& clock_;
  LoginTicketUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_AUTHENTICATION_H_
