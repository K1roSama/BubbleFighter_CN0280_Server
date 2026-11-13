#include "protocol/battle/smart_ping_messages.h"

#include <bit>
#include <cmath>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
bool SmartPingMode(std::uint32_t mode, std::uint32_t kind) noexcept {
  // A40790 -> 11504D0 / 1150570 / 11503C0, with explicit 24/41 exclusion.
  // Item pings are restricted further by A40790's final switch.
  if (kind == 4U)
    return mode == 23U || mode == 25U || mode == 30U || mode == 42U ||
           mode == 44U;
  if (kind > 3U) return false;
  switch (mode) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 9:
    case 11:
    case 12:
    case 14:
    case 16:
    case 18:
    case 19:
    case 20:
    case 21:
    case 23:  // Personal survival: local markers, without disclosing them to
              // opponents.
    case 25:
    case 27:
    case 28:
    case 29:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
    case 38:
    case 39:
    case 42:
    case 43:
    case 44:
    case 101:
    case 102:
    case 103:
    case 106:
      return true;
    default:
      return false;
  }
}

namespace {
void Validate(const SmartPing& v) {
  if (v.actor == 0U || v.kind > 4U ||
      (v.kind == 4U ? (v.object == 0U || v.object == 0xffffffffU)
                    : v.object != 0xffffffffU))
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid smart ping identity or kind"};
  for (float f : v.position)
    if (!std::isfinite(f) || std::abs(f) > 1000000.0F)
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid smart ping position"};
  for (float f : v.normal)
    if (!std::isfinite(f) || std::abs(f) > 1.01F)
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid smart ping normal"};
}
}  // namespace

SmartPing DecodeSmartPing(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  SmartPing result{.actor = reader.ReadU32(),
                   .object = reader.ReadU32(),
                   .kind = reader.ReadU32()};
  for (auto& f : result.position) f = std::bit_cast<float>(reader.ReadU32());
  for (auto& f : result.normal) f = std::bit_cast<float>(reader.ReadU32());
  reader.RequireEnd();
  Validate(result);
  return result;
}

std::vector<std::uint8_t> EncodeSmartPing(const SmartPing& value) {
  Validate(value);
  PacketWriter writer;
  writer.WriteU32(value.actor);
  writer.WriteU32(value.object);
  writer.WriteU32(value.kind);
  for (float f : value.position)
    writer.WriteU32(std::bit_cast<std::uint32_t>(f));
  for (float f : value.normal) writer.WriteU32(std::bit_cast<std::uint32_t>(f));
  return writer.Release();
}
}  // namespace bf::protocol
