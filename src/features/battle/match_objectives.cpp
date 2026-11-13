#include "features/battle/match_objectives.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "features/battle/native_item_rules.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/packet_io.h"
#include "protocol/lobby/menu_messages.h"

namespace bf::runtime {
namespace {
using Time = std::chrono::steady_clock::time_point;
#include "features/battle/generated/native_bomb_supplies.inc"
#include "features/battle/generated/native_infection_supplies.inc"
#include "features/battle/generated/native_sniper_supplies.inc"

bool NativeItemType(std::uint32_t type) {
  // ItemSpecItem: union of positive native draw weights. Random selection is
  // performed by 92D370 -> B3D900/B3DD40 and arrives in C2S707, including
  // team/score weighting.
  constexpr std::uint32_t types[]{101U, 108U, 109U, 112U, 115U, 122U,
                                  126U, 127U, 129U, 130U, 136U, 138U,
                                  141U, 300U, 302U, 303U};
  return std::ranges::find(types, type) != std::end(types);
}

std::array<float, 3U> Vector3(protocol::PacketReader& reader,
                              float bound = 10'000'000.0F) {
  std::array<float, 3U> value{reader.ReadF32(), reader.ReadF32(),
                              reader.ReadF32()};
  for (const auto component : value) {
    if (!std::isfinite(component) || std::abs(component) > bound) {
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid objective vector"};
    }
  }
  return value;
}

bool Nearby(const CombatRelay& combat, domain::PlayerId player,
            const std::array<float, 3U>& point) {
  const auto position = combat.LastPosition(player);
  if (!position.has_value()) {
    return false;
  }
  double squared = 0.0;
  for (std::size_t i = 0U; i < 3U; ++i) {
    const auto delta = static_cast<double>((*position)[i]) - point[i];
    squared += delta * delta;
  }
  return squared <= 90'000.0;
}

void WriteVector(protocol::PacketWriter& writer,
                 const std::array<float, 3U>& value) {
  for (const auto component : value) {
    writer.WriteF32(component);
  }
}
}  // namespace

MatchObjectives::MatchObjectives(MatchObjectiveRules rules,
                                 std::vector<CombatParticipant> participants,
                                 const protocol::MapAssetsRequest& assets,
                                 Time now)
    : survival_nicknames_(std::move(rules.nicknames)),
      mode_(rules.mode),
      target_(rules.target),
      participants_(std::move(participants)) {
  if (mode_ == 23U || mode_ == 25U) {
    survival_tide_due_ = now;
    survival_loot_ = GenerateSurvivalLoot(rules.map_key, supply_random_());
    AllocateSurvivalLootIds(survival_loot_);
    survival_loot_pending_ = !survival_loot_.empty();
  }
  // 541710 读取 RenewalBomb*TimeN；GameOption.xml 的有效 N 为 2/4/6/8/10/12。
  // 未列出的奇数人数使用 BombInstallTime/BombUninstallTime
  // 回退，不采用反编译默认 7000。
  if (mode_ == 19U) {
    bomb_map_ = FindBombMap(rules.map_key);
    const auto count = participants_.size();
    if (count == 6U) {
      plant_duration_ = std::chrono::milliseconds{4000};
      defuse_duration_ = std::chrono::milliseconds{5500};
    } else if (count == 8U || count == 10U || count == 12U) {
      plant_duration_ = std::chrono::milliseconds{5000};
      defuse_duration_ = std::chrono::milliseconds{6000};
    }
    if (count / 2U == 3U) {
      fuse_duration_ = std::chrono::milliseconds{35000};
    } else if (count / 2U >= 4U && count / 2U <= 6U) {
      fuse_duration_ = std::chrono::milliseconds{40000};
    }
  }
  for (const auto& player : participants_) {
    // 95A600 -> 7C58B0(6) -> 5ECD70 reports StarModeSpec +16/+148.
    // StarMode.xml fixes these at 10/5. Seed once from the frozen roster:
    // identical C2S2026 reports have no request ID and must never mint stars.
    scores_.emplace(player.player_id, mode_ == 5U   ? 10U
                                      : mode_ == 6U ? 5U
                                                    : 0U);
  }
  supplies_started_at_ = now;
  if (mode_ == 19U) {
    // 53ED20 -> 53F100 gathers Sniper markers from the released bomb map.
    // Freeze original anchors even when the host's 702 is empty or arrives
    // late.
    protocol::MapAssetsRequest native;
    for (const auto& point : kNativeBombSupplies) {
      if (point.map != rules.map_key) continue;
      native.field_0.push_back(
          {point.position[0], point.position[1], point.position[2]});
      native.field_1.push_back(point.type);
      native.field_2.push_back(0U);
      native.field_3.push_back(point.minimum_ms);
      native.field_4.push_back(point.maximum_ms);
    }
    LoadSupplies(native, now);
  } else if (mode_ == 12U) {
    // DF1910 -> 53F100 uses these original Sniper markers. Keep known maps
    // usable when the host omits 702 or uploads it after match installation.
    protocol::MapAssetsRequest native;
    for (const auto& point : kNativeSniperSupplies) {
      if (point.map != rules.map_key) continue;
      native.field_0.push_back(
          {point.position[0], point.position[1], point.position[2]});
      native.field_1.push_back(point.type);
      native.field_2.push_back(0U);
      native.field_3.push_back(point.minimum_ms);
      native.field_4.push_back(point.maximum_ms);
    }
    LoadSupplies(native.field_0.empty() ? assets : native, now);
  } else {
    LoadSupplies(assets, now);
  }
  if ((mode_ == 8U || mode_ == 28U) && supplies_.empty()) {
    // A86910 gathers these exact WB markers. An empty/invalid host 702 must not
    // permanently disable supplies on a known original map.
    protocol::MapAssetsRequest native;
    for (const auto& point : kNativeInfectionSupplies) {
      if (point.map != rules.map_key) continue;
      native.field_0.push_back(
          {point.position[0], point.position[1], point.position[2]});
      native.field_1.push_back(point.type);
      native.field_2.push_back(0U);
      native.field_3.push_back(point.minimum_ms);
      native.field_4.push_back(point.maximum_ms);
    }
    LoadSupplies(native, now);
  }
  if (mode_ != 5U && mode_ != 6U && mode_ != 7U) {
    return;
  }
  const auto delay = [](std::uint32_t value) {
    return std::chrono::milliseconds{
        value >= 1000U && value <= 300'000U ? value : 15'000U};
  };
  for (std::size_t i = 0U; i < assets.field_6.size(); ++i) {
    const auto kind = i < assets.field_5.size() ? assets.field_5[i] : 1U;
    if (kind != 1U && kind != 2U) {
      continue;
    }
    const auto& point = assets.field_6[i];
    Collectible item;
    item.position = {point.x, point.y, point.z};
    item.kind = kind;
    item.due = now;
    item.delay = delay(i < assets.field_7.size() ? assets.field_7[i] : 15'000U);
    item.spawn_opcode = mode_ == 7U ? 718U : 716U;
    item.collect_opcode = mode_ == 7U ? 734U : 732U;
    item.value = kind == 2U ? (mode_ == 7U ? 5U : 10U) : 1U;
    collectibles_.push_back(item);
  }
  if (mode_ == 7U) {
    for (std::size_t i = 0U; i < assets.field_10.size(); ++i) {
      const auto& point = assets.field_10[i];
      Collectible item;
      item.position = {point.x, point.y, point.z};
      item.due = now;
      item.delay =
          delay(i < assets.field_11.size() ? assets.field_11[i] : 15'000U);
      item.spawn_opcode = 720U;
      item.collect_opcode = 736U;
      collectibles_.push_back(item);
    }
  }
}

bool MatchObjectives::RecoverInfectionSupplies(
    const protocol::MapAssetsRequest& assets) {
  if ((mode_ != 8U && mode_ != 28U) || !supplies_.empty()) return false;
  LoadSupplies(assets, supplies_started_at_);
  return !supplies_.empty();
}

void MatchObjectives::LoadSupplies(const protocol::MapAssetsRequest& assets,
                                   Time now) {
  if (mode_ == 3U || mode_ == 8U || mode_ == 28U || mode_ == 12U ||
      mode_ == 13U || mode_ == 19U || mode_ == 101U || mode_ == 102U) {
    // 7C29F0/7ED6F0：坐标、类型、队伍、再生下限/上限为前五个平行向量。
    const auto count = assets.field_0.size();
    if (assets.field_1.size() != count || assets.field_2.size() != count ||
        assets.field_3.size() != count || assets.field_4.size() != count) {
      return;
    }
    for (std::size_t i = 0U; i < count; ++i) {
      const auto type = assets.field_1[i];
      const bool supported =
          mode_ == 3U                     ? (type == 0U || NativeItemType(type))
          : (mode_ == 8U || mode_ == 28U) ? (type == 10U || type == 11U)
          : (mode_ == 12U || mode_ == 13U || mode_ == 19U)
              ? (type == 15U || type == 16U)
              : (type == 10U || type == 11U || type == 15U || type == 16U);
      if (!supported || assets.field_2[i] > 2U) {
        continue;
      }
      const auto& point = assets.field_0[i];
      if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z) || std::abs(point.x) > 10'000'000.0F ||
          std::abs(point.y) > 10'000'000.0F ||
          std::abs(point.z) > 10'000'000.0F)
        continue;
      Supply item;
      item.position = {point.x, point.y, point.z};
      item.type = type;
      item.team = assets.field_2[i];
      item.due = now;
      const auto minimum = assets.field_3[i];
      const auto maximum = assets.field_4[i];
      if (minimum >= 1000U && maximum >= minimum && maximum <= 300'000U) {
        item.minimum_ms = minimum;
        item.maximum_ms = maximum;
      }
      if (mode_ == 8U || mode_ == 28U) {
        // Infection supplies form one wave per match minute. Per-marker random
        // timers (and pickup-relative regeneration) split a wave across
        // clients.
        item.minimum_ms = item.maximum_ms = 60'000U;
        item.due = now + std::chrono::minutes{1};
      }
      supplies_.push_back(item);
    }
  }
}

void MatchObjectives::StartBombRound(Time now) {
  if (mode_ != 19U) return;
  bomb_spectators_.clear();
  for (auto& item : supplies_) {
    item.id = 0U;
    item.due = now;
  }
  supply_effects_.clear();
  supplies_started_at_ = now;
}

bool MatchObjectives::Accepts(std::uint16_t opcode) noexcept {
  return (opcode >= 32878U && opcode <= 32880U) || opcode == 707U ||
         opcode == 708U || opcode == 745U || opcode == 729U || opcode == 730U ||
         opcode == 731U || opcode == 732U || opcode == 733U || opcode == 734U ||
         opcode == 736U || opcode == 737U || opcode == 738U || opcode == 739U ||
         opcode == 742U || opcode == 743U || opcode == 747U || opcode == 748U ||
         opcode == 749U || opcode == 750U || opcode == 65080U ||
         opcode == 2014U || opcode == 2015U || opcode == 2024U ||
         opcode == 2026U || opcode == 32894U;
}

void MatchObjectives::Validate(const protocol::Packet& packet) {
  if (packet.opcode == 65080U) {
    protocol::PacketReader reader{packet.body};
    const auto serial = reader.ReadU32();
    const auto observed = reader.ReadU32();
    const auto items =
        DecodeSurvivalDrop(reader.ReadBytes(reader.RemainingBytes()));
    if (serial == 0U || observed == 0U || observed > 999U ||
        items.size() != 1U || items.front().quantity > observed ||
        items.front().type < 500U || items.front().type == 570U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid partial survival stack request"};
    return;
  }
  if (packet.opcode == 729U) {
    static_cast<void>(protocol::DecodePveItemDrop(packet.body));
    return;
  }
  if (packet.opcode == 2024U) {
    protocol::ValidateNativeAuxiliaryReport(packet.opcode, packet.body);
    return;
  }
  if (packet.opcode == 748U) {
    static_cast<void>(DecodeSurvivalBackpack(packet.body));
    return;
  }
  if (packet.opcode == 750U) {
    static_cast<void>(DecodeSurvivalDrop(packet.body));
    return;
  }
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 710U) {
    // 7ED370: u8 event, u32 argument1/2, u8 flags[3], u16 values[2].
    const auto event = reader.ReadU8();
    const auto argument = reader.ReadU32();
    static_cast<void>(reader.ReadBytes(11U));
    if (event == 21U && argument > 1U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid soccer event goal index"};
  } else if (packet.opcode == 32878U) {
    if (reader.ReadU8() > 1U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid soccer goal index"};
  } else if (packet.opcode == 32879U || packet.opcode == 32880U) {
    // Native transition requests carry no payload.
  } else if (packet.opcode == 708U || packet.opcode == 745U) {
    // 7C4030 / 7E1110: use / change are empty reliable requests; the server
    // owns selection.
  } else if (packet.opcode == 730U) {
    static_cast<void>(Vector3(reader));
    static_cast<void>(Vector3(reader, 100.0F));
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() > 4U || reader.ReadU32() > 1U)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kInvalidValue,
          "invalid native weapon drop bar or expiry flag"};
  } else if (packet.opcode == 733U || packet.opcode == 737U ||
             packet.opcode == 739U || packet.opcode == 743U) {
    static_cast<void>(Vector3(reader));
    static_cast<void>(Vector3(reader, 100.0F));
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
  } else if (packet.opcode == 2015U) {
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(Vector3(reader));
  } else if (packet.opcode == 2014U) {
    static_cast<void>(reader.ReadU8());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
  } else if (packet.opcode == 2026U) {
    // 5ECD70 serializes a BYTE event, DWORD object and BYTE native seat.
    static_cast<void>(reader.ReadU8());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU8());
  } else if (packet.opcode == 707U || packet.opcode == 731U ||
             packet.opcode == 732U || packet.opcode == 734U ||
             packet.opcode == 736U || packet.opcode == 738U ||
             packet.opcode == 742U || packet.opcode == 747U ||
             packet.opcode == 749U || packet.opcode == 32894U) {
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
  } else {
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "unknown objective request"};
  }
  reader.RequireEnd();
}

