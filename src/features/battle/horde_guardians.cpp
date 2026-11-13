#include <algorithm>
#include <cmath>

#include "features/battle/pirate_map_agents.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
using Point = std::array<float, 3U>;

struct Map final {
  std::array<Point, 5U> links;
  std::array<Point, 11U> points;
};

constexpr Map kMaps[]{
#include "features/battle/generated/horde_guardian_maps.inc"
};

[[noreturn]] void Invalid() {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                "invalid horde guardian payload"};
}

Point ReadPoint(protocol::PacketReader& reader) {
  Point result{reader.ReadF32(), reader.ReadF32(), reader.ReadF32()};
  for (const auto value : result)
    if (!std::isfinite(value) || std::abs(value) > 1.0e7F) Invalid();
  return result;
}

bool Nearby(const Point& a, const Point& b, double radius) {
  double squared = 0.0;
  for (std::size_t i = 0U; i < a.size(); ++i) {
    const auto d = static_cast<double>(a[i]) - b[i];
    squared += d * d;
  }
  return squared <= radius * radius;
}

void WritePoint(protocol::PacketWriter& writer, const Point& value) {
  for (const auto coordinate : value) writer.WriteF32(coordinate);
}
}  // namespace

void PirateMapAgents::Configure(std::uint32_t mode, std::string_view map,
                                std::uint32_t difficulty) {
  *this = {};
  if (mode == 101U && difficulty >= 1U && difficulty <= 3U) {
    guardian_map_ = map == "rel_pirate03_prison"      ? 1U
                    : map == "rel_village08_daohouse" ? 2U
                                                      : 0U;
    difficulty_ = difficulty;
  }
}

bool PirateMapAgents::AcceptsLink(std::uint16_t opcode) noexcept {
  return opcode == 740U || opcode == 741U;
}

void PirateMapAgents::ValidateLink(const protocol::Packet& packet) {
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 741U) {
    static_cast<void>(ReadPoint(reader));
    static_cast<void>(ReadPoint(reader));
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() >= 16U) Invalid();
  } else if (packet.opcode == 740U) {
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() != 0U) Invalid();
  } else {
    Invalid();
  }
  reader.RequireEnd();
}

void PirateMapAgents::ObserveGroup(std::uint16_t wave, std::uint8_t group,
                                   Time now) {
  // DB6D90 passes the WORD in 908, not its separate displayed wave DWORD.
  constexpr std::uint8_t groups[3][3]{
      {4U, 16U, 22U}, {6U, 21U, 29U}, {11U, 25U, 40U}};
  if (guardian_map_ == 0U || wave != difficulty_) return;
  const auto& row = groups[difficulty_ - 1U];
  if (std::ranges::find(row, group) == std::end(row) ||
      !groups_.emplace(wave, group).second)
    return;
  // 6C0BC0/6BF720 parse and visit four LinkIDs, including in the five-link
  // house map.
  for (std::size_t key = group == row[2] ? 0U : 2U; key < 4U; ++key)
    if (links_[key].object == 0U) links_[key].permitted = now;
}

CombatRelayResult PirateMapAgents::HandleLink(domain::PlayerId sender,
                                              const protocol::Packet& packet,
                                              CombatRelay& combat, Time now) {
  ValidateLink(packet);
  const auto ignored = [] {
    return CombatRelayResult{
        .accepted = false,
        .reject_reason = CombatRejectReason::kInvalidLifecycleState};
  };
  const auto player = combat.actors_.find(sender);
  if (guardian_map_ == 0U || player == combat.actors_.end() ||
      player->second.controller)
    return ignored();
  auto& actor = player->second;
  const auto reply = [&](std::uint16_t opcode, std::vector<std::uint8_t> body) {
    CombatRelayResult result{.accepted = true};
    for (const auto& [id, member] : combat.actors_)
      if (!member.controller)
        result.deliveries.push_back(
            {.recipient = id, .message = {.opcode = opcode, .body = body}});
    return result;
  };
  protocol::PacketReader reader{packet.body};
  protocol::PacketWriter writer;
  if (packet.opcode == 741U) {
    const auto position = ReadPoint(reader);
    const auto velocity = ReadPoint(reader);
    const auto key = reader.ReadU32();
    const auto seat = reader.ReadU32();
    if (combat.host_ != sender || seat != actor.participant.seat_index ||
        key >= 4U)
      return ignored();
    auto& link = links_[key];
    const auto& expected = kMaps[guardian_map_ - 1U].links[key];
    // Five seconds and one unit are server acceptance tolerances, not native
    // timers.
    if (!link.permitted || now < *link.permitted ||
        now - *link.permitted > std::chrono::seconds{5} || link.object != 0U ||
        !Nearby(position, expected, 1.0) || velocity != Point{} ||
        link_serial_ >= 70'010'000U || !combat.ConsumeActionQuota(actor, now))
      return ignored();
    link.permitted.reset();
    link.object = ++link_serial_;
    link.created = now;
    writer.WriteU32(link.object);
    writer.WriteU32(key);
    WritePoint(writer, expected);
    WritePoint(writer, velocity);
    return reply(724U, writer.Release());
  }
  const auto collector = reader.ReadU32();
  const auto object = reader.ReadU32();
  const auto found = std::ranges::find(links_, object, &Link::object);
  if (object == 0U || found == links_.end() ||
      collector != actor.participant.actor_object_id ||
      actor.life_state != CombatLifeState::kAlive || !actor.position ||
      now < found->created + std::chrono::seconds{1})
    return ignored();
  const auto key = static_cast<std::size_t>(found - links_.begin());
  // Pickup checks the trusted map anchor; client physics and collision remain
  // client-side.
  if (!Nearby(*actor.position, kMaps[guardian_map_ - 1U].links[key], 300.0) ||
      !combat.ConsumeActionQuota(actor, now))
    return ignored();
  found->object = 0U;
  if (!found->guardian_active) found->spawn_permitted = true;
  writer.WriteU32(collector);
  writer.WriteU32(object);
  return reply(725U, writer.Release());
}

