#ifndef BUBBLEFIGHTER_SERVER_NETWORK_EQUIPMENT_RELAY_HANDLER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_EQUIPMENT_RELAY_HANDLER_H_

/**
 * @file equipment_relay_handler.h
 * @brief 独立原生 Relay TCP 会话，使用主网关签发的本局凭据认证。
 */
#include "network/tcp_server.h"
#include "sessions/equipment_relay_broker.h"

namespace bf::infrastructure::network {
/** @brief 为每条独立 TCP 连接创建仅可认证本局装备的处理器。 */
class EquipmentRelayHandlerFactory final : public SessionPacketHandlerFactory {
 public:
  explicit EquipmentRelayHandlerFactory(runtime::EquipmentRelayBroker& broker)
      : broker_(broker) {}

  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<SessionPacketHandler>>
  Create(SessionHandlerContext context) override;

 private:
  runtime::EquipmentRelayBroker& broker_;
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_EQUIPMENT_RELAY_HANDLER_H_
