#ifndef BUBBLEFIGHTER_SERVER_NETWORK_LAUNCHER_AUTHENTICATION_HANDLER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_LAUNCHER_AUTHENTICATION_HANDLER_H_

/**
 * @file launcher_authentication_handler.h
 * @brief 定义独立登录入口与兼容入口共用的账号协议、限流和大区端点。
 */

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

#include "application/background_task_executor.h"
#include "features/authentication/launcher_authentication.h"
#include "network/tcp_server.h"

namespace bf::infrastructure::network {

/** @brief 登录服务签发票据所指向的固定大区入口；客户端不能覆盖。 */
struct LoginGatewayTarget final {
  std::uint32_t region_id{1U};  ///< 票据限定的大区编号。
  std::string host;             ///< 玩家可访问的游戏网关 IPv4。
  std::uint16_t port{0U};       ///< 游戏主连接端口。
  std::uint16_t clan_port{0U};  ///< 同一网关的家族子连接端口。
};

/** @brief 一分钟内按入口与真实来源 IP 限制密码认证，并支持关闭兼容认证入口。 */
class LauncherAuthenticationRateLimiter final {
 public:
  /** @brief disabled 用于拆分后的游戏入口，拒绝密码认证。 */
  explicit LauncherAuthenticationRateLimiter(bool enabled = true)
      : kEnabled(enabled) {}

  /** @brief 返回当前入口是否允许账号操作。 */
  [[nodiscard]] bool Enabled() const noexcept { return kEnabled; }

  /** @brief 全局最多 120 次、单 IP 最多 12 次；记录大小有界。 */
  [[nodiscard]] bool Accept(std::string_view peer,
                            std::chrono::steady_clock::time_point now);

 private:
  const bool kEnabled;
  std::mutex mutex_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>>
      attempts_;
};

/** @brief 编码不含密码或票据的启动器失败回复。 */
[[nodiscard]] std::vector<runtime::OutboundMessage> LauncherErrorResponse(
    std::string_view message);

/** @brief 在数据库工作线程验证密码、签发票据并返回可选大区端点。 */
[[nodiscard]] std::vector<runtime::OutboundMessage> HandleLauncherRequest(
    const protocol::Packet& packet, std::string_view peer,
    LauncherAuthenticationRateLimiter& limiter,
    application::RegisterAccountService& registration,
    application::LoginWithPasswordService& password_login,
    const LoginGatewayTarget* target = nullptr);

/** @brief 独立登录监听器的会话工厂，只接受账号协议，不创建世界或在线玩家租约。
 */
class LoginSessionHandlerFactory final : public SessionPacketHandlerFactory {
 public:
  /** @brief 外部服务须存活至 TCP 停止且数据库任务排空。 */
  LoginSessionHandlerFactory(application::BackgroundTaskExecutor& tasks,
                             application::RegisterAccountService& registration,
                             application::LoginWithPasswordService& login,
                             LoginGatewayTarget target);
  /** @brief 创建只接受 opcode 65020 的登录处理器。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<SessionPacketHandler>>
  Create(SessionHandlerContext context) override;

 private:
  application::BackgroundTaskExecutor& tasks_;
  application::RegisterAccountService& registration_;
  application::LoginWithPasswordService& login_;
  LoginGatewayTarget target_;
  std::shared_ptr<LauncherAuthenticationRateLimiter> limiter_;
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_LAUNCHER_AUTHENTICATION_HANDLER_H_
