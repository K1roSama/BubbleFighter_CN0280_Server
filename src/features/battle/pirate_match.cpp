#include "features/battle/pirate_match.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
[[noreturn]] void Invalid() {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                "invalid pirate payload"};
}

void Finite(float value) {
  if (!std::isfinite(value) || std::abs(value) > 1.0e7F) {
    Invalid();
  }
}

void Boolean(std::uint32_t value) {
  if (value > 1U) {
    Invalid();
  }
}

std::map<std::u16string, std::u16string> Document(std::u16string text) {
  if (!text.empty() && text.front() == u'\uFEFF') {
    text.erase(0U, 1U);
  }
  if (const auto zero = text.find(u'\0'); zero != std::u16string::npos) {
    text.resize(zero);
  }
  std::map<std::u16string, std::u16string> fields;
  std::size_t start = 0U;
  while (start < text.size()) {
    auto end = text.find(u'\n', start);
    if (end == std::u16string::npos) {
      end = text.size();
    }
    auto line = text.substr(start, end - start);
    if (!line.empty() && line.back() == u'\r') {
      line.pop_back();
    }
    start = end + 1U;
    if (line.empty()) {
      continue;
    }
    const auto equal = line.find(u'=');
    if (equal == std::u16string::npos ||
        !fields.emplace(line.substr(0U, equal), line.substr(equal + 1U))
             .second) {
      Invalid();
    }
  }
  return fields;
}

std::uint8_t IndexValue(const std::u16string& text) {
  if (text.empty()) {
    Invalid();
  }
  std::uint32_t value = 0U;
  for (const auto unit : text) {
    if (unit < u'0' || unit > u'9') {
      Invalid();
    }
    value = value * 10U + static_cast<std::uint32_t>(unit - u'0');
    if (value > 255U) {
      Invalid();
    }
  }
  return static_cast<std::uint8_t>(value);
}

CombatRelayResult Ignored() {
  return {.accepted = false,
          .reject_reason = CombatRejectReason::kInvalidLifecycleState,
          .deliveries = {}};
}

bool MatchesSpawnType(std::u16string_view planned,
                      std::u16string_view spawned) {
  if (planned == spawned) return true;
  // DB6D90 expands ConsoleMacro.ini before sending 32841: the 908 plan
  // contains a macro (.Dao), while mobType names its Lua class (ProtoSample).
  // Keep a verified mapping; arbitrary types must not consume a planned spawn.
  constexpr std::array<std::pair<std::u16string_view, std::u16string_view>, 7U>
      aliases{{{u".Dao", u"ProtoSample"},
               {u".Uni", u"Uni"},
               {u".Kephi", u"Kephi"},
               {u".DaoOI", u"DaoOneItem"},
               {u".UniOI", u"UniOneItem"},
               {u".KephiOI", u"KephiOneItem"},
               {u".Bazzi", u"Bazzi"}}};
  return std::ranges::any_of(aliases, [&](const auto& alias) {
    return planned == alias.first && spawned == alias.second;
  });
}
}  // namespace

PirateMatch::PirateMatch(domain::PlayerId host, std::uint32_t host_object)
    : host_(host), host_object_(host_object) {}

std::uint8_t PirateMatch::Winner() const noexcept { return winner_; }

std::pair<std::uint16_t, std::uint8_t> PirateMatch::LastSpawnGroup()
    const noexcept {
  return {script_wave_, last_group_};
}

bool PirateMatch::ClearedActors(const CombatRelay& combat) const {
  if (!pending_.empty() || spawned_.size() != spawns_.size()) {
    return false;
  }
  return std::ranges::all_of(actors_, [&](const auto& entry) {
    return entry.second != wave_ ||
           combat.actors_.at(combat.object_owners_.at(entry.first))
                   .life_state == CombatLifeState::kDefeated;
  });
}

