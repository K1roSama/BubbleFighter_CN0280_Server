#include "sessions/gateway_session_state.h"

namespace bf::runtime {
namespace {

application::ServiceError InvalidState(std::string_view message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidState,
      .safe_message = std::string{message},
      .correlation_id = {},
      .is_retryable = false,
  };
}

}  // namespace

std::optional<application::ServiceError>
GatewaySessionState::AcceptPatchHandshake() {
  if (phase_ != GatewaySessionPhase::kConnected) {
    return InvalidState("补丁握手只能在 connected 阶段执行一次");
  }
  phase_ = GatewaySessionPhase::kPatchReady;
  return std::nullopt;
}

std::optional<application::ServiceError> GatewaySessionState::CompleteLogin(
    LoginDisposition disposition) {
  if (phase_ != GatewaySessionPhase::kPatchReady) {
    return InvalidState("登录只能在 patch_ready 阶段完成");
  }
  phase_ = disposition == LoginDisposition::kCharacterCreationRequired
               ? GatewaySessionPhase::kCreatingCharacter
               : GatewaySessionPhase::kAuthenticated;
  return std::nullopt;
}

std::optional<application::ServiceError>
GatewaySessionState::CompleteCharacterCreation() {
  if (phase_ != GatewaySessionPhase::kCreatingCharacter) {
    return InvalidState("角色创建只能在 creating_character 阶段完成");
  }
  phase_ = GatewaySessionPhase::kAuthenticated;
  return std::nullopt;
}

std::optional<application::ServiceError>
GatewaySessionState::AcceptPostLoginBootstrap() {
  if (phase_ != GatewaySessionPhase::kAuthenticated) {
    return InvalidState("登录后初始化只能在 authenticated 阶段执行一次");
  }
  phase_ = GatewaySessionPhase::kMenuReady;
  return std::nullopt;
}

void GatewaySessionState::BeginClose() noexcept {
  if (phase_ != GatewaySessionPhase::kClosed) {
    phase_ = GatewaySessionPhase::kClosing;
  }
}

void GatewaySessionState::CompleteClose() noexcept {
  if (phase_ == GatewaySessionPhase::kClosing) {
    phase_ = GatewaySessionPhase::kClosed;
  }
}

GatewaySessionPhase GatewaySessionState::Phase() const noexcept {
  return phase_;
}

std::string_view GatewaySessionPhaseName(GatewaySessionPhase phase) noexcept {
  switch (phase) {
    case GatewaySessionPhase::kConnected:
      return "connected";
    case GatewaySessionPhase::kPatchReady:
      return "patch_ready";
    case GatewaySessionPhase::kCreatingCharacter:
      return "creating_character";
    case GatewaySessionPhase::kAuthenticated:
      return "authenticated";
    case GatewaySessionPhase::kMenuReady:
      return "menu_ready";
    case GatewaySessionPhase::kClosing:
      return "closing";
    case GatewaySessionPhase::kClosed:
      return "closed";
  }
  return "unknown";
}

}  // namespace bf::runtime