const CombatParticipant* MatchObjectives::Participant(
    domain::PlayerId player) const noexcept {
  const auto found =
      std::ranges::find(participants_, player, &CombatParticipant::player_id);
  return found == participants_.end() ? nullptr : &*found;
}

MatchObjectiveUpdate MatchObjectives::Handle(domain::PlayerId player,
                                             const protocol::Packet& packet,
                                             CombatRelay& combat, Time now) {
  Validate(packet);
  if (Participant(player) == nullptr) {
    return {};
  }
  if (mode_ == 19U && (BetweenRounds() || winner_ != 0U)) return {};
  if (packet.opcode == 2024U) {
    auto result = PirateCoin(player, packet, combat, now);
    if (result.rejection_detail == "pirate coin rejected: no_recent_npc_pang" &&
        std::ranges::any_of(combat.actors_,
                            [&](const auto& row) {
                              return row.second.pirate_coin_source &&
                                     row.second.controller == player &&
                                     row.second.life_state !=
                                         CombatLifeState::kDefeated;
                            }) &&
        pending_pirate_coins_.size() < 64U &&
        std::ranges::none_of(pending_pirate_coins_, [&](const auto& entry) {
          return entry.controller == player && entry.packet.body == packet.body;
        })) {
      // The native local defeat callback can enqueue 2024 before its combat
      // report. Retain only briefly; a confirmed, unclaimed death is still
      // mandatory.
      pending_pirate_coins_.push_back(
          {player, packet, now, now + std::chrono::milliseconds{50}});
      result.rejection_detail.reset();
    }
    return result;
  }
  bool item_fever = false;
  if (mode_ == 3U && (packet.opcode == 707U || packet.opcode == 32894U)) {
    protocol::PacketReader reader{packet.body};
    static_cast<void>(reader.ReadU32());
    item_fever = reader.ReadU32() == 104U;
  }
  if ((mode_ == 13U || mode_ == 14U || mode_ == 18U || item_fever) &&
      (packet.opcode == 707U || packet.opcode == 32894U)) {
    protocol::PacketReader reader{packet.body};
    const auto object = reader.ReadU32();
    const auto type = reader.ReadU32();
    const auto* owner = Participant(player);
    const bool apply = packet.opcode == 32894U;
    // SinglePvp also uses real map supply objects (15/16). Only 104 belongs
    // to the once-per-respawn fever handshake with the sentinel object ID.
    if (type == 104U) {
      if (object != (apply ? owner->actor_object_id : 0xFFFFFFFFU) ||
          !combat.DeathmatchFeverItem(player, apply, now))
        return {
            .rejection_detail =
                "native respawn fever has no current once-only entitlement"};
      if (apply)
        return {.messages = {{.opcode = 32894U, .body = packet.body}},
                .exclude_recipient = player};
      protocol::PacketWriter body;
      body.WriteU32(owner->seat_index);
      body.WriteU32(104U);
      body.WriteU32(0U);
      return {.messages = {{.opcode = 715U, .body = body.Release()}}};
    }
  }
  if (packet.opcode == 747U)
    return CollectSurvivalLoot(player, packet, combat, now);
  if (packet.opcode == 750U || packet.opcode == 65080U)
    return DropSurvivalLoot(player, packet, combat, now);
  if (packet.opcode == 748U || packet.opcode == 749U)
    return SurvivalBackpack(player, packet, combat, now);
  if (packet.opcode == 2026U) {
    protocol::PacketReader reader{packet.body};
    const auto event = reader.ReadU8();
    const auto object = reader.ReadU32();
    const auto seat = reader.ReadU8();
    const auto* owner = Participant(player);
    if ((mode_ != 5U && mode_ != 6U) ||
        (event != 6U && event != 7U && event != 15U && event != 16U) ||
        object != owner->actor_object_id || seat != owner->seat_index) {
      return {.rejection_detail =
                  "native 2026 event is unsupported or has a foreign identity"};
    }
    // Events 6/15 mirror initial or collected normal/big stars; 7/16 mirror
    // normal/big 733 transfers (95A600/A65280/A64A70/82F180). None is a
    // second source of credit/debit. Initial/live/final 729 publishes the
    // ledger.
    return {};
  }
  if (packet.opcode == 710U ||
      (packet.opcode >= 32878U && packet.opcode <= 32880U))
    return Soccer(player, packet, combat, now);
  if (packet.opcode == 730U || packet.opcode == 731U)
    return WeaponDrop(player, packet, combat, now);
  if (packet.opcode == 729U)
    return mode_ == 3U ? DropItemBox(player, packet, combat, now)
                       : MatchObjectiveUpdate{};
  if (packet.opcode == 708U) {
    return mode_ == 3U ? UseItem(player, combat, now) : MatchObjectiveUpdate{};
  }
  if (packet.opcode == 745U) {
    return mode_ == 3U ? ChangeItem(player, combat) : MatchObjectiveUpdate{};
  }
  if (packet.opcode == 32894U) {
    return (mode_ == 3U || mode_ == 8U || mode_ == 28U || mode_ == 12U ||
            mode_ == 13U || mode_ == 19U || mode_ == 101U || mode_ == 102U)
               ? ApplySupplyEffect(player, packet, combat, now)
               : MatchObjectiveUpdate{};
  }
  if (packet.opcode == 707U) {
    return (mode_ == 3U || mode_ == 8U || mode_ == 28U || mode_ == 12U ||
            mode_ == 13U || mode_ == 19U || mode_ == 101U || mode_ == 102U)
               ? CollectSupply(player, packet, combat, now)
               : MatchObjectiveUpdate{};
  }
  if (packet.opcode == 2015U) {
    return mode_ == 19U ? Bomb(player, packet, combat, now)
                        : MatchObjectiveUpdate{};
  }
  if (packet.opcode == 739U || packet.opcode == 743U ||
      (packet.opcode == 737U && packet.body[28U] == 5U))
    return Loot(player, packet, combat, now);
  if (packet.opcode == 738U || packet.opcode == 742U || packet.opcode == 736U)
    return Collect(player, packet, combat, now);
  if (packet.opcode == 2014U || (mode_ != 5U && mode_ != 6U && mode_ != 7U)) {
    return {};
  }
  return Collect(player, packet, combat, now);
}

