#include "features/practice/horde_tutor_match.h"

#include <algorithm>
#include <stdexcept>

#include "features/battle/pirate_match.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
CombatRelayResult Ignored() {
  return {.accepted = false,
          .reject_reason = CombatRejectReason::kInvalidLifecycleState,
          .deliveries = {}};
}

std::map<std::u16string, std::u16string> Document(std::u16string text) {
  if (!text.empty() && text.front() == static_cast<char16_t>(0xFEFFU))
    text.erase(0U, 1U);
  if (const auto zero = text.find(u'\0'); zero != std::u16string::npos)
    text.resize(zero);
  std::map<std::u16string, std::u16string> fields;
  std::size_t offset = 0U;
  while (offset < text.size()) {
    const auto end = text.find(u'\n', offset);
    auto line =
        text.substr(offset, end == std::u16string::npos ? end : end - offset);
    if (!line.empty() && line.back() == u'\r') line.pop_back();
    const auto equal = line.find(u'=');
    if (!line.empty())
      fields.emplace(line.substr(0U, equal), line.substr(equal + 1U));
    if (end == std::u16string::npos) break;
    offset = end + 1U;
  }
  return fields;  // The shared wire validator already rejected
                  // duplicates/missing fields.
}

std::u16string Decimal(std::uint32_t value) {
  const auto ascii = std::to_string(value);
  return {ascii.begin(), ascii.end()};
}
}  // namespace

HordeTutorMatch::HordeTutorMatch(domain::PlayerId player, std::uint32_t object)
    : player_(player), player_object_(object) {
  if (!player.IsValid() || object != 1'000'000U)
    throw std::invalid_argument{
        "HordeTutor requires the sole local character at seat zero"};
}

bool HordeTutorMatch::Accepts(std::uint16_t opcode) noexcept {
  return PirateMatch::Accepts(opcode);
}

void HordeTutorMatch::Validate(const protocol::Packet& packet) {
  PirateMatch::Validate(packet);
}

std::uint8_t HordeTutorMatch::Winner() const noexcept { return winner_; }

bool HordeTutorMatch::EnemiesTerminal(const CombatRelay& combat) const {
  return std::ranges::all_of(actors_, [&](const auto& entry) {
    if (HordeTutorSpawns()[entry.second - 1U].friendly) return true;
    const auto owner = combat.object_owners_.find(entry.first);
    return owner != combat.object_owners_.end() &&
           combat.actors_.at(owner->second).life_state ==
               CombatLifeState::kDefeated;
  });
}

bool HordeTutorMatch::ClearedActors(const CombatRelay& combat) const {
  if (!pending_.empty() || spawned_.size() != HordeTutorSpawns().size() ||
      actors_.size() != HordeTutorSpawns().size())
    return false;
  const auto player = combat.actors_.find(player_);
  if (player == combat.actors_.end()) return false;
  return std::ranges::all_of(actors_, [&](const auto& entry) {
    const auto& row = HordeTutorSpawns()[entry.second - 1U];
    const auto owner = combat.object_owners_.find(entry.first);
    if (owner == combat.object_owners_.end()) return false;
    const auto& actor = combat.actors_.at(owner->second);
    // DBF410 examines only mobs on a different team. A friendly NPC must be
    // registered but need not die; a bubble is not the terminal400..499 state.
    return row.friendly ? actor.participant.team_code ==
                              player->second.participant.team_code
                        : actor.participant.team_code !=
                                  player->second.participant.team_code &&
                              actor.life_state == CombatLifeState::kDefeated;
  });
}

std::optional<HordeTutorMatch::Time> HordeTutorMatch::Deadline(
    const CombatRelay& combat) const {
  if (!clear_requested_ || !clear_ready_at_ || cleared_ || winner_ ||
      !ClearedActors(combat))
    return std::nullopt;
  return std::max(started_ + std::chrono::seconds{5}, *clear_ready_at_) +
         std::chrono::seconds{3};
}

std::optional<OutboundMessage> HordeTutorMatch::Advance(
    const CombatRelay& combat, Time now) {
  if (!cleared_ && !clear_ready_at_ && ClearedActors(combat))
    clear_ready_at_ = now;
  const auto due = Deadline(combat);
  if (!due || now < *due) return std::nullopt;
  clear_requested_ = false;
  cleared_ = true;
  clear_ack_at_ = now;
  protocol::PacketWriter writer;
  writer.WriteU32(1U);
  // DC8270's extra3s is before C2S904, in the client. Do not add it twice here.
  return OutboundMessage{905U, writer.Release()};
}

