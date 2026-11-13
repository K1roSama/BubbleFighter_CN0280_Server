#include "protocol/rooms/user_host_messages.h"

#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

void ValidateAmount(std::uint32_t amount) {
  if (amount >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "user-host prize exceeds the native signed integer range"};
  }
}

void ValidateAction(UserHostSettingAction action) {
  if (static_cast<std::uint32_t>(action) > 3U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown user-host setting action"};
  }
}

void ValidateRange(std::uint32_t minimum, std::uint32_t maximum) {
  ValidateAmount(maximum);
  if (minimum > maximum) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "inverted user-host prize range"};
  }
}

}  // namespace

UserHostSettingRequest DecodeUserHostSettingRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto action = static_cast<UserHostSettingAction>(reader.ReadU32());
  const auto amount = reader.ReadU32();
  reader.RequireEnd();
  ValidateAction(action);
  ValidateAmount(amount);
  if (action != UserHostSettingAction::kSetAmount && amount != 0U) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "user-host query or edit notification contains an amount"};
  }
  return {.action = action, .amount = amount};
}

void ValidateUserHostStartRequest(std::span<const std::uint8_t> body) {
  PacketReader{body}.RequireEnd();
}

std::vector<std::uint8_t> EncodeUserHostSettingResponse(
    const UserHostSettingResponse& response) {
  ValidateAction(response.action);
  ValidateAmount(response.amount);
  ValidateRange(response.minimum, response.maximum);
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(response.action));
  writer.WriteU32(response.amount);
  writer.WriteU32(response.minimum);
  writer.WriteU32(response.maximum);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeUserHostDefaultSetting(std::uint32_t minimum,
                                                       std::uint32_t maximum) {
  ValidateRange(minimum, maximum);
  PacketWriter writer;
  writer.WriteU32(minimum);
  writer.WriteU32(maximum);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeUserHostStartResponse(
    UserHostStartStatus status) {
  const auto value = static_cast<std::int32_t>(status);
  if (value > 0 || value < -5) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown user-host start status"};
  }
  PacketWriter writer;
  writer.WriteI32(value);
  return writer.Release();
}

}  // namespace bf::protocol
