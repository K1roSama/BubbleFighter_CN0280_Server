/** @file crazy_racing_match.cpp @brief
 * 原BFRunner合法移动和终点申报；本服兼容2秒终局调度。 */
#include "features/battle/crazy_racing_match.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
#include "features/battle/generated/crazy_racing_maps.inc"

void Require(bool condition) {
  if (!condition)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "invalid BFRunner report"};
}

bool Contains(const CrazyRacingRegion& r, const std::array<float, 3>& p) {
  for (std::size_t i = 0; i < 3; ++i)
    if (!std::isfinite(p[i]) || p[i] < r.bounds[i] || p[i] > r.bounds[i + 3])
      return false;
  return true;
}
}  // namespace

const CrazyRacingMap* GetCrazyRacingMap(std::string_view key) noexcept {
  const auto i = std::ranges::find(kCrazyRacingMaps, key, &CrazyRacingMap::key);
  return i == std::end(kCrazyRacingMaps) ? nullptr : &*i;
}

const CrazyRacingRegion* GetCrazyRacingRegion(std::string_view key,
                                              std::uint8_t kind,
                                              std::uint16_t index) noexcept {
  const auto i = std::ranges::find_if(kCrazyRacingRegions, [&](const auto& r) {
    return r.map == key && r.kind == kind && r.index == index;
  });
  return i == std::end(kCrazyRacingRegions) ? nullptr : &*i;
}

void NormalizeCrazyRacingConfiguration(protocol::SceneConfiguration& c) {
  if (c.mode_id != 21U) return;
  const auto* m = GetCrazyRacingMap(c.family_key);
  if (!m) m = GetCrazyRacingMap(c.map_key);
  if (!m) m = &kCrazyRacingMaps[0];
  c.map_key = m->key;
  c.family_key = m->key;
  for (auto& r : c.rotating_maps) {
    const auto* rotating = GetCrazyRacingMap(r.family_key);
    if (!rotating) rotating = GetCrazyRacingMap(r.map_key);
    if (!rotating) rotating = m;
    r.map_key = rotating->key;
    r.family_key = rotating->key;
  }
}

CrazyRacingMatch::CrazyRacingMatch(std::string_view key,
                                   std::vector<CombatParticipant> players,
                                   std::uint32_t random_seed)
    : map_(GetCrazyRacingMap(key)), random_(random_seed) {
  if (!map_)
    throw std::invalid_argument{"BFRunner requires an original released track"};
  if (players.size() < 4U || players.size() > 12U)
    throw std::invalid_argument{"BFRunner requires original4..12 player count"};
  for (const auto& p : players)
    runners_.emplace(p.player_id, Runner{.participant = p});
}

void CrazyRacingMatch::Validate(const protocol::Packet& p) {
  protocol::PacketReader r{p.body};
  if (p.opcode == 756U) {
    const auto team = r.ReadU8();
    Require(team == 1U || team == 2U);
  } else if (p.opcode == 757U) {
    Require(r.ReadU32() <= 0xFFFFU);
  } else if (p.opcode == 759U) {
    Require(r.ReadU32() == 0xFFFFFFFFU);
    const auto type = r.ReadU32();
    const auto region = r.ReadU32();
    Require((type == 0U && region <= 0xFFFFU) ||
            (type == 101U && region == 0xFFFFFFFFU));
  } else if (p.opcode == 708U) {
    // 7C4030: use the single currently held slot, no actor/type supplied by
    // client.
  } else if (p.opcode == 32894U) {
    static_cast<void>(r.ReadU32());
    static_cast<void>(r.ReadU32());
  } else if (p.opcode == 758U) {
    const auto count = r.ReadU32();
    Require(count <= 12U);
    std::array<bool, 16> seen{};
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto seat = r.ReadU32();
      const auto metric = r.ReadI32();
      Require(seat < 16U && !seen[seat] && metric >= 0);
      seen[seat] = true;
    }
  } else if (p.opcode == 2026U) {
    Require(r.ReadU8() == 32U);
    const auto team = r.ReadU32();
    Require(team == 1U || team == 2U);
    Require(r.ReadU8() == 0U);
  } else
    Require(false);
  r.RequireEnd();
}

void CrazyRacingMatch::Observe(domain::PlayerId p,
                               const protocol::Packet& packet,
                               const CombatRelay& combat, Time now) {
  const auto i = runners_.find(p);
  if (i == runners_.end()) return;
  if (combat.LifeState(p) == CombatLifeState::kDefeated) {
    i->second.held = 0U;
    i->second.effects.clear();
    i->second.goal_entered.reset();
  }
  if (!protocol::IsNativeActionOpcode(packet.opcode)) return;
  const auto action = protocol::DecodeNativeAction(packet.opcode, packet.body);
  if (action.position) ObservePosition(p, *action.position, combat, now);
}

