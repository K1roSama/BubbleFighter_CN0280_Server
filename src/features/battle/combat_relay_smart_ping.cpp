#include "features/battle/combat_relay.h"
#include "protocol/battle/smart_ping_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
application::ServiceResult<CombatRelayResult> CombatRelay::HandleSmartPing(
    domain::PlayerId sender, const protocol::SmartPing& ping,
    std::chrono::steady_clock::time_point now) {
  const auto found = actors_.find(sender);
  if (found == actors_.end() || found->second.controller ||
      found->second.soccer_ball)
    return application::ServiceError{
        .code = application::ErrorCode::kPermissionDenied,
        .safe_message = "只有本场玩家可以发送智能标点"};
  auto& actor = found->second;
  if (ping.actor != actor.participant.actor_object_id)
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "smart ping actor does not belong to authenticated sender"};
  if (!protocol::SmartPingMode(mode_id_, ping.kind))
    return CombatRelayResult{.reject_reason =
                                 CombatRejectReason::kUnrecoveredAction};
  if (actor.life_state != CombatLifeState::kAlive)
    return CombatRelayResult{.reject_reason =
                                 CombatRejectReason::kInvalidLifecycleState};
  if (actor.last_smart_ping &&
      now - *actor.last_smart_ping <
          std::chrono::milliseconds{protocol::kSmartPingCooldownMs})
    return CombatRelayResult{.reject_reason = CombatRejectReason::kRateLimited};
  const OutboundMessage message{.opcode = protocol::kSmartPingOpcode,
                                .body = protocol::EncodeSmartPing(ping)};
  CombatRelayResult result{.accepted = true};
  // Use the current combat team: infection may change it after the room starts.
  // Echo to the sender for the reference's distinct local-player marker, but
  // never send team coordinates to enemies, observers, or synthetic NPCs.
  for (const auto& [id, peer] : actors_)
    if (!peer.controller && !peer.soccer_ball &&
        (id == sender ||
         (!individual_scoring_ &&
          peer.participant.team_code == actor.participant.team_code)))
      result.deliveries.push_back({.recipient = id, .message = message});
  actor.last_smart_ping = now;
  return result;
}
}  // namespace bf::runtime
