#include "protocol/shop/workshop_messages.h"

#include <array>
#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

void WriteItemVector(PacketWriter& writer,
                     std::span<const domain::ItemId> items) {
  if (items.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "workshop item vector is too large"};
  }
  writer.WriteU32(static_cast<std::uint32_t>(items.size()));
  for (const auto item : items) {
    if (!item.IsValid() ||
        item.Value() > std::numeric_limits<std::uint32_t>::max()) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "workshop item ID is invalid"};
    }
    writer.WriteU32(static_cast<std::uint32_t>(item.Value()));
  }
}

}  // namespace

WorkshopRequest DecodeWorkshopRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  WorkshopRequest request;
  const auto count = reader.ReadCount(32U);
  request.input_item_ids.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    request.input_item_ids.push_back(reader.ReadU32());
  }
  request.option = reader.ReadU32();
  reader.RequireEnd();
  return request;
}

AdvancedClothingRequest DecodeAdvancedClothingRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  if (reader.ReadU32() != 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "advanced clothing requires exactly one inventory set"};
  }
  const auto set_id = reader.ReadU32();
  if (set_id == 0U || reader.ReadU32() != 0U) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "advanced clothing requires a nonzero set and option zero"};
  }
  reader.RequireEnd();
  return {.set_id = set_id};
}

DynamicConfigurationRequest DecodeDynamicConfigurationRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto mask = reader.ReadU16();
  for (std::uint32_t bit = 0U; bit < 16U; ++bit) {
    if ((mask & (1U << bit)) != 0U) {
      static_cast<void>(reader.ReadU32());
    }
  }
  reader.RequireEnd();
  return {.mask = mask};
}

std::vector<std::uint8_t> EncodeWorkshopResult(
    const application::WorkshopResult& result) {
  PacketWriter writer;
  writer.WriteU32(0U);
  WriteItemVector(writer, result.rewards);
  WriteItemVector(writer, result.consumed_inputs);
  writer.WriteF64(result.reward_days);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeWorkshopFailure() {
  PacketWriter writer;
  writer.WriteU32(1U);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteF64(0.0);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeWorkshopScienceState() {
  return EncodeWorkshopScienceState(
      application::WorkshopCompoundConfiguration{});
}

std::vector<std::uint8_t> EncodeWorkshopScienceState(
    const application::WorkshopCompoundConfiguration& configuration) {
  if (configuration.compound_card_item_id.Value() >
          std::numeric_limits<std::uint32_t>::max() ||
      configuration.protection_material_item_id.Value() >
          std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "science configuration item exceeds uint32"};
  }
  PacketWriter writer;
  writer.WriteU32(
      static_cast<std::uint32_t>(configuration.compound_card_item_id.Value()));
  writer.WriteU32(static_cast<std::uint32_t>(
      configuration.protection_material_item_id.Value()));
  writer.WriteU32(configuration.minimum_remaining_days);
  writer.WriteU32(100623U);
  constexpr std::array<std::uint32_t, 5U> materials{190111U, 190106U, 190107U,
                                                    190108U, 190109U};
  writer.WriteU32(static_cast<std::uint32_t>(materials.size()));
  for (const auto item : materials) {
    writer.WriteU32(item);
  }
  writer.WriteU32(0U);
  writer.WriteU32(190137U);
  writer.WriteU32(190151U);
  writer.WriteU32(0U);
  for (std::size_t index = 0U; index < 6U; ++index) {
    writer.WriteU32(1U);
  }
  writer.WriteU32(190247U);
  writer.WriteU32(1U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask) {
  return EncodeDynamicConfigurationResponse(requested_mask, {});
}

std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section) {
  return EncodeDynamicConfigurationResponse(requested_mask,
                                            lobby_minigame_section, {});
}

std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section,
    std::span<const std::uint8_t> event_zone_section) {
  const auto science = EncodeWorkshopScienceState();
  return EncodeDynamicConfigurationResponse(
      requested_mask, lobby_minigame_section, event_zone_section, science);
}

std::vector<std::uint8_t> EncodeDynamicConfigurationResponse(
    std::uint16_t requested_mask,
    std::span<const std::uint8_t> lobby_minigame_section,
    std::span<const std::uint8_t> event_zone_section,
    std::span<const std::uint8_t> science_section) {
  if (!event_zone_section.empty() && event_zone_section.size() != 44U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "EventZone section must be exactly 44 bytes"};
  }
  constexpr std::uint16_t k_lobby_minigame = 1U << 3U;
  constexpr std::uint16_t k_dates = 1U << 4U;
  constexpr std::uint16_t k_event_zone = 1U << 6U;
  constexpr std::uint16_t k_science = 1U << 7U;
  const std::uint16_t supported = static_cast<std::uint16_t>(
      k_dates | (science_section.empty() ? 0U : k_science) |
      (lobby_minigame_section.empty() ? 0U : k_lobby_minigame) |
      (event_zone_section.empty() ? 0U : k_event_zone));
  const auto selected = static_cast<std::uint16_t>(requested_mask & supported);
  PacketWriter writer;
  writer.WriteU16(selected);
  // 657530 decodes set sections in ascending bit order from one stream.
  if ((selected & k_lobby_minigame) != 0U) {
    writer.WriteBytes(lobby_minigame_section);
  }
  if ((selected & k_dates) != 0U) {
    writer.WriteF64(
        43831.0);  // 2020-01-01 相对 1899-12-30 的 OLE Automation 日期。
    writer.WriteF64(72686.0);  // 2099-01-01。
  }
  if ((selected & k_event_zone) != 0U) {
    writer.WriteBytes(event_zone_section);
  }
  if ((selected & k_science) != 0U) {
    writer.WriteBytes(science_section);
  }
  return writer.Release();
}

}  // namespace bf::protocol