MatchObjectiveUpdate MatchObjectives::CollectSupply(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  protocol::PacketReader reader{packet.body};
  const auto id = reader.ReadU32();
  const auto type = reader.ReadU32();
  const auto* owner = Participant(player);
  const auto found = std::ranges::find(supplies_, id, &Supply::id);
  const auto rejected = [&](std::string_view reason) {
    return MatchObjectiveUpdate{
        .rejection_detail =
            "supply pickup rejected: opcode=707 object=" + std::to_string(id) +
            " type=" + std::to_string(type) + " reason=" + std::string{reason}};
  };
  if (id == 0U || found == supplies_.end())
    return rejected("unknown_or_consumed_object");
  if (found->type == 0U && mode_ == 3U ? !NativeItemType(type)
                                       : type != found->type)
    return rejected("item_type_mismatch");
  const auto collector_state = combat.LifeState(player);
  if (collector_state != CombatLifeState::kAlive &&
      !(mode_ == 3U && collector_state == CombatLifeState::kBubbled))
    return rejected("collector_not_alive");
  if ((mode_ == 8U || mode_ == 28U) &&
      combat.actors_.at(player).participant.team_code != 1U)
    return rejected("zombie_cannot_collect_human_ammunition");
  if (found->team != 0U && found->team != owner->team_code)
    return rejected("team_mismatch");
  // Native 92D370 plays the pickup sound before sending 707 (object/type).
  // Position is not part of that request, and movement samples may be absent or
  // stale. Accept the authenticated collision report after
  // object/type/life/team checks; otherwise the client hears pickup but never
  // receives 713 for its slot.
  if (mode_ == 3U) {
    auto& held = held_items_[player];
    MatchObjectiveUpdate result;
    SynchronizeItemInventory(player, combat, result);
    const auto empty = std::ranges::find(held.types, 0U);
    if (empty == held.types.end()) return rejected("both_item_slots_occupied");
    const auto slot = static_cast<std::uint32_t>(empty - held.types.begin());
    // 84CAB0 changes selection only if the previously selected slot is empty.
    if (held.types[held.selected] == 0U) held.selected = slot;
    *empty = type;
    held.usable_at =
        std::max(held.usable_at, now + std::chrono::milliseconds{50});
    protocol::PacketWriter removed, acquired;
    removed.WriteU32(owner->seat_index);
    removed.WriteU32(id);
    acquired.WriteU32(owner->seat_index);
    acquired.WriteU32(type);
    acquired.WriteU32(slot);
    if (found->expires) {
      supplies_.erase(found);
    } else {
      found->id = 0U;
      found->due =
          now + std::chrono::milliseconds{
                    std::uniform_int_distribution<std::uint32_t>{
                        found->minimum_ms, found->maximum_ms}(supply_random_)};
    }
    result.messages.push_back({.opcode = 712U, .body = removed.Release()});
    result.messages.push_back({.opcode = 713U, .body = acquired.Release()});
    return result;
  }
  std::erase_if(supply_effects_, [&](const auto& effect) {
    return now >= effect.expires ||
           combat.LifeGeneration(effect.player) != effect.life;
  });
  if (supply_effects_.size() >= 128U) {
    return rejected("pending_effect_limit");
  }
  supply_effects_.push_back({player, found->type,
                             combat.LifeGeneration(player).value(),
                             now + std::chrono::seconds{10}});
  combat.AuthorizeItemEffects(player, found->type, mode_, now);
  protocol::PacketWriter removed;
  removed.WriteU32(owner->seat_index);
  removed.WriteU32(id);
  protocol::PacketWriter effect;
  effect.WriteU32(owner->seat_index);
  effect.WriteU32(found->type);
  effect.WriteU32(0U);
  found->id = 0U;
  if (mode_ == 8U || mode_ == 28U) {
    const auto wave =
        (now - supplies_started_at_) / std::chrono::minutes{1} + 1;
    found->due = supplies_started_at_ + std::chrono::minutes{wave};
  } else {
    found->due =
        now +
        std::chrono::milliseconds{std::uniform_int_distribution<std::uint32_t>{
            found->minimum_ms, found->maximum_ms}(supply_random_)};
  }
  // 712/715 使用席位；715 -> 840840 在拾取者本机发起 32894 和原生补弹。
  return {.messages = {{.opcode = 712U, .body = removed.Release()},
                       {.opcode = 715U, .body = effect.Release()}}};
}

