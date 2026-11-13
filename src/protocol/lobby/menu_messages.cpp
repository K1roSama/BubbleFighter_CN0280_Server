#include "protocol/lobby/menu_messages.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::protocol {
namespace {

[[nodiscard]] std::uint32_t CheckedU32(std::uint64_t value,
                                       std::string_view message) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

void ValidateIdentity(const application::PlayerMenuSnapshot& snapshot) {
  if (!snapshot.player_id.IsValid() || snapshot.nickname.empty() ||
      snapshot.progress.level <= 0) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidFrame,
        "menu snapshot does not contain a valid player identity"};
  }
}

}  // namespace

void ValidateNativeAuxiliaryReport(std::uint16_t opcode,
                                   std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  if (opcode == 2006U || opcode == 2040U) {
    // 2040: 8FC7A0 sends one DWORD from the option dialog (98AE80) and on
    // gate-ready.
    static_cast<void>(reader.ReadU32());
  } else if (opcode == 711U) {
    // A88410 -> A8BF70: argument, decoded session counter (+612/+616) and an
    // xmm float. The matching S2C711 is swallowed by B3CC30, so the report
    // needs no answer.
    for (unsigned index = 0U; index < 3U; ++index) {
      static_cast<void>(reader.ReadU32());
    }
  } else if (opcode == 2045U) {
    // 64D000/64D140 -> 64DBE0: two DWORDs (kind, value) from the Luxury badge
    // state.
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
  } else if (opcode == 2007U) {
    // 7AB1C0 -> 7ABA20: empty five-minute refresh while the ladder dialog is
    // open.
  } else if (opcode == 927U) {
    // 581E00: empty notice after a lethal PVE hit (modes 16/101); not a life
    // change.
  } else if (opcode == 2024U) {
    // 7CD190 -> 7EDD10：两个三维向量、固定值 5、7FE840 返回的席位/脚本索引。
    for (unsigned index = 0U; index < 6U; ++index) {
      const auto value = reader.ReadF32();
      if (!std::isfinite(value) || std::abs(value) > 10'000'000.0F) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "auxiliary report vector is invalid"};
      }
    }
    if (reader.ReadU32() != 5U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "auxiliary report kind is invalid"};
    }
    static_cast<void>(reader.ReadU32());
  } else {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown auxiliary report opcode"};
  }
  reader.RequireEnd();
}

std::vector<std::uint8_t> EncodeEventZoneConfig(const EventZoneConfig& config) {
  PacketWriter writer;
  writer.WriteU32(config.open ? 1U : 0U);
  writer.WriteU32(config.wait_ms);
  writer.WriteU32(config.reserved);
  writer.WriteF64(config.day);
  writer.WriteF64(config.start);
  writer.WriteF64(config.end);
  writer.WriteU32(config.daily_open);
  writer.WriteU32(config.daily_close);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeAwardInfo(std::uint32_t target,
                                          std::uint32_t result,
                                          std::span<const MedalCount> medals) {
  if (medals.size() > 4096U) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "award list exceeds bound"};
  }
  PacketWriter writer;
  writer.WriteU32(target);
  writer.WriteU32(result);
  writer.WriteU32(static_cast<std::uint32_t>(medals.size()));
  for (const auto& medal : medals) {
    writer.WriteU32(medal.medal_id);
    writer.WriteU32(medal.count);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTitleClaimResponse(std::uint32_t title,
                                                   std::int32_t result,
                                                   std::u16string_view text) {
  PacketWriter writer;
  writer.WriteI32(result);
  writer.WriteU32(title);
  writer.WriteUtf16(text);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeBubbleCashExchangeResponse(
    bool success, std::uint32_t amount) {
  PacketWriter writer;
  writer.WriteU32(success ? 1U : 0U);
  writer.WriteU32(amount);
  return writer.Release();
}

void ValidateEventZoneConfigRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

EventZoneRequest DecodeEventZoneRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  EventZoneRequest request{.mode = reader.ReadU32(),
                           .cached_field_4 = reader.ReadU32(),
                           .cached_field_8 = reader.ReadU32(),
                           .client_tick = reader.ReadU32()};
  const auto selection_byte = reader.ReadU32();
  if (selection_byte > std::numeric_limits<std::uint8_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "event zone selection exceeds original byte"};
  }
  request.selection_byte = static_cast<std::uint8_t>(selection_byte);
  request.map_key = reader.ReadUtf16(256U);
  reader.RequireEnd();
  return request;
}

bool DecodeEventZoneCancelRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto action = reader.ReadU32();
  if (action > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "event zone action must be zero or one"};
  }
  reader.RequireEnd();
  return action == 1U;
}

std::vector<std::uint8_t> EncodeEventZoneUnavailableResponse(bool cancel) {
  PacketWriter writer;
  writer.WriteU32(cancel ? 1U : 22U);
  for (unsigned index = 0U; index < 4U; ++index) {
    writer.WriteU32(0U);
  }
  writer.WriteUtf16(u"");
  writer.WriteU32(0U);
  return writer.Release();
}

WorldContextFlagRequest DecodeWorldContextFlagRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto value = reader.ReadU8();
  if (value > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "world context flag must be zero or one"};
  }
  reader.RequireEnd();
  return {.value = value != 0U};
}

WorldContextSignalRequest DecodeWorldContextSignalRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto mode = reader.ReadU32();
  if (mode > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "world context signal mode must be zero or one"};
  }
  reader.RequireEnd();
  return {.mode = mode};
}

std::vector<std::uint8_t> EncodeSoloPartyResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateIdentity(snapshot);
  const auto player_id =
      CheckedU32(snapshot.player_id.Value(),
                 "party player ID exceeds legacy protocol range");
  PacketWriter writer;
  writer.WriteU32(2U);
  writer.WriteU32(1U);
  writer.WriteU32(player_id);
  writer.WriteUtf16(Utf8ToUtf16(snapshot.nickname));
  writer.WriteU32(std::numeric_limits<std::uint32_t>::max());
  writer.WriteU32(std::numeric_limits<std::uint32_t>::max());
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteU32(1U);
  const auto equipment = EncodeEquippedItemsResponse(snapshot);
  writer.WriteBytes(equipment);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeMenuStateResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateIdentity(snapshot);
  SceneConfiguration empty_configuration;
  empty_configuration.mode_id = 0U;
  empty_configuration.field_316 = 0U;
  empty_configuration.field_319 = 0U;
  empty_configuration.field_320 = 0U;
  empty_configuration.field_408 = 0U;
  SceneSessionSnapshot session{
      .scene = SceneInfo{.scene_id = 1U,
                         .kind = 0U,
                         .name = "Local Lobby",
                         .occupants = 0U,
                         .observers = 0U,
                         .capacity = 0U,
                         .locked = 0U,
                         .field_40 = {},
                         .scene_type = 1U,
                         .configuration = std::move(empty_configuration)},
      .host_slot = 0U,
      .selected_slot = 0U,
      .slots = {{.slot = 0U,
                 .team = 0U,
                 .occupied = true,
                 .locked = false,
                 .ready = false,
                 .player = snapshot}},
      .include_equipment_records = false};
  return EncodeSceneSessionResponse(session);
}

std::vector<std::uint8_t> EncodeSinglePlayClearsResponse(
    std::span<const application::SinglePlayClearView> records) {
  if (records.size() > 24U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "too many SinglePlay clear records"};
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(records.size()));
  std::uint32_t previous = 0U;
  for (const auto& record : records) {
    if (record.stage_key <= previous || record.stage_key > 28U ||
        record.stage_key % 10U == 0U || record.stage_key % 10U > 8U)
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid SinglePlay stage record"};
    previous = record.stage_key;
    writer.WriteU32(record.stage_key);
    writer.WriteU32(0U);  // No recovered authoritative NPC/count metric.
    writer.WriteU32(record.observed_seconds);
  }
  return writer.Release();
}

}  // namespace bf::protocol
