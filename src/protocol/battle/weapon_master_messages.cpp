#include "protocol/battle/weapon_master_messages.h"

#include <array>
#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {

WeaponMasterAssistRequest DecodeWeaponMasterAssist(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto event = reader.ReadU8();
  WeaponMasterAssistRequest result{.finisher_object = reader.ReadU32(),
                                   .assistant_seat = reader.ReadU8()};
  reader.RequireEnd();
  if (event != 39U || result.assistant_seat >= 36U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid weapon master assist event"};
  }
  return result;
}

void ValidateWeaponMasterReroll(std::span<const std::uint8_t> body) {
  PacketReader{body}.RequireEnd();
}

void ValidateWeaponMasterProgress(const WeaponMasterProgress& progress) {
  constexpr auto maximum =
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  if (progress.level == 0U || progress.level > maximum ||
      progress.level_next > maximum ||
      progress.level_base >= progress.level_next ||
      progress.experience < progress.level_base ||
      progress.experience > progress.level_next || progress.assists > maximum ||
      (progress.level == 1U && progress.level_base != 0U)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid weapon master progress range"};
  }
}

std::vector<std::uint8_t> EncodeWeaponMasterChange(
    std::uint8_t seat, std::span<const WeaponMasterWeapon> weapons,
    WeaponMasterChangeReason reason) {
  const auto wire_reason = static_cast<std::uint32_t>(reason);
  if (seat >= 36U || weapons.empty() || weapons.size() > 5U ||
      wire_reason < 1U || wire_reason > 3U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid weapon master change header"};
  }
  std::array<bool, 5U> occupied{};
  for (const auto& weapon : weapons) {
    // 84AA00 intentionally preserves bars 1/2 for level-up and revival. Do not
    // silently encode replacements the native consumer would skip.
    if (weapon.bar >= occupied.size() || occupied[weapon.bar] ||
        weapon.template_id == 0U ||
        weapon.template_id > static_cast<std::uint32_t>(
                                 std::numeric_limits<std::int32_t>::max()) ||
        (reason != WeaponMasterChangeReason::kStart &&
         (weapon.bar == 1U || weapon.bar == 2U))) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid weapon master change entry"};
    }
    occupied[weapon.bar] = true;
  }
  PacketWriter writer;
  writer.WriteU32(seat);
  writer.WriteU32(static_cast<std::uint32_t>(weapons.size()));
  for (const auto& weapon : weapons) writer.WriteU32(weapon.bar);
  writer.WriteU32(static_cast<std::uint32_t>(weapons.size()));
  for (const auto& weapon : weapons) writer.WriteU32(weapon.template_id);
  writer.WriteU32(wire_reason);
  return writer.Release();
}

}  // namespace bf::protocol
