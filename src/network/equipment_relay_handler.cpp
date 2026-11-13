#include "network/equipment_relay_handler.h"

#include "protocol/codec/packet_io.h"

namespace bf::infrastructure::network {
namespace {
class Handler final : public SessionPacketHandler {
 public:
  Handler(runtime::EquipmentRelayBroker& broker, runtime::SessionHandle handle)
      : broker_(broker), handle_(handle) {}

  void AsyncHandle(protocol::Packet packet,
                   PacketCompletion completion) override {
    try {
      completion(broker_.Handle(handle_, packet));
    } catch (const protocol::ProtocolError&) {
      completion(application::ServiceError{
          .code = application::ErrorCode::kInvalidInput,
          .safe_message = "装备初始化负载无效",
          .correlation_id = {},
          .is_retryable = false});
    }
  }

  application::LogLevel RejectionLogLevel(
      const application::ServiceError& error) const noexcept override {
    return error.code == application::ErrorCode::kInvalidState
               ? application::LogLevel::kInfo
               : application::LogLevel::kWarning;
  }

  std::optional<application::ServiceError> ApplyDispatch(
      const runtime::SessionDispatch&) override {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidState,
        .safe_message = "装备连接不接受世界消息投递",
        .correlation_id = {},
        .is_retryable = false};
  }

  void BeginClose() noexcept override { broker_.Disconnect(handle_); }

 private:
  runtime::EquipmentRelayBroker& broker_;
  runtime::SessionHandle handle_;
};
}  // namespace

application::ServiceResult<std::unique_ptr<SessionPacketHandler>>
EquipmentRelayHandlerFactory::Create(SessionHandlerContext context) {
  return std::unique_ptr<SessionPacketHandler>{
      std::make_unique<Handler>(broker_, context.handle)};
}
}  // namespace bf::infrastructure::network
