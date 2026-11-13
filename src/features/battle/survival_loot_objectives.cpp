#include <algorithm>

#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
MatchObjectiveUpdate MatchObjectives::DropSurvivalLoot(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  using namespace std::chrono_literals;
  const bool partial = packet.opcode == 65080U;
  protocol::PacketReader request{packet.body};
  const auto serial = partial ? request.ReadU32() : 0U;
  const auto observed = partial ? request.ReadU32() : 0U;
  auto items = DecodeSurvivalDrop(request.ReadBytes(request.RemainingBytes()));
  const auto ack = [&](std::uint32_t status, std::uint32_t remaining) {
    protocol::PacketWriter writer;
    writer.WriteU32(serial);
    writer.WriteU32(status);
    writer.WriteU32(items.front().type);
    writer.WriteU32(remaining);
    return OutboundMessage{65081U, writer.Release()};
  };
  const auto reject = [&](const char* detail) {
    MatchObjectiveUpdate result{
        .rejection_detail = std::string{"survival drop rejected: "} + detail};
    if (partial) result.deliveries.push_back({player, ack(1U, observed)});
    return result;
  };
  const auto owner = combat.actors_.find(player);
  if ((mode_ != 23U && mode_ != 25U) || owner == combat.actors_.end() ||
      owner->second.controller)
    return reject("unsupported_mode_or_source");
  auto& actor = owner->second;
  const auto reply_key = std::tuple{player, actor.life_generation, serial};
  if (partial) {
    const auto prior = partial_drop_replies_.find(reply_key);
    if (prior != partial_drop_replies_.end()) {
      if (prior->second.request != packet.body)
        return reject("serial_reused_with_different_request");
      MatchObjectiveUpdate result;
      result.deliveries.push_back({player, prior->second.reply});
      return result;
    }
    if (partial_drop_replies_.size() >= 4096U)
      return reject("partial_request_limit");
  }
  if (actor.life_state != CombatLifeState::kAlive || !actor.position ||
      survival_loot_pending_)
    return reject("owner_not_alive_or_position_missing");
  protocol::PacketWriter key;
  key.WriteU64(player.Value());
  key.WriteU64(actor.life_generation);
  key.WriteU16(packet.opcode);
  key.WriteBytes(packet.body);
  auto signature = key.Release();
  if (drops_.contains(signature)) return reject("duplicate_request");
  const auto active_count = std::ranges::count_if(
      survival_loot_,
      [now](const auto& item) { return !item.expires || now < *item.expires; });
  if (active_count + items.size() > 4096U || drops_.size() >= 4096U)
    return reject("object_or_request_limit");
  std::set<std::uint32_t> types;
  for (const auto& item : items) {
    if (!types.insert(item.type).second)
      return reject("duplicate_type_in_batch");
    double squared = 0.0;
    for (std::size_t i = 0; i < 3U; ++i) {
      const double delta = (*actor.position)[i] - item.position[i];
      squared += delta * delta;
    }
    if (squared > 300.0 * 300.0) return reject("drop_out_of_range");
    const auto weapon = SurvivalLootWeapon(item.type);
    if (item.type >= 420U && item.type <= 444U && weapon != 0U) {
      if (item.quantity != 1U ||
          !actor.survival_primary_weapons.contains(weapon))
        return reject("primary_not_owned_or_quantity_mismatch");
    } else if (item.type >= 410U && item.type <= 414U) {
      if (item.quantity != 1U ||
          actor.participant.weapon_templates[1U] != weapon)
        return reject("pistol_not_owned_or_quantity_mismatch");
    } else if (item.type == 580U) {
      if (partial ? observed > actor.survival_medkits
                  : item.quantity != actor.survival_medkits)
        return reject("medicine_quantity_mismatch");
    } else {
      const auto held = actor.survival_supplies.find(item.type);
      if (item.type == 570U || (item.type >= 450U && item.type <= 454U) ||
          held == actor.survival_supplies.end() || item.quantity > held->second)
        return reject("supply_not_owned_or_quantity_mismatch");
      if (partial && observed > held->second)
        return reject("observed_stack_exceeds_owned_quantity");
    }
  }
  if (!combat.ConsumeActionQuota(actor, now)) return reject("rate_limit");
  AllocateSurvivalLootIds(items);
  for (auto& item : items) {
    item.active_at = now + 1s;
    item.expires = now + 15s;
  }
  auto messages = EncodeSurvivalLoot(items);
  // 52F630 -> 5366A0 removes the whole local slot after sending 750.
  // Ammo may be less than the acquired total after local firing; discard that
  // difference, never refund it. Complete firing/reload authority is separate.
  for (const auto& item : items) {
    const auto weapon = SurvivalLootWeapon(item.type);
    if (item.type >= 420U && item.type <= 444U && weapon != 0U) {
      actor.survival_primary_weapons.erase(weapon);
      if (actor.participant.weapon_templates[0U] == weapon)
        actor.participant.weapon_templates[0U].reset();
    } else if (item.type >= 410U && item.type <= 414U) {
      actor.participant.weapon_templates[1U].reset();
    } else if (item.type == 580U) {
      actor.survival_medkits =
          static_cast<std::uint16_t>(partial ? observed - item.quantity : 0U);
      if (actor.survival_medkits == 0U && actor.survival_treatment_start) {
        actor.survival_treatment_start.reset();
        protocol::PacketWriter stopped;
        stopped.WriteU32(actor.participant.actor_object_id);
        messages.push_back({.opcode = 33271U, .body = stopped.Release()});
      }
    } else {
      const auto remaining = partial ? observed - item.quantity : 0U;
      if (remaining != 0U)
        actor.survival_supplies[item.type] = remaining;
      else
        actor.survival_supplies.erase(item.type);
      if (remaining == 0U && item.type == 560U &&
          actor.participant.weapon_templates[3U] == 5921U)
        actor.participant.weapon_templates[3U] = actor.initial_weapons[3U];
    }
  }
  std::erase_if(survival_loot_, [now](const auto& item) {
    return item.expires && now >= *item.expires;
  });
  survival_loot_.insert(survival_loot_.end(), items.begin(), items.end());
  drops_.insert(std::move(signature));
  MatchObjectiveUpdate result{.messages = std::move(messages)};
  if (partial) {
    auto reply = ack(0U, observed - items.front().quantity);
    partial_drop_replies_.emplace(reply_key,
                                  PartialDropReply{packet.body, reply});
    result.deliveries.push_back({player, std::move(reply)});
  }
  return result;
}

