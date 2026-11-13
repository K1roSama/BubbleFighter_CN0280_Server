/** @file soccer_objectives.cpp
 * @brief 实现原生足球对象登记、房主进球报告与服务器复位计时。
 */
#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
bool MatchObjectives::SoccerBallObject(std::uint32_t host_actor,
                                       std::uint32_t object) noexcept {
  // 69D620 initializes a 100000-ID range per seat. 7CB690 takes the next
  // counter value, after any other objects allocated during scene loading.
  return host_actor >= 1'000'000U && host_actor <= 8'900'000U &&
         host_actor % 100'000U == 0U && object > host_actor &&
         object - host_actor < 100'000U;
}

bool MatchObjectives::RegisterSoccerBall(domain::PlayerId host,
                                         std::uint32_t object,
                                         CombatRelay& combat) {
  const auto* owner = Participant(host);
  if (mode_ != 11U || !owner || combat.host_ != host ||
      !SoccerBallObject(owner->actor_object_id, object))
    return false;
  if (soccer_ball_) return *soccer_ball_ == object;
  if (combat.object_ids_.contains(object) ||
      combat.ContainsDamagerCookie(object))
    return false;
  const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) | object};
  CombatRelay::ActorState ball;
  ball.participant = {.player_id = synthetic,
                      .actor_object_id = object,
                      .team_code = 1U,
                      .weapon_templates = {},
                      .ordinary_respawn_allowed = false};
  ball.controller = host;
  ball.soccer_ball = true;
  combat.actors_.emplace(synthetic, std::move(ball));
  combat.object_owners_.emplace(object, synthetic);
  combat.object_ids_.insert(object);
  soccer_ball_ = object;
  return true;
}

MatchObjectiveUpdate MatchObjectives::Soccer(domain::PlayerId player,
                                             const protocol::Packet& packet,
                                             CombatRelay& combat, Time now) {
  if (mode_ != 11U || combat.host_ != player || !soccer_ball_) return {};
  // 7CBF90 reports event 21 via 7C58B0 -> 7E9D00 -> 7ED370 on the reliable
  // server connection. 32878 is the separate peer goal effect; either can
  // arrive first. Both enter this one phase gate, so one goal scores once.
  if (soccer_phase_ != 0U) return {};
  protocol::PacketReader reader{packet.body};
  std::uint32_t goal = 0U;
  if (packet.opcode == 710U) {
    if (reader.ReadU8() != 21U) return {};
    goal = reader.ReadU32();
  } else if (packet.opcode == 32878U) {
    goal = reader.ReadU8();
  } else {
    return {};
  }
  ++round_scores_[1U - goal];  // Goalpost_T1/T2 name the defending team's goal.
  soccer_phase_ = 1U;
  soccer_due_ = now + std::chrono::seconds{5};
  return {.messages = {{.opcode = 32878U,
                        .body = {static_cast<std::uint8_t>(goal)}}},
          .score_changed = true};
}
}  // namespace bf::runtime
