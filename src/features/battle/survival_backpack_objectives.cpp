#include <algorithm>
#include <limits>

#include "features/battle/match_objectives.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
bool WithinBackpackRange(const std::array<float, 3U>& first,
                         const std::array<float, 3U>& second) {
  double squared = 0.0;
  for (std::size_t i = 0; i < 3U; ++i) {
    const double delta = first[i] - second[i];
    squared += delta * delta;
  }
  return squared <= 300.0 * 300.0;
}
}  // namespace

void MatchObjectives::ObserveSurvivalDefeats(CombatRelay& combat, Time now,
                                             MatchObjectiveUpdate& result) {
  const auto remaining =
      std::ranges::count_if(participants_, [&combat](const auto& player) {
        const auto life = combat.LifeState(player.player_id);
        return life && *life != CombatLifeState::kDefeated;
      });
  for (const auto& player : participants_) {
    const auto found = combat.actors_.find(player.player_id);
    if (found == combat.actors_.end() || found->second.controller ||
        found->second.life_state != CombatLifeState::kDefeated)
      continue;
    const auto life = found->second.life_generation;
    const auto previous = backpack_permits_.find(player.player_id);
    if (previous != backpack_permits_.end() && previous->second.life == life)
      continue;
    backpack_permits_[player.player_id] = {life, now + std::chrono::seconds{10},
                                           false};
    // 5E1E50 -> 5E3590: remaining count and native seat (not rank/player/object
    // ID).
    protocol::PacketWriter writer;
    writer.WriteU32(static_cast<std::uint32_t>(remaining));
    writer.WriteU32(player.seat_index);
    result.messages.push_back({.opcode = 759U, .body = writer.Release()});
  }
}

void MatchObjectives::ObserveSurvivalSpectators(const CombatRelay& combat,
                                                MatchObjectiveUpdate& result) {
  for (const auto& player : participants_) {
    const auto found = combat.actors_.find(player.player_id);
    if (found == combat.actors_.end() || found->second.controller ||
        found->second.life_state != CombatLifeState::kDefeated ||
        found->second.death_stage != 2U)
      continue;
    const auto life = found->second.life_generation;
    const auto previous = survival_spectators_.find(player.player_id);
    if (previous != survival_spectators_.end() && previous->second == life)
      continue;
    // 7E6390 -> 7E6140 -> ChrLocalPlayer::5822A0: 980 carries a
    // Character GameObjectID. Broadcast includes the eliminated local player;
    // the native client owns target selection and retains the original team.
    protocol::PacketWriter writer;
    writer.WriteU32(player.actor_object_id);
    result.messages.push_back({.opcode = 980U, .body = writer.Release()});
    survival_spectators_[player.player_id] = life;
  }
}

