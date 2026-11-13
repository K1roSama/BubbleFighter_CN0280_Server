#include "protocol/battle/npc_messages.h"

#include <cmath>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {
[[noreturn]] void Invalid() {
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "invalid observed NPC payload"};
}

float Finite(PacketReader& reader) {
  const auto value = reader.ReadF32();
  // Conservative server parser bound, not a claim about the native map limits.
  if (!std::isfinite(value) || std::abs(value) > 1.0e7F) Invalid();
  return value;
}

std::array<float, 3U> Vector(PacketReader& reader) {
  return {Finite(reader), Finite(reader), Finite(reader)};
}

bool Boolean(PacketReader& reader) {
  const auto word = reader.ReadU32();
  if (word > 1U) Invalid();
  return word != 0U;
}
}  // namespace

bool IsObservedNpcSyncOpcode(std::uint16_t opcode) noexcept {
  switch (opcode) {
    case 32842U:
    case 32843U:
    case 32844U:
    case 32845U:
    case 32846U:
    case 32847U:
    case 32848U:
    case 32849U:
    case 32852U:
    case 32853U:
    case 32859U:
    case 32860U:
    case 32861U:
    case 32862U:
    case 32863U:
    case 32864U:
      return true;
    default:
      return false;
  }
}

NativeNpcPayload DecodeObservedNpcSync(std::uint16_t opcode,
                                       std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NativeNpcPayload result;
  switch (opcode) {
    case 32842U:
      result = NativeNpcCreate{reader.ReadUtf16(256U), Vector(reader),
                               Finite(reader), reader.ReadU32(),
                               reader.ReadUtf16(4096U)};
      break;
    case 32843U:
      // 7EFC80 sign-extends the single BYTE at offset8. Never align the vec3
      // at9.
      result = NativeNpcDamage{
          reader.ReadU32(), reader.ReadU32(), reader.ReadI8(), Vector(reader),
          Vector(reader),   reader.ReadU32(), Finite(reader)};
      break;
    case 32844U:
    case 32848U:
    case 32852U:
    case 32853U:
      result = NativeNpcPair{reader.ReadU32(), reader.ReadU32()};
      break;
    case 32845U:
      result = NativeNpcTransition{reader.ReadU32(), reader.ReadU32(),
                                   Vector(reader), reader.ReadU32(),
                                   Boolean(reader)};
      break;
    case 32846U:
    case 32847U:
      result = NativeNpcVector{reader.ReadU32(), Vector(reader)};
      break;
    case 32849U:
      result = NativeNpcKicked{reader.ReadU32(), Boolean(reader),
                               reader.ReadU32(), Vector(reader)};
      break;
    case 32859U:
    case 32863U:
      result = NativeNpcTrigger{reader.ReadU32()};
      break;
    case 32860U:
      result = NativeNpcNamedSpawn{reader.ReadUtf16(256U), reader.ReadU32(),
                                   Vector(reader), Finite(reader)};
      break;
    case 32861U:
    case 32864U:
      result = NativeNpcText{reader.ReadUtf16(4096U)};
      break;
    case 32862U:
      result = NativeNpcGroupedSpawn{reader.ReadU32(), reader.ReadU32(),
                                     reader.ReadUtf16(256U), Vector(reader),
                                     Finite(reader)};
      break;
    default:
      Invalid();
  }
  reader.RequireEnd();
  return result;
}
}  // namespace bf::protocol
