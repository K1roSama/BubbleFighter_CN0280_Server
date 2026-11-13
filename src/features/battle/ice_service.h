#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ICE_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ICE_SERVICE_H_

#include <cstdint>
#include <optional>
#include <string>

#include "application/identifier.h"

namespace bf::application {
struct IceLease final {
  std::string stun_host, turn_host, username, password;
  std::uint16_t stun_port{0}, turn_port{0};
  std::uint64_t expires_at{0};
  bool relay_only{false};  ///< Administrator/test policy; never a client claim.
  bool operator==(const IceLease&) const = default;
};

class IceLeaseProvider {
 public:
  virtual ~IceLeaseProvider() = default;
  [[nodiscard]] virtual std::optional<IceLease> Issue(
      domain::PlayerId player) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_ICE_SERVICE_H_
