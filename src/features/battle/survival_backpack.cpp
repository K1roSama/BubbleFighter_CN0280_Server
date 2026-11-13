#include <cmath>

#include "features/battle/survival_loot.h"
#include "protocol/codec/packet_io.h"

namespace bf::runtime {
SurvivalBackpack DecodeSurvivalBackpack(std::span<const std::uint8_t> body) {
  protocol::PacketReader reader{body};
  SurvivalBackpack bag;
  bag.actor = reader.ReadU32();
  const auto vector = [&reader](auto& value, float bound) {
    for (auto& axis : value) {
      axis = reader.ReadF32();
      if (!std::isfinite(axis) || std::abs(axis) > bound)
        throw protocol::ProtocolError{
            protocol::ProtocolErrorCode::kInvalidValue,
            "invalid survival backpack vector"};
    }
  };
  vector(bag.position, 10'000'000.0F);
  vector(bag.velocity, 100.0F);
  bag.nickname = reader.ReadUtf16(64U);
  const auto count = reader.ReadCount(14U);
  for (std::uint32_t i = 0; i < count; ++i) {
    SurvivalBackpackItem item{reader.ReadU32(), reader.ReadU32(),
                              reader.ReadU32()};
    // 53A110 includes occupied slots even when their current stock is zero.
    if (item.quantity > 999U)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid survival backpack quantity"};
    bag.items.push_back(item);
  }
  reader.RequireEnd();
  return bag;
}

OutboundMessage EncodeSurvivalBackpack(const SurvivalBackpack& bag) {
  protocol::PacketWriter writer;
  writer.WriteU32(bag.id);
  writer.WriteU32(bag.actor);
  for (auto axis : bag.position) writer.WriteF32(axis);
  for (auto axis : bag.velocity) writer.WriteF32(axis);
  writer.WriteUtf16(bag.nickname);
  writer.WriteU32(static_cast<std::uint32_t>(bag.items.size()));
  for (const auto& item : bag.items) {
    writer.WriteU32(item.type);
    writer.WriteU32(item.weapon);
    writer.WriteU32(item.quantity);
  }
  return {.opcode = 754U, .body = writer.Release()};
}
}  // namespace bf::runtime