MatchObjectiveUpdate MatchObjectives::DropItemBox(
    domain::PlayerId sender, const protocol::Packet& packet,
    const CombatRelay& combat, Time now) {
  const auto drop = protocol::DecodePveItemDrop(packet.body);
  const auto rejected = [](std::string_view why) {
    return MatchObjectiveUpdate{.rejection_detail = "item box drop rejected: " +
                                                    std::string{why}};
  };
  if (!NativeItemType(drop.type) || drop.team > 2U ||
      drop.lifetime_ms != 20'000U ||
      !std::ranges::all_of(
          drop.position,
          [](float v) { return std::isfinite(v) && std::abs(v) <= 1.0e7F; }) ||
      !std::ranges::all_of(drop.velocity, [](float v) {
        return std::isfinite(v) && std::abs(v) <= 1.0e4F;
      }))
    return rejected("invalid native type, team, lifetime or trajectory");
  const auto sender_actor = combat.actors_.find(sender);
  if (sender_actor == combat.actors_.end() || sender_actor->second.controller)
    return rejected("unknown sender");
  // 729 can follow its lifecycle packet before the next room timer fires.
  ObserveItemDrops(combat, now);
  const auto available = [&](const auto& entry) {
    return now < entry.second.expires && entry.second.used < 5U;
  };
  auto permit = item_drop_permits_.find(sender);
  if (permit == item_drop_permits_.end() || !available(*permit)) {
    if (combat.host_ != sender) return rejected("no recent lifecycle permit");
    permit = std::ranges::find_if(item_drop_permits_, available);
    if (permit == item_drop_permits_.end())
      return rejected("no recent lifecycle permit");
  }
  const auto active = std::ranges::count_if(
      supplies_, [](const Supply& item) { return item.expires.has_value(); });
  if (active >= 128 || item_drop_requests_.size() >= 1024U)
    return rejected("dynamic box capacity reached");
  protocol::PacketWriter signature;
  signature.WriteU64(permit->first.Value());
  signature.WriteU64(permit->second.life);
  signature.WriteBytes(packet.body);
  if (!item_drop_requests_.insert(signature.Release()).second)
    return rejected("duplicate drop request");
  ++permit->second.used;
  Supply item;
  item.position = drop.position;
  item.velocity = drop.velocity;
  item.type = drop.type;
  item.team = drop.team;
  item.due = now;
  item.expires = now + std::chrono::milliseconds{drop.lifetime_ms};
  supplies_.push_back(item);
  return {};
}

OutboundMessage MatchObjectives::ItemSelection(domain::PlayerId player,
                                               const HeldItem& held,
                                               bool success) const {
  protocol::PacketWriter writer;
  writer.WriteU32(Participant(player)->seat_index);
  writer.WriteU32(held.selected);
  const auto type = held.types[held.selected];
  writer.WriteU32(type == 0U ? 0xFFFFFFFFU : type);
  writer.WriteU32(success ? 0U : 1U);
  // 7E11B0 always releases the pending-change latch; status zero also selects
  // the slot.
  return {.opcode = 749U, .body = writer.Release()};
}

