#include "protocol/rooms/room_messages.h"

#include "protocol/battle/match_transition_messages.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {

bool ValidateRoomControlMessage(std::uint16_t opcode,
                                std::span<const std::uint8_t> body) {
  switch (opcode) {
    case 503U:
      static_cast<void>(DecodeStartMatchRequest(body));
      break;
    case 504U:
      static_cast<void>(DecodeStartRotatingMatchRequest(body));
      break;
    case 506U:
      PacketReader{body}.RequireEnd();
      break;
    case 505U:
      static_cast<void>(DecodeRoomStatusRequest(body));
      break;
    case 508U:
      static_cast<void>(DecodeTransferHostRequest(body));
      break;
    case 509U:
      static_cast<void>(DecodeSeatLockRequest(body));
      break;
    case 513U:
      static_cast<void>(DecodeRoomConfigurationRequest(body));
      break;
    case 514U:
      static_cast<void>(DecodeRotatingMapSelectionRequest(body));
      break;
    case 515U:
      static_cast<void>(DecodeSwitchTeamRequest(body));
      break;
    case 517U:
      static_cast<void>(DecodeRoomLatencyProbeRequest(body));
      break;
    case 518U:
      static_cast<void>(DecodeRoomLatencyReportRequest(body));
      break;
    default:
      return false;
  }
  return true;
}

namespace {

[[nodiscard]] bool ReadBooleanDword(PacketReader& reader) {
  const auto value = reader.ReadU32();
  if (value > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "room boolean DWORD must be zero or one"};
  }
  return value != 0U;
}

[[nodiscard]] std::vector<std::uint8_t> EncodeResult(std::uint32_t result) {
  PacketWriter writer;
  writer.WriteU32(result);
  return writer.Release();
}

}  // namespace

std::u16string DecodeTransferHostRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  auto nickname = reader.ReadUtf16(64U);
  reader.RequireEnd();
  if (nickname.empty()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "host transfer nickname is empty"};
  }
  return nickname;
}

SurvivalQueueRequest DecodeSurvivalQueueRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  SurvivalQueueRequest request{.cancel = ReadBooleanDword(reader),
                               .mode = reader.ReadU32(),
                               .map_key = reader.ReadUtf16(256U)};
  reader.RequireEnd();
  if (request.cancel && (request.mode != 0U || !request.map_key.empty())) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid survival queue cancellation"};
  }
  return request;
}

std::vector<std::uint8_t> EncodeSurvivalQueueResponse(std::uint32_t status,
                                                      std::uint32_t value) {
  PacketWriter writer;
  writer.WriteU32(status);
  if (status <= 2U) {
    writer.WriteU32(value);
  }
  return writer.Release();
}

LeaveRoomRequest DecodeLeaveRoomRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
  return {};
}

RoomStatusRequest DecodeRoomStatusRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const RoomStatusRequest request{.ready = ReadBooleanDword(reader),
                                  .shop_open = ReadBooleanDword(reader),
                                  .inventory_open = ReadBooleanDword(reader)};
  reader.RequireEnd();
  return request;
}

SeatLockRequest DecodeSeatLockRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const SeatLockRequest request{.slot = reader.ReadU32(),
                                .locked = ReadBooleanDword(reader)};
  reader.RequireEnd();
  return request;
}

SwitchTeamRequest DecodeSwitchTeamRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
  return {};
}

RoomLatencyProbeRequest DecodeRoomLatencyProbeRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const RoomLatencyProbeRequest request{.slot = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

MatchLatencyProbe DecodeMatchLatencyProbe(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const MatchLatencyProbe probe{.slot = reader.ReadU32(),
                                .token_ms = reader.ReadU32()};
  reader.RequireEnd();
  return probe;
}

std::vector<std::uint8_t> EncodeMatchLatencyProbe(
    const MatchLatencyProbe& probe) {
  PacketWriter writer;
  writer.WriteU32(probe.slot);
  writer.WriteU32(probe.token_ms);
  return writer.Release();
}

RoomLatencyReportRequest DecodeRoomLatencyReportRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const RoomLatencyReportRequest request{.slot = reader.ReadU32(),
                                         .token = reader.ReadU32(),
                                         .fps = reader.ReadU32(),
                                         .stage = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

RoomContextSignalRequest DecodeRoomContextSignalRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const RoomContextSignalRequest request{.value = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

SceneResourceReportRequest DecodeSceneResourceReportRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  constexpr std::size_t maximum_resource_name_code_units = 128U;
  SceneResourceReportRequest request{
      .resource_name = reader.ReadUtf16(maximum_resource_name_code_units)};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeCreateRoomResponse(std::uint32_t result) {
  return EncodeResult(result);
}

std::vector<std::uint8_t> EncodeJoinRoomResponse(std::uint32_t result) {
  return EncodeResult(result);
}

std::vector<std::uint8_t> EncodeLeaveRoomResponse() { return {}; }

std::vector<std::uint8_t> EncodeRoomSceneUpdateResponse(
    const SceneInfo& scene) {
  PacketWriter writer;
  EncodeSceneInfo(writer, scene);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeRoomHostChangedResponse(std::uint32_t slot) {
  PacketWriter writer;
  writer.WriteU32(slot);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeRoomTeamChangedResponse(
    const RoomTeamChangedResponse& response) {
  if (response.team < 1U || response.team > 16U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "room team update must contain a playing faction"};
  }
  PacketWriter writer;
  writer.WriteU32(response.slot);
  writer.WriteU8(response.team);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeRoomSeatLockChangedResponse(
    const RoomSeatLockChangedResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.slot);
  writer.WriteU32(response.locked ? 1U : 0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeRoomLatencyProbeResponse(std::uint32_t token) {
  PacketWriter writer;
  writer.WriteU32(token);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeRoomLatencyReportResponse(
    const RoomLatencyReportResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.slot);
  writer.WriteU32(response.fps);
  writer.WriteU32(response.stage);
  return writer.Release();
}

}  // namespace bf::protocol