bool PirateMapAgents::ApplyGuardian(const protocol::Packet& packet,
                                    std::uint32_t host_object,
                                    CombatRelay* combat, Time now) {
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  const std::uint32_t count = guardian_map_ == 1U ? 4U : 5U;
  if (packet.opcode == 32892U) {
    const auto property = reader.ReadU32();
    static_cast<void>(reader.ReadF64());
    const auto slot = reader.ReadU32();
    const auto position = ReadPoint(reader);
    auto& agent = agents_[slot];
    if (object > count || property != (object == count ? 1U : 0U) ||
        agent.active || agent.spawn == packet.body ||
        (object == count ? angel_spawned_
                         : !links_[object].spawn_permitted ||
                               links_[object].guardian_active))
      return false;
    const auto& positions = kMaps[guardian_map_ - 1U].points;
    if (!Nearby(position, positions[object * 2U], 1.0) &&
        (object == count ||
         !Nearby(position, positions[object * 2U + 1U], 1.0)))
      return false;
    agent = {.active = true,
             .spawn = packet.body,
             .object = std::nullopt,
             .spawner = object,
             .created = now};
    if (object == count)
      angel_spawned_ = true;
    else {
      links_[object].spawn_permitted = false;
      links_[object].guardian_active = true;
    }
    return true;
  }
  if (!combat || !combat->host_ || object <= host_object ||
      object >= host_object + 100'000U || retired_.contains(object))
    return false;
  if (packet.opcode == 32891U) {
    if (std::ranges::any_of(agents_, [&](const auto& a) {
          return a.active && a.object == object;
        }))
      return true;
    const auto unbound = std::ranges::count_if(
        agents_, [](const auto& a) { return a.active && !a.object; });
    if (combat->object_ids_.contains(object) ||
        (!deferred_.contains(object) &&
         deferred_.size() >= static_cast<std::size_t>(unbound)))
      return false;
    deferred_.try_emplace(object);
    return true;
  }
  const auto hint = reader.ReadU32();
  const auto known = std::ranges::find_if(agents_, [object](const auto& a) {
    return a.active && a.object == object;
  });
  const auto index = packet.opcode == 32890U && known != agents_.end()
                         ? static_cast<std::size_t>(known - agents_.begin())
                         : hint;
  if (index >= agents_.size()) return false;
  auto& agent = agents_[index];
  if (!agent.active || (agent.object && agent.object != object)) return false;
  if (packet.opcode == 32890U &&
      (agent.spawner == count ||
       now < agent.created + std::chrono::seconds{20})) {
    const auto owner = combat->object_owners_.find(object);
    if (!agent.object || owner == combat->object_owners_.end() ||
        combat->actors_.at(owner->second).life_state !=
            CombatLifeState::kDefeated)
      return false;
  }
  if (!agent.object) {
    if (combat->object_ids_.contains(object)) return false;
    const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) | object};
    CombatRelay::ActorState state;
    state.participant = {.player_id = synthetic,
                         .actor_object_id = object,
                         .team_code = 2U,
                         .weapon_templates = {},
                         .ordinary_respawn_allowed = false};
    state.controller = combat->host_;
    state.melee_uses_equipped_template = true;
    combat->actors_.emplace(synthetic, std::move(state));
    combat->object_ids_.insert(object);
    combat->object_owners_.emplace(object, synthetic);
    agent.object = object;
  }
  auto& actor = combat->actors_.at(combat->object_owners_.at(object));
  if (packet.opcode == 32893U) {
    actor.position = ReadPoint(reader);
    return true;
  }
  // Removal does not manufacture a death, a cleared enemy or a reward.
  Retire(agent, *combat);
  return true;
}

