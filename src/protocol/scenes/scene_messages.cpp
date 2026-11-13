#include "protocol/scenes/scene_messages.h"

#include <limits>
#include <string_view>

#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kMaximumConfigurationItems = 128U;
constexpr std::size_t kMaximumConfigurationTextUnits = 256U;
constexpr std::size_t kMaximumSceneNameUnits = 64U;
constexpr std::size_t kMaximumPasswordUnits = 32U;
constexpr std::size_t kMaximumRoomPageSize = 100U;

[[nodiscard]] std::string ReadText(PacketReader& reader,
                                   std::size_t maximum_code_units) {
  return Utf16ToUtf8(reader.ReadUtf16(maximum_code_units));
}

void WriteText(PacketWriter& writer, std::string_view text,
               std::size_t maximum_code_units) {
  const auto utf16 = Utf8ToUtf16(text);
  if (utf16.size() > maximum_code_units) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "scene UTF-16 field exceeds its code-unit limit"};
  }
  writer.WriteUtf16(utf16);
}

void WriteCount(PacketWriter& writer, std::size_t count, std::size_t maximum) {
  if (count > maximum || count > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "scene vector exceeds its element limit"};
  }
  writer.WriteU32(static_cast<std::uint32_t>(count));
}

}  // namespace

std::vector<std::uint8_t> EncodePlazaUserListResponse(
    std::span<const PlazaUserListEntry> players) {
  PacketWriter writer;
  WriteCount(writer, players.size(), kMaximumPlazaUserList);
  for (const auto& player : players) {
    if (player.nickname.empty() || player.level == 0U || player.level > 1000U)
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid plaza player identity or level"};
    WriteText(writer, player.nickname, 12U);
    writer.WriteU32(player.level);
    writer.WriteU32(player.arena_first);
    writer.WriteU32(player.arena_second);
  }
  return writer.Release();
}

SceneConfiguration DecodeSceneConfiguration(PacketReader& reader) {
  SceneConfiguration result;
  result.mode_id = reader.ReadU32();
  result.submode_id = reader.ReadU32();
  result.field_8 = reader.ReadU32();
  result.field_12 = reader.ReadU32();
  result.field_13 = reader.ReadU32();
  result.field_14 = reader.ReadU32();
  result.score_target = reader.ReadU32();
  result.selected_duration_ms = reader.ReadU32();
  result.field_24 = reader.ReadU32();
  result.field_28 = reader.ReadU32();
  result.field_32 = reader.ReadU32();
  result.family_key = ReadText(reader, kMaximumConfigurationTextUnits);
  result.map_key = ReadText(reader, kMaximumConfigurationTextUnits);
  result.display_name = ReadText(reader, kMaximumConfigurationTextUnits);
  result.auxiliary_label = ReadText(reader, kMaximumConfigurationTextUnits);
  result.map_value = reader.ReadU32();
  for (auto& map : result.rotating_maps) {
    map.family_key = ReadText(reader, kMaximumConfigurationTextUnits);
    map.map_key = ReadText(reader, kMaximumConfigurationTextUnits);
  }
  result.field_280 = reader.ReadU32();
  result.field_284 = reader.ReadU32();
  result.field_288 = reader.ReadU32();

  const auto pair_count = reader.ReadCount(kMaximumConfigurationItems);
  result.field_289.reserve(pair_count);
  for (std::uint32_t index = 0U; index < pair_count; ++index) {
    result.field_289.push_back(
        SceneConfigPair{.first = reader.ReadU32(), .second = reader.ReadU32()});
  }
  result.field_301 = reader.ReadU32();
  const auto named_count = reader.ReadCount(kMaximumConfigurationItems);
  result.field_302.reserve(named_count);
  for (std::uint32_t index = 0U; index < named_count; ++index) {
    result.field_302.push_back(SceneConfigNamedValue{
        .value = reader.ReadU32(),
        .name = ReadText(reader, kMaximumConfigurationTextUnits),
    });
  }

  result.field_314 = reader.ReadU32();
  result.field_315 = reader.ReadU32();
  result.field_316 = reader.ReadU32();
  result.field_317 = reader.ReadU32();
  result.field_318 = reader.ReadU32();
  result.field_319 = reader.ReadU32();
  result.field_320 = reader.ReadU32();
  result.field_324 = reader.ReadU8();
  result.field_328 = reader.ReadU32();
  const auto text_count = reader.ReadCount(kMaximumConfigurationItems);
  result.field_332.reserve(text_count);
  for (std::uint32_t index = 0U; index < text_count; ++index) {
    result.field_332.push_back(
        ReadText(reader, kMaximumConfigurationTextUnits));
  }
  result.field_344 = reader.ReadU32();
  result.field_348 = reader.ReadU32();
  result.field_352 = reader.ReadU32();
  result.field_356 = reader.ReadU32();
  result.field_360 = ReadText(reader, kMaximumConfigurationTextUnits);
  result.field_384 = ReadText(reader, kMaximumConfigurationTextUnits);
  result.field_408 = reader.ReadU32();
  return result;
}

