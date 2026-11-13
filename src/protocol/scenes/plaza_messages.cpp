#include "protocol/scenes/plaza_messages.h"

#include <cmath>
#include <limits>
#include <string_view>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr float kMaximumSceneCoordinate = 10'000'000.0F;

void RequireValidMovement(const SceneMovementRequest& movement) {
  const auto valid_scalar = [](float value) {
    return std::isfinite(value) && std::abs(value) <= kMaximumSceneCoordinate;
  };
  const auto valid_axis = [](std::int8_t value) {
    return value >= -1 && value <= 1;
  };
  if (!valid_scalar(movement.x) || !valid_scalar(movement.y) ||
      !valid_scalar(movement.z) || !valid_scalar(movement.yaw) ||
      !valid_axis(movement.forward) || !valid_axis(movement.sideways)) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "scene movement contains an invalid coordinate or direction"};
  }
}

void EncodeMovement(PacketWriter& writer,
                    const SceneMovementRequest& movement) {
  RequireValidMovement(movement);
  writer.WriteU32(movement.sequence);
  writer.WriteF32(movement.x);
  writer.WriteF32(movement.y);
  writer.WriteF32(movement.z);
  writer.WriteI8(movement.forward);
  writer.WriteI8(movement.sideways);
  writer.WriteF32(movement.yaw);
}

void RequireEmpty(std::span<const std::uint8_t> body,
                  std::string_view message) {
  PacketReader reader{body};
  try {
    reader.RequireEnd();
  } catch (const ProtocolError&) {
    throw ProtocolError{ProtocolErrorCode::kTrailingPayload, message};
  }
}

}  // namespace

ChannelSelectionRequest DecodeChannelSelectionRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  ChannelSelectionRequest request{.channel = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

ReturnToMenuRequest DecodeReturnToMenuRequest(
    std::span<const std::uint8_t> body) {
  RequireEmpty(body, "return-to-menu request has trailing bytes");
  return {};
}

EnterPlazaRequest DecodeEnterPlazaRequest(std::span<const std::uint8_t> body) {
  RequireEmpty(body, "enter-plaza request has trailing bytes");
  return {};
}

SceneMovementRequest DecodeSceneMovementRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  SceneMovementRequest movement{.sequence = reader.ReadU32(),
                                .x = reader.ReadF32(),
                                .y = reader.ReadF32(),
                                .z = reader.ReadF32(),
                                .forward = reader.ReadI8(),
                                .sideways = reader.ReadI8(),
                                .yaw = reader.ReadF32()};
  reader.RequireEnd();
  RequireValidMovement(movement);
  return movement;
}

std::vector<std::uint8_t> EncodeChannelSelectionResponse(
    bool success, std::uint32_t channel, std::uint32_t channel_id) {
  PacketWriter writer;
  writer.WriteU32(success ? 1U : 0U);
  writer.WriteU32(channel);
  writer.WriteU32(channel_id);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeSceneMovementResponse(
    domain::PlayerId player_id, const SceneMovementRequest& movement) {
  if (!player_id.IsValid() ||
      player_id.Value() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "scene movement player ID exceeds legacy protocol range"};
  }
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(player_id.Value()));
  EncodeMovement(writer, movement);
  return writer.Release();
}

}  // namespace bf::protocol