void PirateMapAgents::Retire(Agent& agent, CombatRelay& combat) {
  const auto count = guardian_map_ == 1U ? 4U : 5U;
  const auto object = *agent.object;
  if (agent.spawner < count) links_[agent.spawner].guardian_active = false;
  agent.active = false;
  retired_.insert(object);
  deferred_.erase(object);
  const auto owner = combat.object_owners_.find(object);
  if (owner != combat.object_owners_.end())
    static_cast<void>(combat.RemoveParticipant(owner->second));
}

std::optional<PirateMapAgents::Time> PirateMapAgents::Deadline() const {
  std::optional<Time> result;
  if (guardian_map_ == 0U) return result;
  const auto count = guardian_map_ == 1U ? 4U : 5U;
  for (const auto& agent : agents_) {
    // Until 32893 binds the object, emitting 32890 would require inventing its
    // ID.
    if (!agent.active || !agent.object || agent.spawner >= count) continue;
    const auto due = agent.created + std::chrono::seconds{20};
    if (!result || due < *result) result = due;
  }
  return result;
}

std::vector<OutboundMessage> PirateMapAgents::Tick(CombatRelay& combat,
                                                   Time now) {
  std::vector<OutboundMessage> messages;
  if (guardian_map_ == 0U) return messages;
  const auto count = guardian_map_ == 1U ? 4U : 5U;
  for (std::size_t slot = 0U; slot < agents_.size(); ++slot) {
    auto& agent = agents_[slot];
    if (!agent.active || !agent.object || agent.spawner >= count ||
        now < agent.created + std::chrono::seconds{20})
      continue;
    protocol::PacketWriter writer;
    writer.WriteU32(*agent.object);
    writer.WriteU32(static_cast<std::uint32_t>(slot));
    messages.push_back({.opcode = 32890U, .body = writer.Release()});
    Retire(agent, combat);
  }
  return messages;
}

std::optional<bool> PirateMapAgents::PrepareAction(
    domain::PlayerId sender, const protocol::Packet& packet,
    CombatRelay& combat, Time now) {
  if (guardian_map_ == 0U) return std::nullopt;
  std::optional<protocol::NativeActionRequest> action;
  std::optional<std::uint32_t> object;
  if (protocol::IsNativeActionOpcode(packet.opcode)) {
    action = protocol::DecodeNativeAction(packet.opcode, packet.body);
    object = action->actor_object_id;
  } else if (protocol::IsNativeLifecycleOpcode(packet.opcode)) {
    object = protocol::DecodeNativeLifecycle(packet.opcode, packet.body)
                 .actor_object_id;
  }
  if (!object) return std::nullopt;
  if (retired_.contains(*object)) return false;
  const auto bound = std::ranges::find_if(
      agents_, [&](const auto& a) { return a.active && a.object == object; });
  if (bound != agents_.end()) {
    const auto count = guardian_map_ == 1U ? 4U : 5U;
    if (combat.host_ != sender ||
        (bound->spawner < count &&
         now >= bound->created + std::chrono::seconds{20}))
      return false;
    if (action && action->weapon_switch) {
      const auto& weapon = *action->weapon_switch;
      if (bound->spawner == count || weapon.template_id != 9022U) return false;
      combat.actors_.at(combat.object_owners_.at(*object))
          .participant.weapon_templates[weapon.bar_index] = 9022U;
    }
    return true;
  }
  const bool guardian_weapon = action && action->weapon_switch &&
                               action->weapon_switch->template_id == 9022U;
  if (!deferred_.contains(*object) && !guardian_weapon) return std::nullopt;
  if (combat.host_ != sender || combat.object_ids_.contains(*object))
    return false;
  const auto host = combat.actors_.find(sender);
  if (host == combat.actors_.end()) return false;
  const auto base = host->second.participant.actor_object_id;
  const auto unbound = std::ranges::count_if(
      agents_, [](const auto& a) { return a.active && !a.object; });
  if (*object <= base || *object >= base + 100'000U ||
      packet.body.size() > 4096U ||
      (!deferred_.contains(*object) &&
       deferred_.size() >= static_cast<std::size_t>(unbound)))
    return false;
  auto& queue = deferred_[*object];
  if (queue.size() < 64U) queue.push_back(packet);
  return false;
}

std::vector<protocol::Packet> PirateMapAgents::TakeDeferredActions(
    std::uint32_t object) {
  if (std::ranges::none_of(agents_, [&](const auto& a) {
        return a.active && a.object == object;
      }))
    return {};
  auto node = deferred_.extract(object);
  return node.empty() ? std::vector<protocol::Packet>{}
                      : std::move(node.mapped());
}
}  // namespace bf::runtime