bool HordeTutorMatch::PrepareAction(domain::PlayerId sender,
                                    const protocol::Packet& packet,
                                    CombatRelay& combat) {
  if (!protocol::IsNativeActionOpcode(packet.opcode)) return true;
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto object = request.actor_object_id;
  if (sender != player_ || combat.mode_id_ != kHordeTutorMode ||
      combat.host_ != player_ || winner_)
    return false;
  if (object == player_object_) return true;
  if (!begun_ || cleared_ || object <= player_object_ ||
      object >= player_object_ + 100'000U)
    return false;
  if (packet.opcode == 32810U && !actors_.contains(object)) {
    if (pending_.empty() || combat.object_ids_.contains(object)) return false;
    const auto group = pending_.front();
    const auto& row = HordeTutorSpawns()[group - 1U];
    const auto& change = *request.weapon_switch;
    // WaveTestInit overrides enemy slots2202/7105; all four support NPC Lua
    // classes have2001/7001, and their commented overrides are not active.
    const auto primary = row.friendly ? 2001U : 2202U;
    const auto melee = row.friendly ? 7001U : 7105U;
    if (!((change.bar_index == 0U && change.template_id == primary) ||
          (change.bar_index == 2U && change.template_id == melee)))
      return false;
    const auto player = combat.actors_.find(player_);
    if (player == combat.actors_.end() ||
        player->second.participant.team_code != 2U)
      return false;
    const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) | object};
    CombatRelay::ActorState state;
    state.participant = {.player_id = synthetic,
                         .actor_object_id = object,
                         .team_code = row.friendly
                                          ? player->second.participant.team_code
                                          : std::uint8_t{1U},
                         .weapon_templates = {primary, std::nullopt, melee},
                         .ordinary_respawn_allowed = false};
    state.controller = player_;
    state.position = HordeTutorSpawnPosition(group);
    // Unknown original tutor rewards remain zero. Neither allies nor enemies
    // get the ordinary101 coin/drop capability; NPC kills never credit the
    // host.
    state.pirate_coin_source = false;
    state.melee_uses_equipped_template = true;
    combat.actors_.emplace(synthetic, std::move(state));
    combat.object_ids_.insert(object);
    combat.object_owners_.emplace(object, synthetic);
    actors_.emplace(object, group);
    pending_.pop_front();
  }
  if (!actors_.contains(object)) return false;
  if (packet.opcode == 32810U) {
    const auto& change = *request.weapon_switch;
    const auto& actor = combat.actors_.at(combat.object_owners_.at(object));
    return actor.participant.weapon_templates[change.bar_index] ==
           change.template_id;
  }
  return true;
}

