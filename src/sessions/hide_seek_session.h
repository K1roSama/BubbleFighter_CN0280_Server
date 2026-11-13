#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_HIDE_SEEK_SESSION_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_HIDE_SEEK_SESSION_H_

/** @file hide_seek_session.h @brief Authenticated, epoch-bound commands and
 * private deliveries.
 */
#include <map>

#include "features/battle/hide_seek_match.h"
#include "protocol/battle/hide_seek_wire.h"

namespace bf::runtime {
struct HideSeekDelivery final {
  HideSeekPlayer recipient{};
  std::uint16_t opcode{};
  std::vector<std::uint8_t> body;
};

/** Owns a round's replay ledger. Authenticated player IDs come from the caller,
 * never the payload.
 */
class HideSeekSession final {
 public:
  explicit HideSeekSession(HideSeekMatch match) : match_(std::move(match)) {}

  [[nodiscard]] std::vector<HideSeekDelivery> Request(
      HideSeekPlayer authenticated_player, std::span<const std::uint8_t> bytes,
      HideSeekTime now);
  [[nodiscard]] std::vector<HideSeekDelivery> Tick(HideSeekTime now);
  [[nodiscard]] std::vector<HideSeekDelivery> Leave(HideSeekPlayer player,
                                                    HideSeekTime now);
  /** Late join/reconnect is a spectator subscription, never a roster
   * resurrection. */
  [[nodiscard]] std::vector<HideSeekDelivery> Observe(HideSeekPlayer player,
                                                      HideSeekTime now);

  [[nodiscard]] const HideSeekMatch& Match() const noexcept { return match_; }

 private:
  [[nodiscard]] HideSeekSnapshot Snapshot(HideSeekPlayer player) const;
  [[nodiscard]] std::vector<HideSeekDelivery> Reject(HideSeekPlayer player,
                                                     HideSeekReject reason);
  [[nodiscard]] std::vector<HideSeekDelivery> Deliveries(
      std::optional<std::pair<HideSeekPlayer, HideSeekReject>> rejection =
          std::nullopt);
  HideSeekMatch match_;
  std::map<HideSeekPlayer, std::uint64_t> last_serial_;
  std::map<HideSeekPlayer, HideSeekTime> last_poll_;
  std::map<HideSeekPlayer, HideSeekTime> last_request_;
  std::set<HideSeekPlayer> observers_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_HIDE_SEEK_SESSION_H_
