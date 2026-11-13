#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ICE_NETWORK_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ICE_NETWORK_SERVICE_H_

#include <memory>

#include "features/battle/ice_service.h"
#include "features/configuration/server_config.h"

namespace bf::infrastructure::network {
class IceNetworkService final : public application::IceLeaseProvider {
 public:
  explicit IceNetworkService(configuration::IceConfig config);
  ~IceNetworkService() override;
  [[nodiscard]] std::optional<application::IceLease> Issue(
      domain::PlayerId player) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ICE_NETWORK_SERVICE_H_
