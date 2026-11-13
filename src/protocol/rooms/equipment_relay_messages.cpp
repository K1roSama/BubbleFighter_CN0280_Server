#include "protocol/rooms/equipment_relay_messages.h"

#include <algorithm>
#include <cmath>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {
void RequireObject(std::uint32_t object) {
  // 7BB450 / 7BB5B0 以 (object - 1000000) / 100000 寻找 36 个玩家席位。
  if (object < 1'000'000U || object >= 4'600'000U)
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "equipment sequence actor is not a player"};
}

void WriteBits(PacketWriter& writer, const EquipmentSequenceBits& bits) {
  if ((bits.back() & 0xFFFFFF00U) != 0U)
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "equipment sequence has nonzero padding"};
  writer.WriteU32(static_cast<std::uint32_t>(bits.size()));
  for (auto value : bits) writer.WriteU32(value);
}
}  // namespace

EquipmentHealthEvent DecodeEquipmentHealthEvent(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  EquipmentHealthEvent event;
  event.actor = reader.ReadU32();
  event.reason = reader.ReadU8();
  event.tick = reader.ReadU32() & 0x00FFFFFFU;
  // Every 7D65D0 caller in build 752: 0..13, 15, 16. No caller emits 14.
  if (event.reason > 16U || event.reason == 14U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown native health event reason"};
  if (event.reason == 16U) event.detail = reader.ReadU8();
  reader.RequireEnd();
  RequireObject(event.actor);
  return event;
}

EquipmentActorReport DecodeEquipmentActorReport(
    std::uint16_t opcode, std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  EquipmentActorReport report{.opcode = opcode, .actor = reader.ReadU32()};
  switch (opcode) {
    case 11U:  // 7C78C0 / 7EDE80: actor, state reason, value, tick.
      report.kind = reader.ReadU8();
      report.value = reader.ReadU16();
      report.tick = reader.ReadU32() & 0x00FFFFFFU;
      if (report.kind > 11U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "unknown native state report reason"};
      break;
    case 13U:  // 7D64D0 / 7ED550: local victim, source, value, tick, two
               // samples.
      report.related = reader.ReadU32();
      report.value = reader.ReadU16();
      report.tick = reader.ReadU32() & 0x00FFFFFFU;
      report.first = reader.ReadU16();
      report.second = reader.ReadU16();
      break;
    case 15U:  // 7D67C0 / 7EA8A0.
      report.value = reader.ReadU16();
      report.tick = reader.ReadU32() & 0x00FFFFFFU;
      break;
    case 14U:  // 7D66C0 -> 7EA710: actor and low-24-bit match clock.
      report.tick = reader.ReadU32() & 0x00FFFFFFU;
      break;
    case 24U:  // 7C8B00 -> 7EA140 / 7EA200, after 7C8920 changes the selected
               // weapon.
      for (auto& value : report.critical) {
        value = reader.ReadF32();
        if (!std::isfinite(value))
          throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                              "non-finite critical buff value"};
      }
      break;
    case 22U:  // 7C7A40 / 7E9FB0: previous action-state group.
    case 25U:  // B7F120 -> B8D620: local actor and dynamic-config flag.
      report.kind = reader.ReadU8();
      break;
    case 26U:  // 851730 / 7E5F80: local actor entering a map region.
      report.value = reader.ReadU16();
      break;
    case 27U:  // B7F600 -> B8EDD0: actor, type, source, copied type/padding,
               // seat.
      report.value = reader.ReadU16();
      report.related = reader.ReadU32();
      report.first = static_cast<std::uint16_t>(reader.ReadU32());
      report.seat = reader.ReadU32();
      if (report.first != report.value || report.seat >= 36U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "invalid native event acknowledgement"};
      break;
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "unsupported equipment actor report"};
  }
  reader.RequireEnd();
  RequireObject(report.actor);
  if (report.actor % 100000U != 0U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid equipment report actor"};
  return report;
}

std::vector<std::uint8_t> EncodeEquipmentRelayEndpoint(
    const std::array<std::uint8_t, 4U>& ipv4, std::uint16_t port,
    const EquipmentRelayKey& key) {
  if (port == 0U ||
      std::ranges::all_of(ipv4, [](auto byte) { return byte == 0U; }))
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "equipment relay endpoint is empty"};
  PacketWriter writer;
  writer.WriteBytes(ipv4);
  writer.WriteU16(port);
  writer.WriteBytes(key);
  return writer.Release();
}

EquipmentRelayLogin DecodeEquipmentRelayLogin(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  EquipmentRelayLogin result;
  const auto key = reader.ReadBytes(result.key.size());
  std::ranges::copy(key, result.key.begin());
  result.seat = reader.ReadU8();
  result.client_context = reader.ReadU32();
  result.profile_id = reader.ReadU32();
  result.nickname = Utf16ToUtf8(reader.ReadUtf16(64U));
  reader.RequireEnd();
  if (result.seat >= 36U || result.nickname.empty())
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "invalid equipment relay identity"};
  return result;
}

std::vector<std::uint8_t> EncodeEquipmentDefenseSequence(
    std::uint32_t object, const EquipmentDefenseSequence& sequence) {
  RequireObject(object);
  if (std::ranges::any_of(sequence.defense,
                          [](auto value) { return value > 100U; }))
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "defense percentage exceeds 100"};
  PacketWriter writer;
  writer.WriteU32(object);
  writer.WriteU32(static_cast<std::uint32_t>(sequence.defense.size()));
  writer.WriteBytes(sequence.defense);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeEquipmentCriticalCancelSequence(
    std::uint32_t object, const EquipmentDefenseSequence& sequence) {
  RequireObject(object);
  PacketWriter writer;
  writer.WriteU32(object);
  WriteBits(writer, sequence.critical_cancel);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeEquipmentCriticalAttackSequences(
    std::uint32_t object, const EquipmentSequenceBits& first,
    const EquipmentSequenceBits& second) {
  RequireObject(object);
  PacketWriter writer;
  writer.WriteU32(object);
  WriteBits(writer, first);
  WriteBits(writer, second);
  return writer.Release();
}
}  // namespace bf::protocol