void CrazyRacingMatch::ObservePosition(domain::PlayerId p,
                                       const std::array<float, 3>& point,
                                       const CombatRelay& combat, Time now) {
  const auto i = runners_.find(p);
  if (i != runners_.end() && combat.LifeGeneration(p) &&
      std::ranges::all_of(point,
                          [](float value) { return std::isfinite(value); })) {
    if (i->second.life != *combat.LifeGeneration(p))
      i->second.goal_entered.reset();
    i->second.moved = now;
    i->second.life = *combat.LifeGeneration(p);
    if (point[0] >= map_->goal_min[0] && point[0] <= map_->goal_max[0] &&
        point[2] >= map_->goal_min[2] && point[2] <= map_->goal_max[2]) {
      if (!i->second.goal_entered) i->second.goal_entered = now;
    } else
      i->second.goal_entered.reset();
  }
}

bool CrazyRacingMatch::Checkpoint(domain::PlayerId p, std::uint16_t index,
                                  const std::array<float, 3>& position,
                                  const CombatRelay& combat, Time now) {
  const auto i = runners_.find(p);
  if (i == runners_.end() || goal_ ||
      combat.LifeState(p) != CombatLifeState::kAlive)
    return false;
  const auto observed = combat.LastPosition(p);
  if (!observed || !i->second.moved || now < *i->second.moved ||
      now - *i->second.moved > std::chrono::seconds{2} ||
      combat.LifeGeneration(p) != i->second.life)
    return false;
  for (std::size_t axis = 0; axis < 3; ++axis)
    if (std::abs(position[axis] - (*observed)[axis]) > 0.01F) return false;
  const auto region =
      std::ranges::find_if(kCrazyRacingRegions, [&](const auto& x) {
        return x.map == map_->key && x.kind == 4U && x.index == index &&
               Contains(x, position);
      });
  if (region == std::end(kCrazyRacingRegions) ||
      (i->second.checkpoint && index <= *i->second.checkpoint))
    return false;
  i->second.checkpoint = index;
  return true;
}

std::vector<OutboundMessage> CrazyRacingMatch::Interact(
    domain::PlayerId player, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  Validate(packet);
  const auto found = runners_.find(player);
  if (found == runners_.end() || goal_ ||
      combat.LifeState(player) != CombatLifeState::kAlive)
    return {};
  auto& runner = found->second;
  const auto life = combat.LifeGeneration(player);
  const auto point = combat.LastPosition(player);
  protocol::PacketReader input{packet.body};
  if (packet.opcode == 32894U) {
    Require(input.ReadU32() == runner.participant.actor_object_id);
    const auto type = input.ReadU32();
    std::erase_if(runner.effects, [&](const auto& e) {
      return e.second.first != life || now >= e.second.second;
    });
    if (!runner.effects.contains(type)) return {};
    runner.effects.erase(type);
    return {{32894U, packet.body}};
  }
  if (packet.opcode == 708U) {
    if (runner.held == 0U || now < runner.usable || !life) return {};
    const auto type = runner.held;
    runner.held = 0U;
    runner.usable = now + std::chrono::milliseconds{100};
    runner.effects[type] = {*life, now + std::chrono::seconds{10}};
    combat.AuthorizeItemEffects(player, type, 21U, now);
    protocol::PacketWriter w;
    w.WriteU32(runner.participant.seat_index);
    w.WriteU32(type);
    w.WriteU32(0U);
    return {{714U, w.Release()}};
  }
  if (!point || !runner.moved || now < *runner.moved ||
      now - *runner.moved > std::chrono::seconds{2} || life != runner.life)
    return {};
  if (packet.opcode == 757U) {
    const auto index = static_cast<std::uint16_t>(input.ReadU32());
    if (!Checkpoint(player, index, *point, combat, now)) return {};
    protocol::PacketWriter w;
    w.WriteU32(runner.participant.seat_index);
    w.WriteU32(index);
    return {{772U, w.Release()}};
  }
  if (packet.opcode != 759U) return {};
  static_cast<void>(input.ReadU32());
  const auto requested = input.ReadU32();
  const auto index = input.ReadU32();
  const auto kind = static_cast<std::uint8_t>(requested == 101U ? 2U : 3U);
  const auto region =
      std::ranges::find_if(kCrazyRacingRegions, [&](const auto& r) {
        return r.map == map_->key && r.kind == kind &&
               (kind == 2U || r.index == index) && Contains(r, *point);
      });
  if (region == std::end(kCrazyRacingRegions)) return {};
  if (kind == 2U) {
    if (runner.boost &&
        (now < *runner.boost || now - *runner.boost < std::chrono::seconds{1}))
      return {};
    runner.boost = now;
    runner.effects[101U] = {*life, now + std::chrono::seconds{10}};
    combat.AuthorizeItemEffects(player, 101U, 21U, now);
    protocol::PacketWriter w;
    w.WriteU32(runner.participant.seat_index);
    w.WriteU32(101U);
    w.WriteU32(0U);
    return {{715U, w.Release()}};
  }
  if (runner.held != 0U || runner.box == region->index ||
      (runner.pickup && (now < *runner.pickup ||
                         now - *runner.pickup < std::chrono::seconds{1})))
    return {};
  // Native BFRunner enabled IDs only. Uniform selection is an explicit server
  // compatibility policy; original private weights/selection are not recovered.
  constexpr std::array<std::uint32_t, 7> pool{101U, 102U, 146U, 130U,
                                              147U, 128U, 234U};
  std::uniform_int_distribution<std::size_t> choose{0U, pool.size() - 1U};
  runner.held = pool[choose(random_)];
  runner.box = region->index;
  runner.pickup = now;
  runner.usable =
      now + std::chrono::seconds{1};  // 84CAB0 native pickup cooldown.
  protocol::PacketWriter w;
  w.WriteU32(runner.participant.seat_index);
  w.WriteU32(runner.held);
  w.WriteU32(0U);
  return {{713U, w.Release()}};
}

