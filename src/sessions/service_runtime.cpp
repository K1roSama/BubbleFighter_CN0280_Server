#include "sessions/service_runtime.h"

#include <string>

namespace bf::runtime {

ServiceRuntime::ServiceRuntime(application::EventLogger& logger) noexcept
    : logger_(logger) {}

std::optional<application::ServiceError> ServiceRuntime::Start() {
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::kCreated) {
      return application::ServiceError{
          .code = application::ErrorCode::kInvalidState,
          .safe_message = "The service runtime can only be started once",
          .correlation_id = {},
          .is_retryable = false,
      };
    }
    state_ = RuntimeState::kRunning;
  }
  logger_.Log(application::LogLevel::kInfo, "[Server] Starting BubbleFighter.");
  return std::nullopt;
}

void ServiceRuntime::RequestStop(std::string_view reason) noexcept {
  bool changed = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == RuntimeState::kCreated) {
      state_ = RuntimeState::kStopped;
      changed = true;
    } else if (state_ == RuntimeState::kRunning) {
      state_ = RuntimeState::kStopping;
      changed = true;
    }
  }
  if (changed) {
    try {
      logger_.Log(
          application::LogLevel::kInfo,
          reason.empty()
              ? "[Server] Shutdown requested. Stopping services gracefully."
              : "[Server] Shutting down gracefully; reason=" +
                    std::string{reason});
    } catch (...) {
      // EventLogger::log 本身为
      // noexcept；此分支只处理拼接停止原因时的分配失败。
      logger_.Log(application::LogLevel::kInfo,
                  "[Server] Shutdown requested. Stopping services gracefully.");
    }
  }
}

void ServiceRuntime::CompleteStop() noexcept {
  bool changed = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == RuntimeState::kStopping) {
      state_ = RuntimeState::kStopped;
      changed = true;
    }
  }
  if (changed) {
    logger_.Log(application::LogLevel::kInfo, "[Server] Shutdown completed.");
  }
}

RuntimeState ServiceRuntime::State() const noexcept {
  std::lock_guard lock(mutex_);
  return state_;
}

bool ServiceRuntime::IsReady() const noexcept {
  return State() == RuntimeState::kRunning;
}

std::string_view RuntimeStateName(RuntimeState state) noexcept {
  switch (state) {
    case RuntimeState::kCreated:
      return "created";
    case RuntimeState::kRunning:
      return "running";
    case RuntimeState::kStopping:
      return "stopping";
    case RuntimeState::kStopped:
      return "stopped";
  }
  return "unknown";
}

}  // namespace bf::runtime