MatchObjectiveUpdate MatchObjectives::CollectSurvivalLoot(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  using namespace std::chrono_literals;
  const auto reject = [](const char* detail) {
    return MatchObjectiveUpdate{
        .rejection_detail = std::string{"survival pickup rejected: "} + detail};
  };
  const auto owner = combat.actors_.find(player);
  if ((mode_ != 23U && mode_ != 25U) || owner == combat.actors_.end() ||
      owner->second.controller)
    return reject("unsupported_mode_or_source");
  auto& actor = owner->second;
  protocol::PacketReader reader{packet.body};
  const auto id = reader.ReadU32();
  if (reader.ReadU32() != actor.participant.actor_object_id)
    return reject("foreign_collector");
  if (actor.life_state != CombatLifeState::kAlive)
    return reject("owner_not_alive");
  if (combat.survival_traps_.contains(id)) {
    if (!combat.CollectSurvivalTrap(actor, id, now))
      return reject("trap_rod_not_collectible");
    return {.messages = {{.opcode = 753U, .body = packet.body}}};
  }
  const auto item = std::ranges::find(survival_loot_, id, &SurvivalLoot::id);
  if (item == survival_loot_.end() || survival_loot_pending_ ||
      now < item->active_at || (item->expires && now >= *item->expires))
    return reject("unknown_consumed_or_not_active_object");
  // 57FC30 -> 5838A0 -> 7CCC90 reports the local E interaction, without
  // a position sample. Movement can be missing/stale and GoGenItem moves
  // under native physics. Its original spawn point is not a pickup position.
  // Authenticate the current object/life/inventory and consume it once.
  const auto type = item->type;
  const auto weapon = SurvivalLootWeapon(type);
  const bool primary = type >= 420U && type <= 444U;
  const bool pistol = type >= 410U && type <= 414U;
  if (primary && (actor.survival_primary_weapons.contains(weapon) ||
                  actor.survival_primary_weapons.size() >= 3U))
    return reject("duplicate_primary_or_full_weapon_slots");
  // 536880 returns 2 for an occupied pistol slot and 3 for the same pistol.
  // 57FC30 displays the native refusal in either case. Replacement requires
  // an authenticated 750 drop first; 753 never grants an implicit exchange.
  if (pistol && actor.participant.weapon_templates[1U])
    return reject(actor.participant.weapon_templates[1U] == weapon
                      ? "duplicate_pistol"
                      : "occupied_pistol_slot");
  const auto held = actor.survival_supplies.find(type);
  const auto quantity = type == 580U ? actor.survival_medkits
                        : held == actor.survival_supplies.end() ? 0U
                                                                : held->second;
  if (!primary && !pistol && type != 570U &&
      (quantity >= 999U ||
       (held == actor.survival_supplies.end() && type != 580U &&
        actor.survival_supplies.size() >= 10U)))
    return reject("full_supply_stack_or_slots");
  if (!combat.ConsumeActionQuota(actor, now)) return reject("rate_limit");
  if (primary) {
    actor.survival_primary_weapons.insert(weapon);
    // 538C10 equips the first primary automatically; later ones stay in the
    // backpack until an authenticated switch names an actually acquired gun.
    if (!actor.participant.weapon_templates[0U])
      actor.participant.weapon_templates[0U] = weapon;
  } else if (pistol) {
    actor.participant.weapon_templates[1U] = weapon;
  } else if (type == 580U) {
    actor.survival_medkits =
        static_cast<std::uint16_t>(std::min(999U, quantity + item->quantity));
  } else if (type != 570U) {
    actor.survival_supplies[type] = std::min(999U, quantity + item->quantity);
  }
  // 570 is an immediate native armour effect, not an inventory stack. 753
  // executes it on the owner. This ledger does not claim authoritative HP.
  survival_loot_.erase(item);
  return {.messages = {{.opcode = 753U, .body = packet.body}}};
}
}  // namespace bf::runtime
