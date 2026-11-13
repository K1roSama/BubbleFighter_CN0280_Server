#include "protocol/scenes/scene_session_messages.h"

#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"

namespace bf::protocol {
namespace {

[[nodiscard]] std::uint32_t CheckedU32(std::uint64_t value,
                                       std::string_view message) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t CheckedU32(std::int64_t value,
                                       std::string_view message) {
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

void WriteZeroU32(PacketWriter& writer, std::size_t count) {
  for (std::size_t index = 0U; index < count; ++index) {
    writer.WriteU32(0U);
  }
}

[[nodiscard]] std::uint16_t ActiveFighter(
    const application::PlayerMenuSnapshot& snapshot) {
  std::optional<std::uint16_t> selected;
  for (const auto& item : snapshot.items) {
    if (!item.equipped || item.slot_code != "character") {
      continue;
    }
    if (item.item_id.Value() <= 300'000U || item.item_id.Value() >= 301'000U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                          "character template is out of range"};
    }
    const auto raw = item.item_id.Value() - 300'000U;
    if (raw > std::numeric_limits<std::uint16_t>::max() ||
        selected.has_value()) {
      throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                          "scene slot contains ambiguous fighter equipment"};
    }
    selected = static_cast<std::uint16_t>(raw);
  }
  return selected.value_or(
      static_cast<std::uint16_t>(snapshot.initial_fighter));
}

void WriteEquippedItemIds(PacketWriter& writer,
                          const application::PlayerMenuSnapshot& snapshot) {
  std::size_t count = 0U;
  for (const auto& item : snapshot.items) {
    count += item.equipped ? 1U : 0U;
  }
  writer.WriteU32(
      CheckedU32(count, "scene equipment vector exceeds uint32 count"));
  for (const auto& item : snapshot.items) {
    if (item.equipped) {
      writer.WriteU32(CheckedU32(
          item.item_id.Value(), "scene item ID exceeds legacy protocol range"));
    }
  }
}

void ValidatePlayer(const application::PlayerMenuSnapshot& player) {
  if (!player.player_id.IsValid() || player.nickname.empty() ||
      player.progress.level < 0 || player.level_icon_type > 3U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "occupied scene slot has invalid player identity"};
  }
}

void ValidateSlot(const SceneSessionSlot& slot, std::size_t expected_index) {
  if (slot.party.party_id != 0xFFFFFFFFU && slot.party.party_id >= 8U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "party badge exceeds native texture array"};
  if (static_cast<std::size_t>(slot.slot) != expected_index) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "scene slot index does not match vector position"};
  }
  if (slot.occupied != slot.player.has_value() ||
      (!slot.occupied && slot.ready) || (slot.occupied && slot.locked) ||
      (!slot.occupied && slot.team != 255U) ||
      (slot.occupied && slot.team > 16U && slot.team != 36U) ||
      (slot.team == 36U && slot.ready)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "scene slot occupancy flags are inconsistent"};
  }
  if (slot.player.has_value()) {
    ValidatePlayer(*slot.player);
  }
}