void EncodeSceneConfiguration(PacketWriter& writer,
                              const SceneConfiguration& configuration) {
  writer.WriteU32(configuration.mode_id);
  writer.WriteU32(configuration.submode_id);
  writer.WriteU32(configuration.field_8);
  writer.WriteU32(configuration.field_12);
  writer.WriteU32(configuration.field_13);
  writer.WriteU32(configuration.field_14);
  writer.WriteU32(configuration.score_target);
  writer.WriteU32(configuration.selected_duration_ms);
  writer.WriteU32(configuration.field_24);
  writer.WriteU32(configuration.field_28);
  writer.WriteU32(configuration.field_32);
  WriteText(writer, configuration.family_key, kMaximumConfigurationTextUnits);
  WriteText(writer, configuration.map_key, kMaximumConfigurationTextUnits);
  WriteText(writer, configuration.display_name, kMaximumConfigurationTextUnits);
  WriteText(writer, configuration.auxiliary_label,
            kMaximumConfigurationTextUnits);
  writer.WriteU32(configuration.map_value);
  for (const auto& map : configuration.rotating_maps) {
    WriteText(writer, map.family_key, kMaximumConfigurationTextUnits);
    WriteText(writer, map.map_key, kMaximumConfigurationTextUnits);
  }
  writer.WriteU32(configuration.field_280);
  writer.WriteU32(configuration.field_284);
  writer.WriteU32(configuration.field_288);

  WriteCount(writer, configuration.field_289.size(),
             kMaximumConfigurationItems);
  for (const auto& pair : configuration.field_289) {
    writer.WriteU32(pair.first);
    writer.WriteU32(pair.second);
  }
  writer.WriteU32(configuration.field_301);
  WriteCount(writer, configuration.field_302.size(),
             kMaximumConfigurationItems);
  for (const auto& named : configuration.field_302) {
    writer.WriteU32(named.value);
    WriteText(writer, named.name, kMaximumConfigurationTextUnits);
  }

  writer.WriteU32(configuration.field_314);
  writer.WriteU32(configuration.field_315);
  writer.WriteU32(configuration.field_316);
  writer.WriteU32(configuration.field_317);
  writer.WriteU32(configuration.field_318);
  writer.WriteU32(configuration.field_319);
  writer.WriteU32(configuration.field_320);
  writer.WriteU8(configuration.field_324);
  writer.WriteU32(configuration.field_328);
  WriteCount(writer, configuration.field_332.size(),
             kMaximumConfigurationItems);
  for (const auto& text : configuration.field_332) {
    WriteText(writer, text, kMaximumConfigurationTextUnits);
  }
  writer.WriteU32(configuration.field_344);
  writer.WriteU32(configuration.field_348);
  writer.WriteU32(configuration.field_352);
  writer.WriteU32(configuration.field_356);
  WriteText(writer, configuration.field_360, kMaximumConfigurationTextUnits);
  WriteText(writer, configuration.field_384, kMaximumConfigurationTextUnits);
  writer.WriteU32(configuration.field_408);
}