void MatchObjectives::SynchronizeItemInventory(domain::PlayerId player,
                                               const CombatRelay& combat,
                                               MatchObjectiveUpdate& result) {
  if (mode_ != 3U) return;
  const auto actor = combat.actors_.find(player);
  const auto entry = held_items_.find(player);
  if (actor == combat.actors_.end() || entry == held_items_.end()) return;
  auto& held = entry->second;
  if (held.generation != actor->second.item_inventory_generation)
    held = {.generation = actor->second.item_inventory_generation,
            .clear_pending = true};
  if (!held.clear_pending ||
      (actor->second.life_state != CombatLifeState::kAlive &&
       actor->second.life_state != CombatLifeState::kBubbled))
    return;
  // clearItemWhenDead=1 and85B710 clear both native security slots. The714
  // receiver 7D0230->840840 invokes85B670(slot) before the local effect path.
  // Use the empty sentinel, never a held type:840910(-1) performs no
  // attribute/effect activation. Synchronize after recovery because the
  // receiver's lifecycle guard may ignore a packet while dead;749 also clears
  // the pending-change latch and selected UI.
  for (std::uint32_t slot = 0U; slot < 2U; ++slot) {
    protocol::PacketWriter clear;
    clear.WriteU32(actor->second.participant.seat_index);
    clear.WriteU32(0xFFFFFFFFU);
    clear.WriteU32(slot);
    result.messages.push_back({.opcode = 714U, .body = clear.Release()});
  }
  result.messages.push_back(ItemSelection(player, held, true));
  held.clear_pending = false;
}

MatchObjectiveUpdate MatchObjectives::ChangeItem(domain::PlayerId player,
                                                 const CombatRelay& combat) {
  const auto actor = combat.actors_.find(player);
  if (actor == combat.actors_.end()) return {};
  auto& held = held_items_[player];
  MatchObjectiveUpdate result;
  SynchronizeItemInventory(player, combat, result);
  const auto next = 1U - held.selected;
  const bool accepted =
      actor->second.life_state != CombatLifeState::kDefeated &&
      held.types[next] != 0U;
  if (accepted) held.selected = next;
  result.messages.push_back(ItemSelection(player, held, accepted));
  return result;
}

MatchObjectiveUpdate MatchObjectives::UseItem(domain::PlayerId player,
                                              CombatRelay& combat, Time now) {
  const auto entry = held_items_.find(player);
  const auto actor = combat.actors_.find(player);
  if (entry == held_items_.end() || actor == combat.actors_.end()) return {};
  MatchObjectiveUpdate result;
  SynchronizeItemInventory(player, combat, result);
  auto& held = entry->second;
  const auto slot = held.selected;
  const auto type = held.types[slot];
  if (type == 0U ||
      held.generation != actor->second.item_inventory_generation ||
      now < held.usable_at ||
      (actor->second.life_state != CombatLifeState::kAlive &&
       !(actor->second.life_state == CombatLifeState::kBubbled &&
         ItemUsableInBubble(type))))
    return result;
  std::erase_if(supply_effects_, [&](const auto& effect) {
    return now >= effect.expires ||
           combat.LifeGeneration(effect.player) != effect.life;
  });
  if (supply_effects_.size() >= 128U) return {};
  supply_effects_.push_back({player, type, actor->second.life_generation,
                             now + std::chrono::seconds{10}});
  combat.AuthorizeItemEffects(player, type, mode_, now);
  held.types[slot] = 0U;
  held.usable_at = now + std::chrono::milliseconds{100};
  protocol::PacketWriter used;
  used.WriteU32(Participant(player)->seat_index);
  used.WriteU32(type);
  used.WriteU32(slot);
  // 714 clears just this slot (85B670). Select the remaining item explicitly
  // through 749.
  result.messages.push_back({.opcode = 714U, .body = used.Release()});
  if (held.types[1U - slot] != 0U) {
    held.selected = 1U - slot;
    result.messages.push_back(ItemSelection(player, held, true));
  }
  return result;
}

MatchObjectiveUpdate MatchObjectives::ApplySupplyEffect(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  protocol::PacketReader reader{packet.body};
  const auto actor = reader.ReadU32();
  const auto type = reader.ReadU32();
  const auto* owner = Participant(player);
  std::erase_if(supply_effects_, [&](const auto& effect) {
    return now >= effect.expires ||
           combat.LifeGeneration(effect.player) != effect.life;
  });
  const auto state = combat.LifeState(player);
  if (actor != owner->actor_object_id ||
      (state != CombatLifeState::kAlive &&
       !(mode_ == 3U && state == CombatLifeState::kBubbled &&
         ItemUsableInBubble(type)))) {
    return {};
  }
  const auto found =
      std::ranges::find_if(supply_effects_, [&](const auto& effect) {
        return effect.player == player && effect.type == type;
      });
  if (found == supply_effects_.end()) {
    return {};
  }
  supply_effects_.erase(found);
  if (mode_ == 3U && type == 136U) combat.ActivateAirstrike(player, now);
  return {.messages = {{.opcode = 32894U, .body = packet.body}},
          .exclude_recipient = player};
}