void WriteUserSlot(PacketWriter& writer, const SceneSessionSlot& slot) {
  const auto* player = slot.player ? &*slot.player : nullptr;
  writer.WriteU32(slot.occupied ? 1U : 0U);
  writer.WriteU32(slot.locked ? 1U : 0U);
  writer.WriteU32(0U);
  writer.WriteU8(slot.slot);
  writer.WriteU32(
      player == nullptr
          ? 0U
          : CheckedU32(player->player_id.Value(),
                       "scene player ID exceeds legacy protocol range"));
  writer.WriteUtf16(player == nullptr ? std::u16string{}
                                      : Utf8ToUtf16(player->nickname));
  writer.WriteU16(player == nullptr ? 1U : ActiveFighter(*player));
  writer.WriteU16(player == nullptr ? 0U : player->color_code);
  writer.WriteU8(slot.team);
  writer.WriteU32(player == nullptr
                      ? 0U
                      : CheckedU32(player->progress.level,
                                   "scene player level exceeds uint32 range"));
  writer.WriteU32(slot.ready ? 1U : 0U);
  WriteZeroU32(writer, 5U);
  if (player == nullptr) {
    writer.WriteU32(0U);
  } else {
    WriteEquippedItemIds(writer, *player);
  }
  WriteZeroU32(writer, 2U);
  writer.WriteU32(std::numeric_limits<std::uint32_t>::max());
  WriteZeroU32(writer, 2U);
  writer.WriteU32(slot.party.leader ? 1U : 0U);
  writer.WriteU32(slot.party.party_id);
  for (std::size_t index = 0U; index < 5U; ++index) {
    writer.WriteU16(std::numeric_limits<std::uint16_t>::max());
  }
  writer.WriteU32(0U);
  writer.WriteU32(1U);
  writer.WriteU32(slot.party.recruiting ? 1U : 0U);
  const auto* couple = player && player->couple ? &*player->couple : nullptr;
  writer.WriteU32(couple ? CheckedU32(couple->partner_id.Value(),
                                      "couple partner exceeds uint32")
                         : std::numeric_limits<std::uint32_t>::max());
  writer.WriteUtf16(couple ? Utf8ToUtf16(couple->partner_nickname) : u"");
  WriteZeroU32(writer, 2U);
  writer.WriteU32(player == nullptr ? 0U : player->level_icon_type);
  writer.WriteU32(1U);
  WriteZeroU32(writer, 2U);
  WriteZeroU32(writer, 8U);
  writer.WriteU64(0U);
  writer.WriteU32(0U);
  writer.WriteU32(player == nullptr ? 0U : player->clan_mark);
  writer.WriteU32(player == nullptr ? 0U : player->clan_id);
}

void ValidateSnapshot(const SceneSessionSnapshot& snapshot) {
  if (snapshot.slots.empty() || snapshot.slots.size() > 128U ||
      snapshot.host_slot >= snapshot.slots.size() ||
      snapshot.selected_slot >= snapshot.slots.size()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "scene session slot indexes are invalid"};
  }
  if (!snapshot.slots[snapshot.host_slot].occupied ||
      !snapshot.slots[snapshot.selected_slot].occupied) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "scene session host or selected slot is empty"};
  }

  std::set<domain::PlayerId> players;
  std::size_t occupied_count = 0U;
  std::size_t observer_count = 0U;
  for (std::size_t index = 0U; index < snapshot.slots.size(); ++index) {
    const auto& slot = snapshot.slots[index];
    ValidateSlot(slot, index);
    if (slot.player.has_value()) {
      if (!players.insert(slot.player->player_id).second) {
        throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                            "scene session contains duplicate player"};
      }
      ++occupied_count;
      observer_count += slot.team == 36U ? 1U : 0U;
    }
  }
  if (snapshot.scene.scene_type == 2U) {
    const bool hosted = snapshot.scene.configuration.field_12 != 0U;
    const bool hide_seek =
        !hosted && snapshot.scene.configuration.mode_id == 1U &&
        snapshot.scene.configuration.auxiliary_label == "hide_seek_v1" &&
        snapshot.scene.capacity <= 8U;
    const auto expected_slots = hosted ? 16U : snapshot.scene.capacity;
    if (snapshot.slots.size() != expected_slots ||
        occupied_count - observer_count != snapshot.scene.occupants ||
        observer_count != snapshot.scene.observers ||
        snapshot.scene.occupants > snapshot.scene.capacity ||
        (hosted && (snapshot.scene.capacity > 12U ||
                    snapshot.scene.configuration.mode_id == 23U ||
                    snapshot.host_slot != 12U || observer_count != 1U ||
                    snapshot.slots[12U].team != 36U)) ||
        (!hosted && !hide_seek && observer_count != 0U)) {
      throw ProtocolError{
          ProtocolErrorCode::kInvalidFrame,
          "room scene capacity or occupant count does not match slots"};
    }
    for (const auto& slot : snapshot.slots) {
      const auto mode = snapshot.scene.configuration.mode_id;
      if (slot.occupied && slot.team != 36U &&
          (slot.team < 1U ||
           slot.team > ((mode == 13U || mode == 23U) ? 16U : 2U))) {
        throw ProtocolError{
            ProtocolErrorCode::kInvalidFrame,
            "room participant faction does not belong to this mode"};
      }
      if (slot.occupied && slot.team != 36U &&
          slot.slot >= snapshot.scene.capacity) {
        throw ProtocolError{
            ProtocolErrorCode::kInvalidFrame,
            "room participant exceeds configured combat capacity"};
      }
    }
  }
}

}  // namespace