bool CrazyRacingMatch::Handle(domain::PlayerId p, domain::PlayerId host,
                              const protocol::Packet& packet,
                              const CombatRelay& combat, Time now) {
  Validate(packet);
  if (p != host) return false;
  if (packet.opcode == 758U) {
    // The host's 10000*region+distance samples are not authenticated character
    // movement or original server scoring. Accept only registered roster rows;
    // preserve the independently validated757 route, and mint nothing.
    protocol::PacketReader r{packet.body};
    const auto n = r.ReadU32();
    for (std::uint32_t j = 0; j < n; ++j) {
      const auto seat = r.ReadU32();
      static_cast<void>(r.ReadI32());
      if (std::ranges::none_of(runners_, [&](const auto& x) {
            return x.second.participant.seat_index == seat;
          }))
        return false;
    }
    return true;
  }
  const auto team = packet.opcode == 756U
                        ? packet.body[0]
                        : static_cast<std::uint8_t>(packet.body[1]);
  if (goal_)
    return team == goal_->team && now >= goal_->due && packet.opcode == 2026U;
  if (packet.opcode != 756U) return false;
  const Runner* first = nullptr;
  domain::PlayerId finisher;
  for (const auto& [id, runner] : runners_) {
    const auto position = combat.LastPosition(id);
    const auto baton = runner.participant.weapon_templates[2];
    if (!runner.goal_entered || !baton ||
        *baton != (runner.participant.team_code == 1U ? 7058U : 7057U) ||
        combat.LifeState(id) != CombatLifeState::kAlive || !position ||
        !runner.moved || now < *runner.moved ||
        now - *runner.moved > std::chrono::seconds{2} ||
        combat.LifeGeneration(id) != runner.life)
      continue;
    // 7CBB80 deliberately checks X/Z; jumping height is not a goal exclusion.
    if ((*position)[0] >= map_->goal_min[0] &&
        (*position)[0] <= map_->goal_max[0] &&
        (*position)[2] >= map_->goal_min[2] &&
        (*position)[2] <= map_->goal_max[2]) {
      if (!first || *runner.goal_entered < *first->goal_entered ||
          (*runner.goal_entered == *first->goal_entered &&
           runner.participant.seat_index < first->participant.seat_index)) {
        first = &runner;
        finisher = id;
      }
    }
  }
  if (!first || first->participant.team_code != team) return false;
  goal_ = Goal{finisher, team, now + std::chrono::seconds{2}};
  return true;
}

bool CrazyRacingMatch::Finished(Time now) const noexcept {
  return goal_ && now >= goal_->due;
}

std::optional<CrazyRacingMatch::Time> CrazyRacingMatch::Deadline()
    const noexcept {
  return goal_ ? std::optional{goal_->due} : std::nullopt;
}

std::uint8_t CrazyRacingMatch::Winner() const noexcept {
  return goal_ ? goal_->team : 0U;
}

std::uint32_t CrazyRacingMatch::Goals(domain::PlayerId p) const noexcept {
  return goal_ && goal_->runner == p ? 1U : 0U;
}

std::optional<CrazyRacingSpawn> CrazyRacingMatch::Spawn(
    domain::PlayerId p) const noexcept {
  const auto found = runners_.find(p);
  if (found == runners_.end()) return std::nullopt;
  const auto& own = found->second.participant;
  if (own.team_code != 1U && own.team_code != 2U) return std::nullopt;
  std::uint32_t index = 0U;
  for (const auto& [id, r] : runners_) {
    static_cast<void>(id);
    if (r.participant.team_code == own.team_code &&
        r.participant.seat_index < own.seat_index)
      ++index;
  }
  index %= 6U;
  const auto point = index + (own.team_code == 1U ? 11U : 21U);
  return CrazyRacingSpawn{
      point, map_->spawns[index + (own.team_code == 1U ? 0U : 6U)]};
}
}  // namespace bf::runtime