MatchObjectiveUpdate MatchObjectives::Collect(domain::PlayerId player,
                                              const protocol::Packet& packet,
                                              const CombatRelay& combat,
                                              Time now) {
  const auto* owner = Participant(player);
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 733U || packet.opcode == 737U) {
    const auto point = Vector3(reader);
    const auto velocity = Vector3(reader, 100.0F);
    const auto kind = reader.ReadU32();
    const auto variant = reader.ReadU32();
    const auto slot = reader.ReadU32();
    const bool coin = packet.opcode == 737U;
    if (coin != (mode_ == 7U) ||
        slot != (owner->actor_object_id - 1'000'000U) / 100'000U ||
        !Nearby(combat, player, point) || (kind != 1U && kind != 2U))
      return {};
    const auto& actor = combat.actors_.at(player);
    const auto humiliation_limit = coin ? 7U : mode_ == 6U ? 4U : 6U;
    std::uint64_t event_serial = 0U;
    if (actor.life_state == CombatLifeState::kDefeated &&
        actor.death_stage == 2U && variant == 1U) {
      const auto& event = actor.humiliated;
      if (!event || event->life != actor.life_generation ||
          now < event->completed ||
          now - event->completed > std::chrono::seconds{2})
        return {};
      event_serial = event->serial;
      const auto budget = humiliation_drops_.find(player);
      if (budget != humiliation_drops_.end() &&
          budget->second.first == event_serial &&
          (budget->second.second >= humiliation_limit ||
           (!coin && kind == 2U && budget->second.second != 0U)))
        return {};
    } else if (coin || actor.life_state != CombatLifeState::kDefeated ||
               variant != 2U) {
      // 737 variants 5/6 create weapon/NPC loot, not a transfer from held
      // coins.
      return {};
    }
    // 82F8D0 subtracts 100 for a large held coin, 1 for a small one.
    const auto value = kind == 2U ? (coin ? 100U : 10U) : 1U;
    protocol::PacketWriter signature;
    signature.WriteU64(player.Value());
    signature.WriteU64(actor.life_generation);
    signature.WriteU64(event_serial);
    signature.WriteU16(packet.opcode);
    signature.WriteBytes(packet.body);
    auto signature_bytes = signature.Release();
    if (scores_.at(player) < value || collectibles_.size() >= 4096U ||
        drops_.size() >= 16384U || drops_.contains(signature_bytes))
      return {};
    drops_.insert(std::move(signature_bytes));
    if (event_serial != 0U) {
      auto& budget = humiliation_drops_[player];
      if (budget.first != event_serial) budget = {event_serial, 0U};
      // 82F180 returns immediately after a single big-star drop. A loss
      // event chooses either that branch or bounded normal stars, never both.
      budget.second =
          !coin && kind == 2U ? humiliation_limit : budget.second + 1U;
    }
    scores_.at(player) -= value;
    Collectible item;
    item.position = point;
    item.velocity = velocity;
    item.kind = kind;
    item.variant = variant;
    item.value = value;
    item.spawn_opcode = coin ? 720U : 716U;
    item.collect_opcode = coin ? 736U : 732U;
    item.due = now;
    collectibles_.push_back(item);
    return {.messages = {}, .score_changed = true};
  }
  const auto actor = reader.ReadU32();
  const auto id = reader.ReadU32();
  const auto life = combat.LifeState(player);
  // The original collectible predicates8B1CB0/8B2090 include state300..399.
  // Pickup changes the object/score/HP effect, never the bubble lifecycle.
  if (actor != owner->actor_object_id ||
      (life != CombatLifeState::kAlive && life != CombatLifeState::kBubbled)) {
    return {};
  }
  const auto found = std::ranges::find(collectibles_, id, &Collectible::id);
  if (id == 0U || found == collectibles_.end() ||
      found->collect_opcode != packet.opcode ||
      (!found->pirate_coin && !Nearby(combat, player, found->position)) ||
      now < found->active_at || (found->expires && now >= *found->expires)) {
    return {};
  }
  const bool restoration = packet.opcode == 738U || packet.opcode == 742U;
  const bool bonus = found->bonus_lucci;
  auto& score_value = bonus ? bonus_lucci_[player] : scores_.at(player);
  score_value = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(score_value) +
                                  (restoration ? 0U : found->value),
                              std::numeric_limits<std::uint32_t>::max()));
  if (found->delay.has_value()) {
    found->id = 0U;
    found->due = now + *found->delay;
  } else {
    collectibles_.erase(found);
  }
  const std::uint16_t reply = packet.opcode == 738U   ? 723U
                              : packet.opcode == 742U ? 727U
                              : packet.opcode == 732U ? 717U
                              : packet.opcode == 734U ? 719U
                                                      : 721U;
  return {.messages = {{.opcode = reply, .body = packet.body}},
          .score_changed = !restoration,
          .finished = !restoration && !bonus && ScoreLimitReached()};
}

OutboundMessage MatchObjectives::BombPacket(domain::PlayerId player,
                                            std::uint32_t state) const {
  protocol::PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(player.Value()));
  writer.WriteU32(state);
  WriteVector(writer, bomb_position_);
  writer.WriteU32(0U);
  return {.opcode = 979U, .body = writer.Release()};
}

MatchObjectiveUpdate MatchObjectives::Bomb(domain::PlayerId player,
                                           const protocol::Packet& packet,
                                           const CombatRelay& combat,
                                           Time now) {
  protocol::PacketReader reader{packet.body};
  // 53F4C0/53F5F0 -> 7E6270: Character+860 is PlayerID, not actor or seat.
  const auto requested_player = domain::PlayerId{reader.ReadU32()};
  const auto state = reader.ReadU32();
  const auto position = Vector3(reader);
  const auto* owner = Participant(player);
  if (requested_player != player ||
      combat.LifeState(player) != CombatLifeState::kAlive || BetweenRounds() ||
      winner_ != 0U) {
    return {.rejection_detail = "bomb rejected: actor_phase_timing_or_region"};
  }
  if (!bomb_map_) return {.rejection_detail = "bomb rejected: no_released_map"};
  if (state != 1U && position != bomb_position_)
    return {.rejection_detail = "bomb rejected: changed_install_point"};
  const auto actor_position = combat.LastPosition(player);
  const auto attack = 1U + round_ % 2U;
  if (state == 1U && bomb_state_ == 0U && owner->team_code == attack &&
      std::ranges::find(bomb_map_->sites, position) != bomb_map_->sites.end() &&
      actor_position && WithinBombRadius(*actor_position, position)) {
    planter_ = player;
    bomb_position_ = position;
    action_started_ = now;
    bomb_state_ = 1U;
  } else if (state == 0U && bomb_state_ == 1U && planter_ == player) {
    bomb_state_ = 0U;
    planter_.reset();
  } else if (state == 2U && bomb_state_ == 1U && planter_ == player &&
             ActiveBombOperator(planter_, combat) &&
             now - action_started_ >=
                 plant_duration_ - std::chrono::milliseconds{100}) {
    bomb_state_ = 2U;
    fuse_ = now + fuse_duration_;
  } else if (state == 4U && bomb_state_ == 2U && owner->team_code != attack &&
             fuse_ && now < *fuse_ && actor_position &&
             WithinBombRadius(*actor_position, bomb_position_)) {
    bomb_state_ = 4U;
    defuser_ = player;
    action_started_ = now;
  } else if (state == 5U && bomb_state_ == 4U && defuser_ == player) {
    bomb_state_ = 2U;
    defuser_.reset();
  } else if (state == 6U && bomb_state_ == 4U && defuser_ == player &&
             ActiveBombOperator(defuser_, combat) &&
             now - action_started_ >=
                 defuse_duration_ - std::chrono::milliseconds{100} &&
             fuse_.has_value() && now < *fuse_) {
    bomb_state_ = 6U;
    fuse_.reset();
    pending_bomb_winner_ = static_cast<std::uint8_t>(3U - attack);
    bomb_result_due_ = now + std::chrono::seconds{2};
  } else {
    return {.rejection_detail = "bomb rejected: actor_phase_timing_or_region"};
  }
  return {.messages = {BombPacket(player, state)}};
}

bool MatchObjectives::ActiveBombOperator(std::optional<domain::PlayerId> player,
                                         const CombatRelay& combat) const {
  if (!player || combat.LifeState(*player) != CombatLifeState::kAlive)
    return false;
  const auto position = combat.LastPosition(*player);
  return position && WithinBombRadius(*position, bomb_position_);
}