std::vector<std::uint8_t> EncodeSceneMemberStateResponse(
    const SceneSessionSlot& slot) {
  ValidateSlot(slot, slot.slot);
  PacketWriter writer;
  writer.WriteU32(slot.slot);
  WriteUserSlot(writer, slot);
  return writer.Release();
}

std::vector<std::uint8_t> EncodePartyRecruitmentTextResponse(
    std::uint8_t slot, std::u16string_view text) {
  if (text.size() > 20U)
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "recruitment text exceeds native limit"};
  PacketWriter writer;
  writer.WriteU16(slot);
  writer.WriteUtf16(text);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeSceneSessionResponse(
    const SceneSessionSnapshot& snapshot) {
  ValidateSnapshot(snapshot);
  PacketWriter writer;
  EncodeSceneInfo(writer, snapshot.scene);
  writer.WriteU32(snapshot.host_slot);
  writer.WriteU32(snapshot.selected_slot);
  writer.WriteU32(CheckedU32(snapshot.slots.size(),
                             "scene slot vector exceeds uint32 count"));
  for (const auto& slot : snapshot.slots) {
    WriteUserSlot(writer, slot);
  }

  std::size_t occupied_count = 0U;
  if (snapshot.include_equipment_records) {
    for (const auto& slot : snapshot.slots) {
      occupied_count += slot.occupied ? 1U : 0U;
    }
  }
  writer.WriteU32(
      CheckedU32(occupied_count, "scene equipment list exceeds uint32 count"));
  if (snapshot.include_equipment_records) {
    for (const auto& slot : snapshot.slots) {
      if (slot.player.has_value()) {
        const auto equipment = EncodeEquippedItemsResponse(*slot.player);
        writer.WriteBytes(equipment);
      }
    }
  }
  WriteZeroU32(writer, 2U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeSceneMemberUpdateResponse(
    const SceneSessionSlot& slot) {
  ValidateSlot(slot, slot.slot);
  if (!slot.occupied || !slot.player.has_value()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "scene member update requires an occupied player slot"};
  }
  PacketWriter writer;
  writer.WriteU32(slot.slot);
  WriteUserSlot(writer, slot);
  const auto equipment = EncodeEquippedItemsResponse(*slot.player);
  writer.WriteBytes(equipment);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeSceneMemberRemovedResponse(std::uint8_t slot) {
  const SceneSessionSlot empty_slot{.slot = slot,
                                    .team = 255U,
                                    .occupied = false,
                                    .locked = false,
                                    .ready = false,
                                    .player = std::nullopt};
  PacketWriter writer;
  writer.WriteU32(slot);
  WriteUserSlot(writer, empty_slot);
  return writer.Release();
}

application::LevelIconSelection DecodeLevelIconRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  application::LevelIconSelection result{.display_type = reader.ReadU32(),
                                         .clan_id = reader.ReadU32(),
                                         .clan_mark = reader.ReadU32()};
  reader.RequireEnd();
  return result;
}

std::vector<std::uint8_t> EncodeLevelIconResponse(
    const SceneSessionSlot& slot) {
  ValidateSlot(slot, slot.slot);
  if (!slot.occupied)
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "level icon requires an occupied slot"};
  PacketWriter writer;
  WriteUserSlot(writer, slot);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCoupleSlotResponse(
    const SceneSessionSlot& slot) {
  ValidateSlot(slot, slot.slot);
  if (!slot.occupied)
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "couple update requires an existing slot"};
  PacketWriter writer;
  writer.WriteU32(1U);
  WriteUserSlot(writer, slot);
  return writer.Release();
}

}  // namespace bf::protocol
