#include "features/battle/pirate_map_agents.h"

#include <algorithm>
#include <cmath>

#include "protocol/codec/packet_io.h"

namespace bf::runtime {
namespace {
[[noreturn]] void Invalid() {
  throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                "invalid pirate map agent payload"};
}

std::uint32_t Slot(protocol::PacketReader& reader) {
  const auto value = reader.ReadU32();
  if (value >= 64U) Invalid();
  return value;
}

void Position(protocol::PacketReader& reader) {
  for (unsigned i = 0U; i < 3U; ++i) {
    const auto value = reader.ReadF32();
    if (!std::isfinite(value) || std::abs(value) > 1.0e7F) Invalid();
  }
}
}  // namespace

bool PirateMapAgents::Accepts(std::uint16_t opcode) noexcept {
  return opcode >= 32890U && opcode <= 32893U;
}

void PirateMapAgents::Validate(const protocol::Packet& packet) {
  protocol::PacketReader reader{packet.body};
  static_cast<void>(reader.ReadU32());
  switch (packet.opcode) {
    case 32892U: {
      static_cast<void>(reader.ReadU32());
      const auto time = reader.ReadF64();
      if (!std::isfinite(time) || time < 0.0) Invalid();
      static_cast<void>(Slot(reader));
      Position(reader);
      break;
    }
    case 32893U:
      static_cast<void>(Slot(reader));
      Position(reader);
      break;
    case 32890U:
      // 7D2940 looks up the object and ignores this hint. ABB8F0 can send -1
      // when a delayed rescue cleanup has no target; it is not a malformed
      // frame.
      static_cast<void>(reader.ReadU32());
      break;
    case 32891U:
      if (reader.ReadUtf16(256U).empty()) Invalid();
      static_cast<void>(reader.ReadU32());
      break;
    default:
      Invalid();
  }
  reader.RequireEnd();
}

bool PirateMapAgents::Apply(const protocol::Packet& packet,
                            std::uint32_t host_object, CombatRelay* combat,
                            std::chrono::steady_clock::time_point now) {
  Validate(packet);
  if (guardian_map_ != 0U)
    return ApplyGuardian(packet, host_object, combat, now);
  protocol::PacketReader reader{packet.body};
  const auto object = reader.ReadU32();
  if (packet.opcode == 32892U) {
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadF64());
    auto& agent = agents_[Slot(reader)];
    if (agent.active || agent.spawn == packet.body) return false;
    agent = {.active = true, .spawn = packet.body, .object = std::nullopt};
    return true;
  }
  // mode 101 的 ChrAgent 使用房主对象号段；不接受其他真人角色号或其他玩家号段。
  if (object <= host_object ||
      static_cast<std::uint64_t>(object) >=
          static_cast<std::uint64_t>(host_object) + 100'000U)
    return false;
  if (packet.opcode == 32891U) {
    if (!animated_objects_.contains(object)) {
      const auto active = std::ranges::count_if(
          agents_, [](const auto& a) { return a.active; });
      if (animated_objects_.size() >= static_cast<std::size_t>(active))
        return false;
      animated_objects_.insert(object);
    }
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
  if (std::ranges::any_of(agents_, [&](const auto& other) {
        return &other != &agent && other.active && other.object == object;
      }))
    return false;
  agent.object = object;
  if (packet.opcode == 32890U) {
    agent.active = false;
    animated_objects_.erase(object);
  } else {
    animated_objects_.insert(object);
  }
  return true;
}
}  // namespace bf::runtime