void MatchObjectives::ObserveItemDrops(const CombatRelay& combat, Time now) {
  for (const auto& [id, actor] : combat.actors_) {
    if (actor.controller || !actor.item_drop_at || now < *actor.item_drop_at ||
        now >= *actor.item_drop_at + std::chrono::seconds{20}) {
      item_drop_permits_.erase(id);
      continue;
    }
    const auto permit = item_drop_permits_.find(id);
    if (permit == item_drop_permits_.end() ||
        permit->second.life != actor.item_drop_event)
      item_drop_permits_[id] = {
          .life = actor.item_drop_event,
          .expires = *actor.item_drop_at + std::chrono::seconds{20}};
    auto& current = item_drop_permits_.at(id);
    // Ordinary ChrPlayer has no C2S729 producer: 7CC3D0 is called only
    // by infected child850950 and NPC907690. The server must create the
    // ordinary item-mode bubble/respawn reward rather than await that NPC
    // packet.
    if (!current.spawned && actor.life_state != CombatLifeState::kDefeated &&
        actor.position && supplies_.size() < 256U) {
      current.spawned = true;
      Supply reward;
      reward.position = *actor.position;
      reward.position[1] += 18.F;
      reward.type = 0U;  // Native weighted random box, selected at pickup.
      reward.due = now;
      reward.expires = current.expires;
      supplies_.push_back(reward);
    }
  }
}