CombatRelayResult HordeTutorMatch::Handle(domain::PlayerId sender,
                                          const protocol::Packet& packet,
                                          CombatRelay& combat, Time now) {
  Validate(packet);
  const auto player = combat.actors_.find(player_);
  if (sender != player_ || winner_ || combat.mode_id_ != kHordeTutorMode ||
      combat.host_ != player_ || !combat.observers_.empty() ||
      std::ranges::count_if(combat.actors_,
                            [](const auto& entry) {
                              return !entry.second.controller.has_value();
                            }) != 1 ||
      player == combat.actors_.end() ||
      player->second.participant.actor_object_id != player_object_)
    return Ignored();
  auto reply = [&](std::uint16_t opcode,
                   const std::vector<std::uint8_t>& body) {
    return CombatRelayResult{.accepted = true,
                             .reject_reason = std::nullopt,
                             .deliveries = {{player_, {opcode, body}}}};
  };
  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 903U) {
    if (reader.ReadU32() != 1U || begun_) return Ignored();
    begun_ = true;
    started_ = now;
    return reply(904U, packet.body);
  }
  if (!begun_) return Ignored();
  if (packet.opcode == 908U) {
    if (cleared_ || reader.ReadU32() != 1U || reader.ReadU16() != 1U ||
        reader.ReadU32() != 19U || reader.ReadU32() != 19U)
      return Ignored();
    if (!plan_.empty())
      return plan_ == packet.body ? reply(909U, plan_) : Ignored();
    std::set<std::uint8_t> groups;
    for (std::size_t i = 0U; i < HordeTutorSpawns().size(); ++i) {
      const auto start = reader.ReadU32();
      const auto macro = reader.ReadUtf16(256U);
      const auto point = reader.ReadUtf16(4096U);
      const auto only_spawn_replay = reader.ReadU32();
      const auto group = reader.ReadU8();
      const auto index = reader.ReadU8();
      if (group == 0U || group > 19U || index != 1U ||
          !groups.insert(group).second)
        return Ignored();
      const auto& row = HordeTutorSpawns()[group - 1U];
      if (start != row.start_ms || macro != row.macro ||
          point != row.spawn_point || only_spawn_replay)
        return Ignored();
    }
    // Validate the entire immutable plan before committing any authorization.
    plan_ = packet.body;
    return reply(909U, plan_);
  }
  if (packet.opcode == 904U) {
    if (reader.ReadU32() != 1U || cleared_ || plan_.empty()) return Ignored();
    clear_requested_ = true;
    if (const auto cleared = Advance(combat, now))
      return reply(cleared->opcode, cleared->body);
    return {.accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
  }
  if (packet.opcode == 905U) {
    if (!cleared_ || !clear_ack_at_ ||
        now < *clear_ack_at_ + std::chrono::seconds{3} ||
        player->second.life_state != CombatLifeState::kAlive)
      return Ignored();
    winner_ = 2U;
    return {.accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
  }
  if (packet.opcode == 906U)
    // Shipped GameOption piratesCharDeadGameOver=false. A defeated local
    // player may respawn; even a correctly sized906 is not defeat authority.
    return Ignored();
  if (packet.opcode == 907U)
    return Ignored();  // No second active wave; commented Lua is not a spawn
                       // capability.
  if (packet.opcode == 32841U) {
    if (plan_.empty() || cleared_) return Ignored();
    const auto fields = Document(reader.ReadUtf16(4096U));
    const auto rows = HordeTutorSpawns();
    const auto row = std::ranges::find_if(rows, [&](const auto& expected) {
      return fields.at(u"grpIdx") == Decimal(expected.group) &&
             fields.at(u"grpMobIdx") == u"1" &&
             fields.at(u"mobType") == expected.mob_type &&
             fields.at(u"onlySPR") == u"0" &&
             fields.at(u"repGrpIdx") == Decimal(expected.replay_group) &&
             fields.at(u"spawnPoint") == expected.spawn_point;
    });
    if (row == rows.end() || !pending_.empty() ||
        std::ranges::any_of(rows,
                            [&](const auto& earlier) {
                              return earlier.start_ms < row->start_ms &&
                                     !spawned_.contains(earlier.group);
                            }) ||
        (!EnemiesTerminal(combat) &&
         now < started_ + std::chrono::milliseconds{row->start_ms + 500U}) ||
        !spawned_.insert(row->group).second)
      return Ignored();
    // DB6D90 may accelerate the earliest pending group when existing enemies
    // are terminal. Otherwise it waits startTime+500ms; only one construction
    // may be in flight. A rigid startTime deadline would reject real client
    // Lua.
    pending_.push_back(row->group);
    return combat.AcceptedResult(sender, packet);
  }
  if (packet.opcode == 32865U || packet.opcode == 32866U) {
    if (cleared_ || !actors_.contains(reader.ReadU32())) return Ignored();
    return combat.AcceptedResult(sender, packet);
  }
  if (packet.opcode == 32898U) {
    const auto object = reader.ReadU32();
    const auto weapon = reader.ReadU16();
    static_cast<void>(reader.ReadI8());
    for (unsigned i = 0U; i < 6U; ++i) static_cast<void>(reader.ReadF32());
    const auto target = reader.ReadU32();
    static_cast<void>(reader.ReadU8());
    const auto cookie = reader.ReadU32();
    if (cleared_ || !actors_.contains(object) ||
        (target != 0U && target != 0xFFFFFFFFU &&
         !combat.object_ids_.contains(target)))
      return Ignored();
    auto& actor = combat.actors_.at(combat.object_owners_.at(object));
    const auto bar = std::ranges::find(actor.participant.weapon_templates,
                                       std::optional<std::uint32_t>{weapon});
    if (actor.life_state != CombatLifeState::kAlive ||
        bar == actor.participant.weapon_templates.end() ||
        !combat.ConsumeActionQuota(actor, now))
      return Ignored();
    combat.PruneProjectiles(now);
    if (combat.projectiles_.contains(cookie) ||
        combat.projectiles_.size() >= 8192U)
      return Ignored();
    auto& projectile = combat.projectiles_.try_emplace(cookie).first->second;
    projectile.owner_object_id = object;
    projectile.created_at = now;
    projectile.weapon_bar = static_cast<std::uint8_t>(
        bar - actor.participant.weapon_templates.begin());
    ++actor.accepted_shots;
    return combat.AcceptedResult(sender, packet);
  }
  return Ignored();  // No extra script capabilities are inferred from
                     // authenticated host status.
}
}  // namespace bf::runtime
