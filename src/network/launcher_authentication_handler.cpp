#include "network/launcher_authentication_handler.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

namespace bf::infrastructure::network {
bool LauncherAuthenticationRateLimiter::Accept(
    std::string_view peer, std::chrono::steady_clock::time_point now) {
  std::scoped_lock lock{mutex_};
  const auto oldest = now - std::chrono::minutes{1};
  while (!attempts_.empty() && attempts_.front().first < oldest)
    attempts_.pop_front();
  const auto count =
      std::count_if(attempts_.begin(), attempts_.end(),
                    [peer](const auto& value) { return value.second == peer; });
  if (!kEnabled || attempts_.size() >= 120U || count >= 12) return false;
  attempts_.emplace_back(now, peer);
  return true;
}

namespace {
constexpr std::uint16_t kLauncherAuthenticationResponseOpcode = 65021U;
constexpr std::size_t kMaximumLauncherRequestBytes = 2048U;

struct LauncherAuthenticationRequest final {
  std::string action;
  std::string account_name;
  std::string password;
  std::string machine_code;
};

std::vector<runtime::OutboundMessage> LauncherJsonResponse(
    const nlohmann::json& response) {
  const auto text =
      response.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
  return {{.opcode = kLauncherAuthenticationResponseOpcode,
           .body = std::vector<std::uint8_t>{text.begin(), text.end()}}};
}

std::variant<LauncherAuthenticationRequest, std::string> ParseLauncherRequest(
    std::span<const std::uint8_t> body) {
  if (body.size() > kMaximumLauncherRequestBytes) {
    return std::string{"请求过长。"};
  }
  const auto document =
      nlohmann::json::parse(body.begin(), body.end(), nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    return std::string{"无效的请求格式。"};
  }
  const auto action = document.find("action");
  const auto name = document.find("name");
  const auto password = document.find("password");
  if (action != document.end() && action->is_string() &&
      action->get<std::string>() == "email_policy")
    return LauncherAuthenticationRequest{.action = "email_policy"};
  if (action == document.end() || name == document.end() ||
      password == document.end() || !action->is_string() ||
      !name->is_string() || !password->is_string()) {
    return std::string{"无效的请求。"};
  }
  auto request =
      LauncherAuthenticationRequest{.action = action->get<std::string>(),
                                    .account_name = name->get<std::string>(),
                                    .password = password->get<std::string>()};
  if (request.action != "register" && request.action != "login") {
    return std::string{"不支持的操作。"};
  }
  if (const auto machine = document.find("machine_code");
      machine != document.end() && machine->is_string()) {
    request.machine_code = machine->get<std::string>();
  }
  return request;
}

}  // namespace

std::vector<runtime::OutboundMessage> LauncherErrorResponse(
    std::string_view message) {
  return LauncherJsonResponse({{"ok", false}, {"message", message}});
}

std::vector<runtime::OutboundMessage> HandleLauncherRequest(
    const protocol::Packet& packet, std::string_view peer,
    LauncherAuthenticationRateLimiter& limiter,
    application::RegisterAccountService& registration,
    application::LoginWithPasswordService& password_login,
    const LoginGatewayTarget* target) {
  if (!limiter.Enabled()) {
    return LauncherErrorResponse("请连接独立登录服务进行注册或登录。");
  }
  if (!limiter.Accept(peer, std::chrono::steady_clock::now())) {
    return LauncherErrorResponse("操作过于频繁，请一分钟后重试。");
  }
  auto request_result = ParseLauncherRequest(packet.body);
  if (const auto* error = std::get_if<std::string>(&request_result)) {
    return LauncherErrorResponse(*error);
  }
  auto request =
      std::get<LauncherAuthenticationRequest>(std::move(request_result));
  if (request.action == "email_policy")
    return LauncherJsonResponse(
        {{"ok", true}, {"email", {{"enabled", false}}}});
  if (request.action == "register") {
    auto result = registration.RegisterAccount(
        {.account_name = std::move(request.account_name),
         .password = std::move(request.password)});
    if (const auto* error = std::get_if<application::ServiceError>(&result)) {
      return LauncherErrorResponse(error->safe_message);
    }
    const auto& registered = std::get<application::RegisteredAccount>(result);
    return LauncherJsonResponse({{"ok", true},
                                 {"message", "注册成功，请使用新账号登录。"},
                                 {"name", registered.account_name}});
  }
  auto verified = password_login.VerifyPassword(
      {.account_name = std::move(request.account_name),
       .password = std::move(request.password),
       .machine_code = std::move(request.machine_code)});
  if (const auto* error = std::get_if<application::ServiceError>(&verified)) {
    return LauncherErrorResponse(error->safe_message);
  }
  const auto& account =
      std::get<application::PasswordAuthenticatedAccount>(verified);
  auto result = password_login.IssueTicket(account);
  if (const auto* error = std::get_if<application::ServiceError>(&result))
    return LauncherErrorResponse(error->safe_message);
  auto login = std::get<application::LauncherLoginResult>(std::move(result));
  const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
                           login.expires_at.time_since_epoch())
                           .count();
  nlohmann::json response{{"ok", true},
                          {"message", "登录成功，可以启动游戏。"},
                          {"name", login.account_name},
                          {"token", login.ticket},
                          {"expires", expires}};
  if (target != nullptr) {
    response["game"] = {{"region_id", target->region_id},
                        {"host", target->host},
                        {"port", target->port},
                        {"clan_host", target->host},
                        {"clan_port", target->clan_port}};
  }
  return LauncherJsonResponse(response);
}

