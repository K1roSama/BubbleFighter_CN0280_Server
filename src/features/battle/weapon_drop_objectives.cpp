/** @file weapon_drop_objectives.cpp
 * @brief 道具火箭筒的真实 730 丢弃、747 生成、731 拾取和 748 配装闭环。
 */
#include <algorithm>

#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
MatchObjectiveUpdate MatchObjectives::WeaponDrop(domain::PlayerId player,
                                                 const protocol::Packet& packet,
                                                 CombatRelay& combat,
                                                 Time now) {
  using namespace std::chrono_literals;
  const auto rejected = [](const char* reason) {
    return MatchObjectiveUpdate{
        .rejection_detail =
            std::string{"weapon pickup/drop rejected: "} + reason};
  };
  const auto found = combat.actors_.find(player);
  if ((mode_ != 3U && mode_ != 16U) || found == combat.actors_.end() ||
      found->second.controller)
    return rejected("unsupported_mode_or_source");
  auto& actor = found->second;
  if (actor.life_state != CombatLifeState::kAlive ||
      (mode_ == 3U && !actor.position) ||
      (mode_ == 16U && actor.participant.team_code != 2U))
    return rejected("owner_not_alive_or_position_missing");
  std::erase_if(dropped_weapons_, [&](const auto& item) {
    return now >= item.created && now - item.created >= 20s;
  });
  const auto nearby = [&](const std::array<float, 3U>& point, double radius) {
    // PVE clients own collision; a separate movement sample is not a pickup
    // prerequisite.
    if (mode_ == 16U) return true;
    double squared = 0.0;
    for (std::size_t i = 0; i < 3U; ++i) {
      const double delta = (*actor.position)[i] - point[i];
      squared += delta * delta;
    }
    return squared <= radius * radius;
  };
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 730U) {
    std::array<float, 3U> position{}, velocity{};
    for (auto& axis : position) axis = reader.ReadF32();
    for (auto& axis : velocity) axis = reader.ReadF32();
    const auto weapon = reader.ReadU32();
    const auto total = reader.ReadU32();
    const auto magazine = reader.ReadU32();
    const auto bar = reader.ReadU32();
    const auto expires = reader.ReadU32();
    if (weapon != (mode_ == 16U ? 9025U : 9021U) ||
        weapon != actor.bazooka_template || bar != 4U || expires != 1U ||
        !actor.bazooka || actor.bazooka->first != actor.life_generation ||
        actor.participant.weapon_templates[4U])
      return rejected("weapon_not_granted_for_this_life");
    if (total > actor.bazooka->second || magazine > total)
      return rejected("reported_ammunition_exceeds_remaining_grant");
    // Native adds a vertical offset and throws the object. These radii are
    // transport policy, not server simulation of trajectory or collision.
    if (!nearby(position, 150.0) || dropped_weapons_.size() >= 256U)
      return rejected("drop_out_of_range_or_object_limit");
    if (!combat.ConsumeActionQuota(actor, now)) return rejected("rate_limit");
    const auto id = ++serial_;
    dropped_weapons_.push_back({id, total, magazine, position, now});
    actor.bazooka.reset();
    std::erase_if(combat.item_effects_, [&](const auto& permit) {
      return permit.source == player &&
             (permit.type == 138U || permit.type == 146U);
    });
    // 7F0E10 -> A206D0: object, template, total, magazine, bar, position,
    // velocity, DWORD expiry flag. S2C731 shifts a respawn timer, not a weapon
    // ACK.
    protocol::PacketWriter spawned;
    for (const auto value : {id, weapon, total, magazine, bar})
      spawned.WriteU32(value);
    for (const auto value : position) spawned.WriteF32(value);
    for (const auto value : velocity) spawned.WriteF32(value);
    spawned.WriteU32(expires);
    return {.messages = {{.opcode = 747U, .body = spawned.Release()}}};
  }
  const auto id = reader.ReadU32();
  if (reader.ReadU32() != actor.participant.actor_object_id)
    return rejected("foreign_collector");
  const auto item = std::ranges::find(dropped_weapons_, id, &DroppedWeapon::id);
  if (item == dropped_weapons_.end() || now < item->created ||
      now - item->created < 1s)
    return rejected("unknown_consumed_or_not_active_object");
  // 84AA00 skips zero ammunition setters, which would refill a spent native
  // weapon. Do not authorize that duplication: empty visual drops only expire.
  if (item->total_ammo == 0U || item->magazine_ammo == 0U ||
      actor.participant.weapon_templates[4U] ||
      (actor.bazooka && actor.bazooka->first == actor.life_generation))
    return rejected("spent_object_or_occupied_bar");
  if (!nearby(item->position, 300.0)) return rejected("pickup_out_of_range");
  if (!combat.ConsumeActionQuota(actor, now)) return rejected("rate_limit");
  actor.bazooka = {actor.life_generation, item->total_ammo};
  actor.bazooka_template = mode_ == 16U ? 9025U : 9021U;
  dropped_weapons_.erase(item);
  protocol::PacketWriter acquired;
  acquired.WriteU32(id);
  acquired.WriteU32(actor.participant.actor_object_id);
  return {.messages = {{.opcode = 748U, .body = acquired.Release()}}};
}
}  // namespace bf::runtime
