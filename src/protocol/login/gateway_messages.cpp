#include "protocol/login/gateway_messages.h"

#include <cmath>
#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kLoginTextCodeUnits = 4096U;

void RequireU32Count(std::size_t count, const char* safe_message) {
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, safe_message};
  }
}

void WriteUtf16Vector(PacketWriter& writer,
                      const std::vector<std::u16string>& values) {
  RequireU32Count(values.size(), "outgoing UTF-16 vector exceeds uint32 count");
  writer.WriteU32(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) {
    writer.WriteUtf16(value);
  }
}

}  // namespace

PatchHandshakeRequest DecodePatchHandshakeRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  PatchHandshakeRequest request{};
  request.build = reader.ReadU32();
  for (auto& text : request.first_texts) {
    text = reader.ReadUtf16(kMaximumHandshakeTextCodeUnits);
  }
  request.reserved = reader.ReadU32();
  for (auto& text : request.last_texts) {
    text = reader.ReadUtf16(kMaximumHandshakeTextCodeUnits);
  }
  reader.RequireEnd();
  return request;
}

EnvironmentReport DecodeEnvironmentReport(std::span<const std::uint8_t> body,
                                          std::size_t maximum_code_units) {
  PacketReader reader{body};
  EnvironmentReport report{.text = reader.ReadUtf16(maximum_code_units)};
  reader.RequireEnd();
  return report;
}

LoginRequest DecodeLoginRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LoginRequest request{};
  for (auto& text : request.texts) {
    text = reader.ReadUtf16(kLoginTextCodeUnits);
  }
  const auto count = reader.ReadCount(kMaximumLoginAddresses);
  request.ipv4_addresses.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    request.ipv4_addresses.push_back(reader.ReadU32());
  }
  request.client_key = reader.ReadU32();
  request.mode = reader.ReadU32();
  reader.RequireEnd();
  return request;
}

PostLoginBootstrapRequest DecodePostLoginBootstrapRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  PostLoginBootstrapRequest request{};
  request.first_value = reader.ReadU32();
  request.second_value = reader.ReadU32();
  const auto count = reader.ReadCount(kMaximumBootstrapValues);
  request.values.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    request.values.push_back(reader.ReadU32());
  }
  reader.RequireEnd();
  return request;
}

GatewayExitRequest DecodeGatewayExitRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  GatewayExitRequest request{.main_app_state_is_one = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

void DecodeCharacterCreationCenterRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

void DecodeCharacterCreationCenterAck(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

void DecodeClientStatusReadyRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

void DecodeClientStatusUpdate(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

void DecodeMenuAuxiliaryReadyReport(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
}

LegacyMenuTextReport DecodeLegacyMenuTextReport(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LegacyMenuTextReport report{};
  report.first_value = reader.ReadU32();
  report.text = reader.ReadUtf16(kMaximumLegacyMenuTextCodeUnits);
  report.trailing_value = reader.ReadU32();
  reader.RequireEnd();
  return report;
}

LegacyMenuStateReport DecodeLegacyMenuStateReport(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LegacyMenuStateReport report{};
  for (auto& value : report.values) {
    value = reader.ReadU32();
  }
  reader.RequireEnd();
  return report;
}

TutorialStartReport DecodeTutorialStartReport(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  TutorialStartReport report{};
  for (auto& value : report.values) {
    value = reader.ReadU32();
  }
  report.kind = reader.ReadU8();
  report.relation_key = reader.ReadUtf16(kMaximumTutorialTextCodeUnits);
  report.display_text = reader.ReadUtf16(kMaximumTutorialTextCodeUnits);
  report.mission_key = reader.ReadUtf16(kMaximumTutorialTextCodeUnits);
  report.trailing_value = reader.ReadU32();
  reader.RequireEnd();
  return report;
}

NicknameCheckRequest DecodeNicknameCheckRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NicknameCheckRequest request{.nickname = reader.ReadUtf16(32U)};
  reader.RequireEnd();
  return request;
}

CreateCharacterRequest DecodeCreateCharacterRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  CreateCharacterRequest request{};
  request.nickname = reader.ReadUtf16(32U);
  request.fighter_id = reader.ReadU32();
  request.reserved = reader.ReadU32();
  request.extra_text = reader.ReadUtf16(256U);
  request.region = reader.ReadU32();
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodePatchHandshakeResponse(
    const PatchHandshakeResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.needs_patch ? 1U : 0U);
  writer.WriteUtf16(response.patch_url);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeProcessProbeResponse(
    const ProcessProbeResponse& response) {
  PacketWriter writer;
  for (const auto& group : response.groups) {
    WriteUtf16Vector(writer, group);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeGatewayListResponse(
    std::span<const GatewayEndpoint> endpoints) {
  RequireU32Count(endpoints.size(),
                  "outgoing gateway vector exceeds uint32 count");
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(endpoints.size()));
  for (const auto& endpoint : endpoints) {
    writer.WriteU32(endpoint.first_value);
    writer.WriteU32(endpoint.second_value);
    writer.WriteU32(endpoint.third_value);
    writer.WriteBytes(endpoint.ipv4_address);
    writer.WriteU16(endpoint.port);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCharacterCreationRequired() { return {}; }

std::vector<std::uint8_t> EncodeCharacterCreationCenterResponse(
    const CharacterCreationCenterResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.result);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeNicknameCheckResponse(
    const NicknameCheckResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.result);
  writer.WriteUtf16(response.nickname);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLoginSuccessResponse(
    const LoginSuccessResponse& response) {
  if (response.initial_fighter_id == 0U ||
      !std::isfinite(response.server_date_ole)) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidFrame,
        "login success requires a fighter and finite server date"};
  }
  PacketWriter writer;
  writer.WriteU32(0U);
  writer.WriteUtf16(response.account_name);
  writer.WriteU32(response.player_id);
  writer.WriteUtf16(response.nickname);
  writer.WriteU32(response.initial_fighter_id);
  for (const auto value : response.reserved) {
    writer.WriteU32(value);
  }
  writer.WriteF64(response.server_date_ole);
  writer.WriteU32(response.main_reserved);
  writer.WriteU32(response.national_result);
  if (response.national_result == 0U) {
    writer.WriteUtf16(response.national_message);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLoginFailureResponse(
    const LoginFailureResponse& response) {
  if (response.result == 0U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                        "login failure result must be non-zero"};
  }
  PacketWriter writer;
  writer.WriteU32(response.result);
  writer.WriteU32(response.detail);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeNoticeResponse(std::u16string_view message) {
  PacketWriter writer;
  writer.WriteU8(0U);
  writer.WriteUtf16(message);
  return writer.Release();
}

double UnixSecondsToOleDate(double unix_seconds) noexcept {
  constexpr double k_seconds_per_day = 86'400.0;
  constexpr double k_unix_epoch_ole_date = 25'569.0;
  return unix_seconds / k_seconds_per_day + k_unix_epoch_ole_date;
}

}  // namespace bf::protocol