MatchObjectiveUpdate MatchObjectives::SurvivalBackpack(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  const auto reject = [](const char* detail) {
    return MatchObjectiveUpdate{
        .rejection_detail =
            std::string{"survival backpack rejected: "} + detail};
  };
  const auto found = combat.actors_.find(player);
  if ((mode_ != 23U && mode_ != 25U) || found == combat.actors_.end() ||
      found->second.controller)
    return reject("unsupported_mode_or_source");
  auto& actor = found->second;
  if (survival_loot_pending_) return reject("map_not_started");
  if (packet.opcode == 748U) {
    auto bag = DecodeSurvivalBackpack(packet.body);
    const auto permit = backpack_permits_.find(player);
    const auto name = survival_nicknames_.find(player);
    if (bag.actor != actor.participant.actor_object_id)
      return reject("foreign_owner");
    if (actor.life_state != CombatLifeState::kDefeated ||
        permit == backpack_permits_.end() ||
        permit->second.life != actor.life_generation ||
        now >= permit->second.expires || permit->second.submitted)
      return reject("no_current_defeat_permit_or_already_submitted");
    if (name == survival_nicknames_.end() || !actor.position ||
        !WithinBackpackRange(*actor.position, bag.position))
      return reject("trusted_name_missing_or_out_of_range");
    if (bag.items.empty() || survival_backpacks_.size() >= participants_.size())
      return reject("empty_submission_or_object_limit");
    std::set<std::uint32_t> types, primary;
    bool medicine = false;
    for (const auto& item : bag.items) {
      if (!types.insert(item.type).second) return reject("duplicate_type");
      const auto weapon = SurvivalLootWeapon(item.type);
      if (item.type >= 420U && item.type <= 444U && weapon != 0U) {
        if (item.weapon != weapon || item.quantity != 1U ||
            !actor.survival_primary_weapons.contains(weapon))
          return reject("primary_not_owned_or_mismatch");
        primary.insert(weapon);
      } else if (item.type == 580U) {
        // Initial medicine is seeded by 538870 with template -1. A new
        // stack picked from GenItemSpec carries 0; 539010 uses fixed bar 4.
        if ((item.weapon != 0U && item.weapon != 0xFFFFFFFFU) ||
            item.quantity != actor.survival_medkits)
          return reject("medicine_mismatch");
        medicine = true;
      } else {
        const auto held = actor.survival_supplies.find(item.type);
        if (held == actor.survival_supplies.end() || item.weapon != weapon ||
            item.quantity > held->second)
          return reject("supply_not_owned_or_mismatch");
      }
    }
    if (primary != actor.survival_primary_weapons ||
        (actor.survival_medkits != 0U && !medicine))
      return reject("missing_primary_or_medicine");
    if (!combat.ConsumeActionQuota(actor, now)) return reject("rate_limit");
    bag.nickname = name->second;
    std::erase_if(bag.items,
                  [](const auto& item) { return item.quantity == 0U; });
    MatchObjectiveUpdate result;
    if (!bag.items.empty()) {
      std::array<SurvivalLoot, 1U> identity{};
      AllocateSurvivalLootIds(identity);
      bag.id = identity[0].id;
      result.messages.push_back(EncodeSurvivalBackpack(bag));
      survival_backpacks_.push_back(std::move(bag));
    }
    // 53A110 excludes the pistol. Remaining supply amounts can be lower
    // after local ammo consumption; discard every unreported remainder.
    actor.survival_primary_weapons.clear();
    actor.participant.weapon_templates[0U].reset();
    actor.survival_supplies.clear();
    actor.survival_medkits = 0U;
    actor.participant.weapon_templates[3U] = actor.initial_weapons[3U];
    permit->second.submitted = true;
    return result;
  }

  if (actor.life_state != CombatLifeState::kAlive ||
      player.Value() > std::numeric_limits<std::uint32_t>::max())
    return reject("collector_not_alive_or_invalid_identity");
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  const auto type = reader.ReadU32();
  const auto bag =
      std::ranges::find(survival_backpacks_, object, &SurvivalBackpack::id);
  // Like 747, 749 reports a local E interaction without a fresh position.
  // The death permit and full inventory check above authenticated the bag;
  // pickup authenticates its remaining contents and consumes each item once.
  if (bag == survival_backpacks_.end()) return reject("unknown_bag");
  const auto item =
      std::ranges::find(bag->items, type, &SurvivalBackpackItem::type);
  if (item == bag->items.end()) return reject("unknown_or_consumed_item");
  const bool primary = type >= 420U && type <= 444U;
  if (primary && (actor.survival_primary_weapons.contains(item->weapon) ||
                  actor.survival_primary_weapons.size() >= 3U))
    return reject("duplicate_primary_or_full_weapon_slots");
  const auto held = actor.survival_supplies.find(type);
  const auto quantity = type == 580U ? actor.survival_medkits
                        : held == actor.survival_supplies.end() ? 0U
                                                                : held->second;
  if (!primary && (quantity >= 999U ||
                   (type != 580U && held == actor.survival_supplies.end() &&
                    actor.survival_supplies.size() >= 10U)))
    return reject("full_supply_stack_or_slots");
  if (!combat.ConsumeActionQuota(actor, now)) return reject("rate_limit");
  if (primary) {
    actor.survival_primary_weapons.insert(item->weapon);
    if (!actor.participant.weapon_templates[0U])
      actor.participant.weapon_templates[0U] = item->weapon;
  } else if (type == 580U) {
    actor.survival_medkits =
        static_cast<std::uint16_t>(std::min(999U, quantity + item->quantity));
  } else {
    actor.survival_supplies[type] = std::min(999U, quantity + item->quantity);
  }
  bag->items.erase(item);
  if (bag->items.empty()) survival_backpacks_.erase(bag);
  // 7C5710 -> 52F340 compares Character+860, initialized from UserSlot+8.
  // That value is the authenticated player ID, not native seat or actor ID.
  protocol::PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(player.Value()));
  writer.WriteU32(object);
  writer.WriteU32(type);
  return {.messages = {{.opcode = 755U, .body = writer.Release()}}};
}
}  // namespace bf::runtime