SceneInfo DecodeSceneInfo(PacketReader& reader) {
  SceneInfo result;
  result.scene_id = reader.ReadU16();
  result.kind = reader.ReadU32();
  result.name = ReadText(reader, kMaximumSceneNameUnits);
  result.occupants = reader.ReadU16();
  result.observers = reader.ReadU16();
  result.capacity = reader.ReadU16();
  result.locked = reader.ReadU32();
  result.field_40 = ReadText(reader, kMaximumConfigurationTextUnits);
  result.scene_type = reader.ReadU32();
  result.configuration = DecodeSceneConfiguration(reader);
  result.field_68 = reader.ReadU32();
  result.key = reader.ReadU64();
  result.field_80 = ReadText(reader, kMaximumConfigurationTextUnits);
  result.field_104 = reader.ReadU32();
  result.field_108 = reader.ReadU32();
  result.field_112 = reader.ReadU32();
  result.field_116 = reader.ReadU32();
  return result;
}

void EncodeSceneInfo(PacketWriter& writer, const SceneInfo& scene) {
  writer.WriteU16(scene.scene_id);
  writer.WriteU32(scene.kind);
  WriteText(writer, scene.name, kMaximumSceneNameUnits);
  writer.WriteU16(scene.occupants);
  writer.WriteU16(scene.observers);
  writer.WriteU16(scene.capacity);
  writer.WriteU32(scene.locked);
  WriteText(writer, scene.field_40, kMaximumConfigurationTextUnits);
  writer.WriteU32(scene.scene_type);
  EncodeSceneConfiguration(writer, scene.configuration);
  writer.WriteU32(scene.field_68);
  writer.WriteU64(scene.key);
  WriteText(writer, scene.field_80, kMaximumConfigurationTextUnits);
  writer.WriteU32(scene.field_104);
  writer.WriteU32(scene.field_108);
  writer.WriteU32(scene.field_112);
  writer.WriteU32(scene.field_116);
}

RoomListRequest DecodeRoomListRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const RoomListRequest result{.page = reader.ReadU16(),
                               .page_size = reader.ReadU16()};
  reader.RequireEnd();
  if (result.page_size == 0U || result.page_size > kMaximumRoomPageSize) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "room page size is outside the accepted range"};
  }
  return result;
}

CreateRoomRequest DecodeCreateRoomRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  CreateRoomRequest result{.requested_scene = DecodeSceneInfo(reader),
                           .password = ReadText(reader, kMaximumPasswordUnits)};
  reader.RequireEnd();
  return result;
}

JoinRoomRequest DecodeJoinRoomRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  JoinRoomRequest result{.scene_id = reader.ReadU32(),
                         .password = ReadText(reader, kMaximumPasswordUnits),
                         .entry_option = reader.ReadU32()};
  reader.RequireEnd();
  return result;
}

std::vector<std::uint8_t> EncodeRoomListResponse(
    std::uint32_t page, bool has_next, std::span<const SceneInfo> scenes) {
  if (scenes.size() > kMaximumRoomPageSize) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "room list response exceeds maximum page size"};
  }
  PacketWriter writer;
  writer.WriteU32(page);
  writer.WriteU32(has_next ? 1U : 0U);
  writer.WriteU32(0U);
  WriteCount(writer, scenes.size(), kMaximumRoomPageSize);
  for (const auto& scene : scenes) {
    EncodeSceneInfo(writer, scene);
  }
  return writer.Release();
}

QuickJoinRequest DecodeQuickJoinRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  QuickJoinRequest result{.mode_id = reader.ReadU32(),
                          .size_option = reader.ReadU32()};
  if (result.size_option < 1U || result.size_option > 3U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid quick join size option"};
  }
  const auto count = reader.ReadCount(11U);
  result.history.reserve(count);
  for (std::uint32_t i = 0U; i < count; ++i) {
    result.history.push_back(
        {reader.ReadU32(), reader.ReadU32(), reader.ReadU32()});
  }
  reader.RequireEnd();
  return result;
}

std::vector<std::uint8_t> EncodeQuickJoinResponse(std::uint32_t result) {
  PacketWriter writer;
  writer.WriteU32(result);
  writer.WriteU32(0U);
  writer.WriteUtf16(u"");
  return writer.Release();
}

}  // namespace bf::protocol
