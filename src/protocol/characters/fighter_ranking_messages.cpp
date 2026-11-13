#include "protocol/characters/fighter_ranking_messages.h"

#include <cstddef>

#include "protocol/codec/limits.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kMaximumRankEntries = 4096U;
constexpr std::size_t kMaximumRankText = 4096U;
constexpr std::size_t kMaximumRankPayload =
    ProtocolLimits{}.maximum_packet_bytes - 5U;

void ValidateCount(std::size_t count) {
  if (count > kMaximumRankEntries) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "fighter ranking list exceeds bound"};
  }
}

}  // namespace

FighterRankingRequest DecodeFighterRankingRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const domain::PlayerId target{reader.ReadU32()};
  reader.RequireEnd();
  if (!target.IsValid()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "fighter ranking target is invalid"};
  }
  return {.target = target};
}

std::vector<std::uint8_t> EncodeFighterRankingResponse(
    const FighterRankingResponse& response) {
  ValidateCount(response.grades.size());
  ValidateCount(response.characters.size());
  ValidateCount(response.maps.size());
  ValidateCount(response.weapons.size());

  std::size_t bytes = 20U + response.characters.size() * 16U +
                      response.maps.size() * 12U +
                      response.weapons.size() * 16U;
  for (const auto& entry : response.grades) {
    if (entry.label.size() > kMaximumRankText) {
      throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                          "fighter ranking text exceeds bound"};
    }
    bytes += 16U + entry.label.size() * 2U;
  }
  if (bytes > kMaximumRankPayload) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "fighter ranking payload exceeds bound"};
  }

  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(response.grades.size()));
  for (const auto& entry : response.grades) {
    writer.WriteU32(entry.grade_id);
    writer.WriteI32(entry.rank_value);
    writer.WriteUtf16(entry.label);
    writer.WriteU32(entry.field_36);
  }
  writer.WriteU32(static_cast<std::uint32_t>(response.characters.size()));
  for (const auto& entry : response.characters) {
    writer.WriteU32(entry.mode_id);
    writer.WriteU32(entry.map_id);
    writer.WriteU32(entry.character_type);
    writer.WriteI32(entry.rank_value);
  }
  writer.WriteU32(static_cast<std::uint32_t>(response.maps.size()));
  for (const auto& entry : response.maps) {
    writer.WriteU32(entry.mode_id);
    writer.WriteU32(entry.map_id);
    writer.WriteI32(entry.rank_value);
  }
  writer.WriteU32(static_cast<std::uint32_t>(response.weapons.size()));
  for (const auto& entry : response.weapons) {
    writer.WriteU32(entry.mode_id);
    writer.WriteU32(entry.map_id);
    writer.WriteU32(entry.weapon_category);
    writer.WriteI32(entry.rank_value);
  }
  writer.WriteU32(response.result);
  return writer.Release();
}

}  // namespace bf::protocol