std::optional<PirateMatch::Time> PirateMatch::Deadline(
    const CombatRelay& combat) const {
  if (!clear_requested_ || winner_ != 0U || !ClearedActors(combat)) {
    return std::nullopt;
  }
  return started_ + std::chrono::seconds{5};
}

std::optional<OutboundMessage> PirateMatch::Advance(const CombatRelay& combat,
                                                    Time now) {
  const auto due = Deadline(combat);
  if (!due.has_value() || now < *due) {
    return std::nullopt;
  }
  clear_requested_ = false;
  cleared_ = wave_;
  protocol::PacketWriter writer;
  writer.WriteU32(cleared_);
  return OutboundMessage{.opcode = 905U, .body = writer.Release()};
}

bool PirateMatch::Accepts(std::uint16_t opcode) noexcept {
  return (opcode >= 903U && opcode <= 908U) || opcode == 32841U ||
         opcode == 32857U || opcode == 32865U || opcode == 32866U ||
         opcode == 32898U;
}

void PirateMatch::Validate(const protocol::Packet& packet) {
  protocol::PacketReader reader{packet.body};
  switch (packet.opcode) {
    case 903U:
    case 904U:
    case 32857U:
      static_cast<void>(reader.ReadU32());
      break;
    case 905U:
    case 906U:
    case 907U:
      break;
    case 908U: {
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU16());
      const auto concurrent = reader.ReadU32();
      const auto count = reader.ReadCount(512U);
      if ((concurrent == 0U && count != 0U) || concurrent > 128U) {
        Invalid();
      }
      std::set<std::pair<std::uint8_t, std::uint8_t>> keys;
      for (std::uint32_t i = 0U; i < count; ++i) {
        if (reader.ReadU32() > 3'600'000U || reader.ReadUtf16(256U).empty()) {
          Invalid();
        }
        static_cast<void>(reader.ReadUtf16(4096U));
        Boolean(reader.ReadU32());
        const auto group = reader.ReadU8();
        const auto index = reader.ReadU8();
        if (!keys.emplace(group, index).second) {
          Invalid();
        }
      }
      break;
    }
    case 32841U: {
      const auto fields = Document(reader.ReadUtf16(4096U));
      const std::array names{u"cmd",     u"grpIdx",    u"grpMobIdx", u"mobType",
                             u"onlySPR", u"repGrpIdx", u"spawnPoint"};
      if (fields.size() != names.size() ||
          !std::ranges::all_of(names, [&](const auto kName) {
            return fields.contains(kName);
          })) {
        Invalid();
      }
      if (fields.at(u"cmd") != u"newSpawn" ||
          (fields.at(u"onlySPR") != u"0" && fields.at(u"onlySPR") != u"1")) {
        Invalid();
      }
      static_cast<void>(IndexValue(fields.at(u"grpIdx")));
      static_cast<void>(IndexValue(fields.at(u"grpMobIdx")));
      break;
    }
    case 32865U: {
      static_cast<void>(reader.ReadU32());
      auto text = reader.ReadUtf16(4096U);
      if (!text.empty() && text.front() == u'\uFEFF') {
        text.erase(0U, 1U);
      }
      if (!text.starts_with(u"cmd=")) {
        Invalid();
      }
      break;
    }
    case 32866U:
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadUtf16(256U));
      Boolean(reader.ReadU32());
      break;
    case 32898U:
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU16());
      static_cast<void>(reader.ReadI8());
      for (unsigned i = 0U; i < 6U; ++i) {
        Finite(reader.ReadF32());
      }
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU8());
      static_cast<void>(reader.ReadU32());
      static_cast<void>(reader.ReadU32());
      break;
    default:
      Invalid();
  }
  reader.RequireEnd();
}