MatchObjectiveUpdate MatchObjectives::Tick(CombatRelay& combat, Time now,
                                           Time round_deadline) {
  MatchObjectiveUpdate result;
  if (mode_ == 3U) {
    ObserveItemDrops(combat, now);
    for (const auto& [player, held] : held_items_) {
      static_cast<void>(held);
      SynchronizeItemInventory(player, combat, result);
    }
  }
  if (mode_ == 19U) {
    if (winner_ != 0U) return result;
    if (bomb_result_due_) {
      if (now >= *bomb_result_due_) {
        bomb_result_due_.reset();
        CompleteBombRound(pending_bomb_winner_, now, result);
        pending_bomb_winner_ = 0U;
      }
      return result;
    }
    if (next_round_) {
      if (now >= *next_round_) {
        ++round_;
        bomb_state_ = 0U;
        planter_.reset();
        defuser_.reset();
        fuse_.reset();
        next_round_.reset();
        bomb_position_ = {};
        // Loading has no active objects. Arm supplies only after the next 704.
        StartBombRound(Time::max());
        result.restart_round = true;
      }
      return result;
    }
    for (const auto& player : participants_) {
      const auto actor = combat.actors_.find(player.player_id);
      if (actor != combat.actors_.end() &&
          actor->second.life_state == CombatLifeState::kDefeated &&
          actor->second.death_stage == 2U &&
          bomb_spectators_.insert(player.player_id).second) {
        // 7C1610 dispatches 980 to 7E6390: a DWORD GameObjectID switches
        // the eliminated character to spectator state, not a new respawn.
        protocol::PacketWriter writer;
        writer.WriteU32(player.actor_object_id);
        result.messages.push_back({.opcode = 980U, .body = writer.Release()});
      }
    }
  }
  AdvancePirateCoins(combat, now, result);
  AdvanceSurvivalTide(now, result);
  if (mode_ == 23U || mode_ == 25U) ObserveSurvivalDefeats(combat, now, result);
  for (auto& [id, rod] : combat.survival_traps_) {
    const auto owner = combat.actors_.find(rod.owner);
    if (!rod.removed &&
        (owner == combat.actors_.end() ||
         (!rod.linked &&
          (owner->second.life_generation != rod.life ||
           owner->second.life_state != CombatLifeState::kAlive)))) {
      protocol::PacketWriter writer;
      writer.WriteU32(id);
      result.messages.push_back({.opcode = 32774U, .body = writer.Release()});
      rod.removed = true;
      rod.removed_at = now;
    }
  }
  if (survival_loot_pending_) {
    const auto spawns = EncodeSurvivalLoot(survival_loot_);
    result.messages.insert(result.messages.end(), spawns.begin(), spawns.end());
    for (auto& item : survival_loot_)
      item.active_at = now + std::chrono::seconds{1};
    survival_loot_pending_ = false;
  }
  std::erase_if(survival_loot_, [now](const auto& item) {
    return item.expires && now >= *item.expires;
  });
  if (mode_ == 11U) {
    if (target_ > 0U && target_ <= 10'000U &&
        (round_scores_[0] >= target_ || round_scores_[1] >= target_)) {
      winner_ = round_scores_[0] > round_scores_[1] ? 1U : 2U;
      result.finished = true;
    } else if (soccer_due_ && now >= *soccer_due_) {
      if (soccer_phase_ == 1U) {
        // 7E0670 makes every local human issue 704, including living players.
        combat.ResetRound();
        for (auto& [id, actor] : combat.actors_) {
          if (actor.controller) continue;
          actor.life_state = CombatLifeState::kDefeated;
          actor.death_stage = 2U;
          ++actor.life_generation;
        }
        result.messages.push_back({.opcode = 32879U, .body = {}});
        soccer_phase_ = 2U;
        soccer_due_ = now + std::chrono::seconds{4};
      } else {
        result.messages.push_back({.opcode = 32880U, .body = {}});
        soccer_phase_ = 0U;
        soccer_due_.reset();
      }
    }
    return result;
  }
  std::erase_if(supplies_, [&](const Supply& item) {
    if (!item.expires || now < *item.expires) return false;
    if (item.id != 0U) {
      protocol::PacketWriter removed;
      removed.WriteU32(0xFFFFFFFFU);
      removed.WriteU32(item.id);
      result.messages.push_back({.opcode = 712U, .body = removed.Release()});
    }
    return true;
  });
  for (auto& item : supplies_) {
    if (item.id != 0U || now < item.due) {
      continue;
    }
    item.id = ++serial_;
    protocol::PacketWriter writer;
    writer.WriteU32(item.id);
    // 7D1DA0 creates a GoItemBox only when this object-class field is 20000.
    // The pickup effect type is the later field at offset 20.
    writer.WriteU32(20'000U);
    WriteVector(writer, item.position);
    writer.WriteU32(item.type);
    WriteVector(writer, item.velocity);
    writer.WriteU8(item.team);
    result.messages.push_back({.opcode = 711U, .body = writer.Release()});
  }
  std::erase_if(collectibles_, [now](const auto& item) {
    return item.expires && now >= *item.expires;
  });
  for (auto& item : collectibles_) {
    if (item.id != 0U || now < item.due) {
      continue;
    }
    item.id = ++serial_;
    if (item.spawn_opcode == 716U) {
      // Both native star constructors start their 1000 ms activation timer
      // when the object is created, including dropped and regenerated stars.
      item.active_at = now + std::chrono::seconds{1};
    }
    protocol::PacketWriter writer;
    writer.WriteU32(item.id);
    writer.WriteU32(item.spawn_opcode == 718U && item.kind == 2U ? 3U
                                                                 : item.kind);
    WriteVector(writer, item.position);
    WriteVector(writer, item.velocity);
    writer.WriteU32(item.variant);
    result.messages.push_back(
        {.opcode = item.spawn_opcode, .body = writer.Release()});
  }
  std::set<std::uint8_t> original_teams, alive_teams;
  std::size_t survivors = 0U;
  for (const auto& player : participants_) {
    original_teams.insert(player.team_code);
    const auto life = combat.LifeState(player.player_id);
    if (life.has_value() && *life != CombatLifeState::kDefeated) {
      ++survivors;
      alive_teams.insert(player.team_code);
    }
  }
  if (mode_ == 23U) {
    result.finished = participants_.size() > 1U && survivors <= 1U;
    if (!result.finished) ObserveSurvivalSpectators(combat, result);
    return result;
  }
  if (mode_ == 25U) {
    result.finished = original_teams.size() > 1U && alive_teams.size() <= 1U;
    return result;
  }
  if (mode_ == 5U || mode_ == 6U || mode_ == 7U) {
    result.finished = ScoreLimitReached();
    return result;
  }
  if (mode_ != 19U || winner_ != 0U) {
    return result;
  }
  if (bomb_state_ == 1U && !ActiveBombOperator(planter_, combat)) {
    result.messages.push_back(
        BombPacket(planter_.value_or(domain::PlayerId{0xFFFFFFFFU}), 0U));
    bomb_state_ = 0U;
    planter_.reset();
  }
  if (bomb_state_ == 4U && !ActiveBombOperator(defuser_, combat)) {
    result.messages.push_back(
        BombPacket(defuser_.value_or(domain::PlayerId{0xFFFFFFFFU}), 5U));
    bomb_state_ = 2U;
    defuser_.reset();
  }
  const auto attack = static_cast<std::uint8_t>(1U + round_ % 2U);
  const auto defend = static_cast<std::uint8_t>(3U - attack);
  std::uint8_t decided =
      !fuse_.has_value() &&
              (now >= round_deadline ||
               (original_teams.size() == 2U && !alive_teams.contains(attack)))
          ? defend
          : std::uint8_t{0U};
  if (bomb_state_ == 6U) {
    decided = defend;
  } else if (fuse_.has_value() &&
             (now >= *fuse_ ||
              (original_teams.size() == 2U && !alive_teams.contains(defend)))) {
    // 7E6390 -> 541380 uses the same explosion when the last defender falls.
    bomb_state_ = 3U;
    fuse_.reset();
    result.messages.push_back(
        BombPacket(planter_.value_or(domain::PlayerId{0xFFFFFFFFU}), 3U));
    decided = attack;
  } else if (original_teams.size() == 2U && !alive_teams.contains(defend)) {
    decided = attack;
  }
  if (decided != 0U) {
    fuse_.reset();
    if (bomb_state_ == 3U || bomb_state_ == 6U) {
      // Native 540110 starts a 2000 ms timer; 53E720 plays the explosion
      // after 1000 ms, then reports 2014. An immediate 705 cuts this short.
      pending_bomb_winner_ = decided;
      bomb_result_due_ = now + std::chrono::seconds{2};
    } else {
      CompleteBombRound(decided, now, result);
    }
  }
  return result;
}

void MatchObjectives::CompleteBombRound(std::uint8_t team, Time now,
                                        MatchObjectiveUpdate& result) {
  ++round_scores_[team - 1U];
  result.score_changed = true;
  // build752-bomb-bo5-first3-v1: user-requested first3 BO5 policy and
  // original Feature21/7AF920 maximum index4.513's cached Pang target is
  // preserved by the room/UI but never treated as required bomb wins.
  constexpr std::uint32_t winning_rounds = 3U;
  constexpr std::uint32_t maximum_round_index = 4U;
  if (round_scores_[team - 1U] >= winning_rounds ||
      round_ >= maximum_round_index) {
    winner_ = team;
    result.finished = true;
  } else {
    next_round_ = now + std::chrono::seconds{2};
    result.messages.push_back(
        {.opcode = 705U, .body = protocol::EncodeMatchStopResponse(team)});
  }
}

std::optional<Time> MatchObjectives::Deadline() const {
  // Stopped rounds must not keep scheduling an expired fuse or supply timer.
  if (mode_ == 19U) {
    if (winner_ != 0U) return std::nullopt;
    if (bomb_result_due_) return bomb_result_due_;
    if (next_round_) return next_round_;
  }
  std::optional<Time> value;
  const auto include = [&value](Time next) {
    if (!value.has_value() || next < *value) {
      value = next;
    }
  };
  for (const auto& report : pending_pirate_coins_) include(report.retry);
  for (const auto& item : collectibles_) {
    if (item.expires) include(*item.expires);
    if (item.id == 0U) {
      include(item.due);
    }
  }
  for (const auto& item : supplies_) {
    if (item.expires) include(*item.expires);
    if (item.id == 0U) {
      include(item.due);
    }
  }
  if (soccer_due_) include(*soccer_due_);
  if (survival_tide_due_) include(*survival_tide_due_);
  if (fuse_.has_value()) {
    include(*fuse_);
  }
  if (next_round_.has_value()) {
    include(*next_round_);
  }
  return value;
}

bool MatchObjectives::ScoreLimitReached() const {
  if (target_ == 0U || target_ > 10'000U) {
    return false;
  }
  std::array<std::uint64_t, 2U> teams{};
  for (const auto& player : participants_) {
    const auto value = Score(player.player_id);
    if (mode_ != 6U && value >= target_) {
      return true;
    }
    if (mode_ == 6U && (player.team_code == 1U || player.team_code == 2U))
      teams[player.team_code - 1U] += value;
  }
  return mode_ == 6U && (teams[0] >= target_ || teams[1] >= target_);
}

std::uint32_t MatchObjectives::Score(domain::PlayerId player) const noexcept {
  const auto found = scores_.find(player);
  return found == scores_.end() ? 0U : found->second;
}

std::array<std::uint32_t, 2U> MatchObjectives::RoundScores() const noexcept {
  return round_scores_;
}

std::uint8_t MatchObjectives::Winner() const noexcept {
  if (mode_ == 11U)
    return round_scores_[0] == round_scores_[1]  ? 0U
           : round_scores_[0] > round_scores_[1] ? 1U
                                                 : 2U;
  return winner_;
}

std::uint32_t MatchObjectives::RoundIndex() const noexcept { return round_; }

bool MatchObjectives::SmartPingItem(
    std::uint32_t object,
    std::chrono::steady_clock::time_point now) const noexcept {
  if (survival_loot_pending_) return false;
  return std::ranges::any_of(survival_loot_, [=](const auto& item) {
    return item.id == object && (!item.expires || now < *item.expires);
  });
}

bool MatchObjectives::BetweenRounds() const noexcept {
  return next_round_.has_value() || bomb_result_due_.has_value();
}

bool MatchObjectives::FuseActive() const noexcept { return fuse_.has_value(); }
}  // namespace bf::runtime
