/** @file infection_fever.cpp @brief 原版母体暴走的客户端蓄力、服务端单次授权。
 */
#include <algorithm>

#include "features/battle/infection_match.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
constexpr auto kApplicationWindow =
    std::chrono::seconds{10};                         // Transport policy.
constexpr auto kDuration = std::chrono::seconds{20};  // ItemSpecZombie104.
}  // namespace

bool InfectionMatch::AcceptsFever(const protocol::Packet& packet) {
  if (packet.opcode != 707U && packet.opcode != 32894U &&
      packet.opcode != 32828U && packet.opcode != 32812U)
    return false;
  protocol::PacketReader reader{packet.body};
  static_cast<void>(reader.ReadU32());
  const auto type = reader.ReadU32();
  return type ==
         ((packet.opcode == 707U || packet.opcode == 32894U) ? 104U : 11U);
}

std::vector<OutboundMessage> InfectionMatch::UpdateFever(CombatRelay& combat,
                                                         Time now) {
  if (!fever_) return {};
  const auto found = combat.actors_.find(mother_);
  if (found == combat.actors_.end()) {
    fever_.reset();
    return {};
  }
  auto& actor = found->second;
  std::vector<OutboundMessage> messages;
  if (!fever_->ended &&
      (now >= fever_->until || actor.life_generation != fever_->life ||
       actor.life_state != CombatLifeState::kAlive || winner_)) {
    fever_->ended = true;
    fever_->blast_pending = false;
    actor.participant.weapon_templates[2U] = fever_->base_weapon;
    actor.last_fire.reset();
    melee_.clear();
    blasts_.clear();
    const auto object = actor.participant.actor_object_id;
    std::erase_if(combat.weapon_objects_, [object](const auto& entry) {
      return entry.second.owner_object_id == object;
    });
    protocol::PacketWriter body;
    body.WriteU32(object);
    body.WriteU32(11U);
    messages.push_back({32812U, body.Release()});
  }
  // Cleanup carries no activation ID. An early/late receipt never advances the
  // current deadline. Expiry releases the grant even if the original client
  // suppressed its own callback after receiving the server's cleanup.
  if (fever_->ended) fever_.reset();
  return messages;
}

CombatRelayResult InfectionMatch::HandleFever(domain::PlayerId player,
                                              const protocol::Packet& packet,
                                              CombatRelay& combat, Time now) {
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  const auto type = reader.ReadU32();
  std::uint32_t item = 104U;
  std::uint32_t duration = 20000U;
  if (packet.opcode == 32828U) {
    item = reader.ReadU32();
    duration = reader.ReadU32();
  }
  reader.RequireEnd();
  const bool request = packet.opcode == 707U;
  const bool cleanup = packet.opcode == 32812U;
  const bool attribute = packet.opcode == 32828U;
  if ((!request && !cleanup && !attribute && packet.opcode != 32894U) ||
      type != ((request || packet.opcode == 32894U) ? 104U : 11U) ||
      item != 104U || duration != 20000U || (request && object != 0xffffffffU))
    return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
  const auto found = combat.actors_.find(player);
  if (found == combat.actors_.end() || found->second.controller)
    return {.reject_reason = CombatRejectReason::kUnknownRelatedActor};
  auto& actor = found->second;
  if (!request && object != actor.participant.actor_object_id) {
    // Native host prediction also reports remote attribute cleanup.
    if (cleanup && combat.host_ == player &&
        combat.object_owners_.contains(object))
      return {.reject_reason = CombatRejectReason::kUnrecoveredAction};
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kInvalidValue,
        "infection fever belongs to another participant"};
  }
  if (!started_ || winner_ || player != mother_)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  if (cleanup) {
    if (!fever_ || fever_->cleanup_reported)
      return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
    fever_->cleanup_reported = true;
    CombatRelayResult result{.accepted = true};
    for (const auto& message : UpdateFever(combat, now)) {
      auto relay =
          combat.AcceptedResult(player, {message.opcode, message.body});
      relay.deliveries.push_back({player, message});
      result.deliveries.insert(result.deliveries.end(),
                               relay.deliveries.begin(),
                               relay.deliveries.end());
    }
    return result;
  }
  if (actor.life_state != CombatLifeState::kAlive)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  if (request) {
    if (fever_) return {.reject_reason = CombatRejectReason::kDuplicateAction};
    if (!combat.ConsumeActionQuota(actor, now))
      return {.reject_reason = CombatRejectReason::kRateLimited};
    // A7C4F0 calculates readiness locally; its complete damage inputs are not
    // transmitted. This compatibility policy accepts the authenticated claim.
    const auto base = actor.participant.weapon_templates[2U].value_or(7200U);
    fever_ = Fever{now, now + kDuration, actor.life_generation, base};
    actor.participant.weapon_templates[2U] = base == 7220U ? 7222U : 7207U;
    actor.last_fire.reset();
    protocol::PacketWriter body;
    body.WriteU32(actor.participant.seat_index);
    body.WriteU32(104U);
    body.WriteU32(0U);
    const protocol::Packet granted{715U, body.Release()};
    auto result = combat.AcceptedResult(player, granted);
    result.deliveries.push_back({player, {granted.opcode, granted.body}});
    return result;
  }
  if (!fever_ || fever_->ended || fever_->life != actor.life_generation ||
      now < fever_->requested || now >= fever_->requested + kApplicationWindow)
    return {.reject_reason = CombatRejectReason::kInvalidLifecycleState};
  auto& applied = attribute ? fever_->attribute_applied : fever_->item_applied;
  if (applied) return {.reject_reason = CombatRejectReason::kDuplicateAction};
  if (!combat.ConsumeActionQuota(actor, now))
    return {.reject_reason = CombatRejectReason::kRateLimited};
  applied = true;
  // 32894 prediction may precede or follow the locally generated32828.
  // Both consume the same715 grant; neither can extend its20-second window.
  return combat.AcceptedResult(player, packet);
}
}  // namespace bf::runtime