bool PirateMatch::PrepareAction(domain::PlayerId sender,
                                const protocol::Packet& packet,
                                CombatRelay& combat) {
  if (!protocol::IsNativeActionOpcode(packet.opcode)) {
    return true;
  }
  const auto request = protocol::DecodeNativeAction(packet.opcode, packet.body);
  const auto object = request.actor_object_id;
  if (object <= host_object_ || object >= host_object_ + 100'000U) {
    return true;
  }
  if (sender != host_) {
    return false;
  }
  if (packet.opcode == 32810U) {
    const auto& change = *request.weapon_switch;
    if (change.template_id == 0U || change.template_id >= 65536U) {
      Invalid();
    }
    if (!actors_.contains(object)) {
      if (pending_.empty() || actors_.size() >= 4096U ||
          combat.object_ids_.contains(object)) {
        return false;
      }
      const domain::PlayerId synthetic{(std::uint64_t{1U} << 63U) | object};
      CombatRelay::ActorState state;
      state.participant = {.player_id = synthetic,
                           .actor_object_id = object,
                           .team_code = 1U,
                           .weapon_templates = {},
                           .ordinary_respawn_allowed = false};
      state.controller = host_;
      state.pirate_coin_source = true;
      state.melee_uses_equipped_template = true;
      // Released NPC script list maps .KephiOI to KephiOneItem.lua (item 124).
      state.storm_source = pending_.front().storm ? 1U : 0U;
      combat.actors_.emplace(synthetic, std::move(state));
      combat.object_ids_.insert(object);
      combat.object_owners_.emplace(object, synthetic);
      actors_.emplace(object, pending_.front().wave);
      pending_.pop_front();
    }
    combat.actors_.at(combat.object_owners_.at(object))
        .participant.weapon_templates[change.bar_index] = change.template_id;
  }
  return actors_.contains(object);
}