namespace {
class LoginHandler final : public SessionPacketHandler {
 public:
  LoginHandler(application::BackgroundTaskExecutor& tasks,
               application::RegisterAccountService& registration,
               application::LoginWithPasswordService& login,
               LoginGatewayTarget target,
               std::shared_ptr<LauncherAuthenticationRateLimiter> limiter,
               std::string peer)
      : tasks_(tasks),
        registration_(registration),
        login_(login),
        target_(std::move(target)),
        limiter_(std::move(limiter)),
        peer_(std::move(peer)) {}

  void AsyncHandle(protocol::Packet packet,
                   PacketCompletion completion) override {
    if (packet.opcode != 65020U) {
      completion(application::ServiceError{
          .code = application::ErrorCode::kUnsupportedOpcode,
          .safe_message = "登录入口仅接受账号操作，请连接返回的游戏网关",
          .correlation_id = {}});
      return;
    }
    auto callback = std::make_shared<PacketCompletion>(std::move(completion));
    auto error =
        tasks_.Post([packet = std::move(packet), callback, closed = closed_,
                     limiter = limiter_, peer = peer_, target = target_,
                     registration = &registration_, login = &login_]() mutable {
          if (closed->load()) {
            (*callback)(LauncherErrorResponse("登录连接已关闭。"));
            return;
          }
          std::vector<runtime::OutboundMessage> replies;
          try {
            replies = HandleLauncherRequest(packet, peer, *limiter,
                                            *registration, *login, &target);
          } catch (const std::exception&) {
            replies = LauncherErrorResponse("登录服务暂时不可用，请稍后重试。");
          }
          (*callback)(std::move(replies));
        });
    if (error) (*callback)(*error);
  }

  std::optional<application::ServiceError> ApplyDispatch(
      const runtime::SessionDispatch&) override {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidState,
        .safe_message = "登录连接不接受游戏状态投递",
        .correlation_id = {}};
  }

  void BeginClose() noexcept override { closed_->store(true); }

 private:
  application::BackgroundTaskExecutor& tasks_;
  application::RegisterAccountService& registration_;
  application::LoginWithPasswordService& login_;
  LoginGatewayTarget target_;
  std::shared_ptr<LauncherAuthenticationRateLimiter> limiter_;
  std::string peer_;
  std::shared_ptr<std::atomic<bool>> closed_{
      std::make_shared<std::atomic<bool>>(false)};
};
}  // namespace

LoginSessionHandlerFactory::LoginSessionHandlerFactory(
    application::BackgroundTaskExecutor& tasks,
    application::RegisterAccountService& registration,
    application::LoginWithPasswordService& login, LoginGatewayTarget target)
    : tasks_(tasks),
      registration_(registration),
      login_(login),
      target_(std::move(target)),
      limiter_(std::make_shared<LauncherAuthenticationRateLimiter>()) {}

application::ServiceResult<std::unique_ptr<SessionPacketHandler>>
LoginSessionHandlerFactory::Create(SessionHandlerContext context) {
  return std::unique_ptr<SessionPacketHandler>{std::make_unique<LoginHandler>(
      tasks_, registration_, login_, target_, limiter_,
      std::move(context.peer_address))};
}
}  // namespace bf::infrastructure::network