CombatRelayResult PirateMatch::Handle(domain::PlayerId sender,
                                      const protocol::Packet& packet,
                                      CombatRelay& combat, Time now) {
  Validate(packet);
  if (sender != host_ || winner_ != 0U) {
    return Ignored();
  }
  protocol::PacketReader reader{packet.body};
  auto reply = [&](std::uint16_t opcode,
                   const std::vector<std::uint8_t>& body) {
    CombatRelayResult result{
        .accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
    for (const auto& [id, actor] : combat.actors_) {
      if (!actor.controller.has_value()) {
        result.deliveries.push_back(
            {.recipient = id, .message = {.opcode = opcode, .body = body}});
      }
    }
    return result;
  };
  if (packet.opcode == 903U) {
    const auto wave = reader.ReadU32();
    if (wave != cleared_ + 1U || wave == 0U || wave > 100U ||
        wave_ > cleared_) {
      return Ignored();
    }
    wave_ = wave;
    plan_.clear();
    spawns_.clear();
    spawned_.clear();
    clear_requested_ = false;
    started_ = now;
    return reply(904U, packet.body);
  }
  if (packet.opcode == 908U) {
    const auto wave = reader.ReadU32();
    if (wave != wave_ || wave_ <= cleared_) {
      return Ignored();
    }
    if (!plan_.empty()) {
      return plan_ == packet.body ? reply(909U, plan_) : Ignored();
    }
    script_wave_ = reader.ReadU16();
    static_cast<void>(reader.ReadU32());
    const auto count = reader.ReadU32();
    for (std::uint32_t i = 0U; i < count; ++i) {
      static_cast<void>(reader.ReadU32());
      auto mob_type = reader.ReadUtf16(256U);
      auto settings = reader.ReadUtf16(4096U);
      static_cast<void>(reader.ReadU32());
      const auto group = reader.ReadU8();
      const auto index = reader.ReadU8();
      spawns_.push_back({.group = group,
                         .index = index,
                         .settings = std::move(settings),
                         .mob_type = std::move(mob_type)});
    }
    plan_ = packet.body;
    return reply(909U, plan_);
  }
  if (packet.opcode == 904U) {
    const auto wave = reader.ReadU32();
    if (wave != wave_ || wave_ <= cleared_ || plan_.empty()) {
      return Ignored();
    }
    // DB6D90 设置请求已发送标志后等待 905，不会因服务端暂未就绪而重发。
    clear_requested_ = true;
    if (const auto cleared = Advance(combat, now)) {
      return reply(cleared->opcode, cleared->body);
    }
    return {.accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
  }
  if (packet.opcode >= 905U && packet.opcode <= 907U) {
    if (packet.opcode == 907U) {
      return wave_ != 0U && cleared_ == wave_ ? reply(908U, {}) : Ignored();
    }
    const bool alive =
        std::ranges::any_of(combat.actors_, [](const auto& entry) {
          return !entry.second.controller.has_value() &&
                 entry.second.life_state == CombatLifeState::kAlive;
        });
    if (!alive) {
      winner_ = 1U;
    } else if (packet.opcode == 905U && wave_ != 0U && cleared_ == wave_) {
      winner_ = 2U;
    } else {
      return Ignored();
    }
    return {.accepted = true, .reject_reason = std::nullopt, .deliveries = {}};
  }
  if (packet.opcode == 32841U) {
    const auto fields = Document(reader.ReadUtf16(4096U));
    const auto group = IndexValue(fields.at(u"grpIdx"));
    const auto index = IndexValue(fields.at(u"grpMobIdx"));
    const auto expected = std::ranges::find_if(spawns_, [&](const auto& row) {
      return row.group == group && row.index == index &&
             row.settings == fields.at(u"spawnPoint") &&
             MatchesSpawnType(row.mob_type, fields.at(u"mobType"));
    });
    if (plan_.empty() || wave_ <= cleared_ || expected == spawns_.end() ||
        actors_.size() + pending_.size() >= 4096U ||
        !spawned_.emplace(group, index).second) {
      return Ignored();
    }
    pending_.push_back(
        {.wave = wave_, .storm = expected->mob_type == u".KephiOI"});
    last_group_ = group;
  } else if (packet.opcode == 32865U || packet.opcode == 32866U) {
    if (!actors_.contains(reader.ReadU32())) {
      return Ignored();
    }
  } else if (packet.opcode == 32898U) {
    const auto object = reader.ReadU32();
    const auto template_id = reader.ReadU16();
    static_cast<void>(reader.ReadI8());
    for (unsigned i = 0U; i < 6U; ++i) {
      static_cast<void>(reader.ReadF32());
    }
    const auto target = reader.ReadU32();
    static_cast<void>(reader.ReadU8());
    const auto cookie = reader.ReadU32();
    if (!actors_.contains(object) || (target != 0U && target != 0xFFFFFFFFU &&
                                      !combat.object_ids_.contains(target))) {
      return Ignored();
    }
    auto& actor = combat.actors_.at(combat.object_owners_.at(object));
    const auto bar =
        std::ranges::find(actor.participant.weapon_templates,
                          std::optional<std::uint32_t>{template_id});
    if (bar == actor.participant.weapon_templates.end() ||
        !combat.ConsumeActionQuota(actor, now)) {
      return Ignored();
    }
    combat.PruneProjectiles(now);
    // 7BBFB0 registers every GoDamager cookie in Manager+984, including
    // explicit NPC bullets, melee, explosions and storm objects. The native
    // lookup must remain unambiguous across their separate server ledgers.
    if (combat.ContainsDamagerCookie(cookie) ||
        combat.projectiles_.size() >= 8192U) {
      return Ignored();
    }
    auto& projectile = combat.projectiles_.try_emplace(cookie).first->second;
    projectile.owner_object_id = object;
    projectile.created_at = now;
    projectile.weapon_bar = static_cast<std::uint8_t>(
        bar - actor.participant.weapon_templates.begin());
    ++actor.accepted_shots;
  }
  return combat.AcceptedResult(sender, packet);
}

}  // namespace bf::runtime
