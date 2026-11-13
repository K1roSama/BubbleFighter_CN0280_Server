#include "protocol/lobby/lobby_minigame_messages.h"

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::uint32_t kFortuneCategories =
    6U;  // FortunePiniPaper: level, weapon, money, win rate, couple, clan.

bool ReadBoolDword(PacketReader& reader, std::string_view what) {
  const auto value = reader.ReadU32();
  if (value > 1U) throw ProtocolError{ProtocolErrorCode::kInvalidValue, what};
  return value == 1U;
}

LobbyFishingTerms ReadFishingTerms(PacketReader& reader) {
  LobbyFishingTerms terms;
  terms.premium_rod = ReadBoolDword(reader, "fishing rod flag is not boolean");
  terms.use_free_pass =
      ReadBoolDword(reader, "fishing free-pass flag is not boolean");
  terms.base_cost = reader.ReadU16();
  terms.completed_count = reader.ReadU16();
  return terms;
}

void WriteHeader(PacketWriter& writer, std::uint16_t sub) {
  writer.WriteU16(sub);
}

}  // namespace

bool ValidFortunePaper(std::uint64_t paper) noexcept {
  // 643160: four 12-bit cells at shifts 0/12/24/36, one category for the whole
  // paper, and a random 0..0x8000 salt in the top word.
  if (paper == 0U || (paper >> 48U) > 0x8000U) return false;
  const auto category = static_cast<std::uint32_t>((paper >> 8U) & 0xFU);
  if (category >= kFortuneCategories) return false;
  for (unsigned shift = 0U; shift < 48U; shift += 12U) {
    if (((paper >> (shift + 8U)) & 0xFU) != category) return false;
  }
  return true;
}

std::vector<std::uint8_t> EncodeLobbyMinigameConfig(
    const LobbyMinigameConfig& config) {
  PacketWriter writer;
  const auto& basketball = config.basketball;
  writer.WriteU32(basketball.enabled ? 1U : 0U);
  writer.WriteU16(basketball.cost_lucci);
  writer.WriteU16(basketball.gauge_lead_ms);
  writer.WriteU16(basketball.hit_window_ms);
  writer.WriteU16(basketball.round_end_ms);
  const auto& fishing = config.fishing;
  writer.WriteU32(fishing.enabled ? 1U : 0U);
  writer.WriteU16(fishing.cost_lucci);
  writer.WriteU16(fishing.alternate_cost_lucci);
  writer.WriteU16(fishing.cast_ms);
  writer.WriteU16(fishing.bite_min_seconds);
  writer.WriteU16(fishing.bite_max_seconds);
  writer.WriteU16(fishing.hook_window_seconds);
  writer.WriteU32(fishing.premium_rod_item);
  writer.WriteU32(fishing.free_pass_item);
  writer.WriteU32(config.fortune.enabled ? 1U : 0U);
  writer.WriteU16(config.fortune.daily_cost_lucci);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLobbyMinigameRecord(
    std::uint16_t fishing_count, std::uint64_t fortune_paper) {
  PacketWriter writer;
  writer.WriteU16(fishing_count);
  writer.WriteU64(fortune_paper);
  return writer.Release();
}

LobbyMinigameRequest DecodeLobbyMinigameRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LobbyMinigameRequest request;
  switch (reader.ReadU16()) {
    case 1U:
      request.kind = LobbyMinigameRequestKind::kShootBasketball;
      request.displayed_cost = reader.ReadU16();
      request.hit = ReadBoolDword(reader, "basketball hit flag is not boolean");
      if (reader.ReadU32() != 0U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "basketball reserved field is set"};
      break;
    case 10U:
      request.kind = LobbyMinigameRequestKind::kStartFishing;
      request.fishing = ReadFishingTerms(reader);
      break;
    case 11U:
      request.kind = LobbyMinigameRequestKind::kLiftFishingRod;
      request.fishing = ReadFishingTerms(reader);
      request.hooked =
          ReadBoolDword(reader, "fishing hook flag is not boolean");
      break;
    case 12U:
      request.kind = LobbyMinigameRequestKind::kFortune;
      request.displayed_cost = reader.ReadU16();
      request.fortune_paper = reader.ReadU64();
      if (reader.ReadU32() != 0U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "fortune reserved field is set"};
      if (!ValidFortunePaper(request.fortune_paper))
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "fortune paper is malformed"};
      break;
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "unknown lobby minigame request"};
  }
  reader.RequireEnd();
  return request;
}

LobbyMinigameSync DecodeLobbyMinigameSync(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LobbyMinigameSync sync;
  const auto sub = reader.ReadU16();
  sync.value = reader.ReadU32();
  reader.RequireEnd();
  switch (sub) {
    case 100U:
      // 67F0C0/66CEF0: 0 miss, 1 hit, 2 clean shot.
      if (sync.value > 2U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "basketball animation is invalid"};
      sync.kind = LobbyMinigameSyncKind::kShootFire;
      break;
    case 101U:
      if (sync.value > 0xFFFFU)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "minigame state is out of range"};
      sync.kind = LobbyMinigameSyncKind::kChangeState;
      break;
    case 200U:
    case 201U:
      // 66CFC0 and 66D050 consume the low byte (gamer+321 rod visual).
      if (sync.value > 0xFFU)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "fishing visual is not a byte"};
      sync.kind = sub == 200U ? LobbyMinigameSyncKind::kCastFishing
                              : LobbyMinigameSyncKind::kLiftFishing;
      break;
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "unknown lobby minigame sync"};
  }
  return sync;
}

std::vector<std::uint8_t> EncodeBasketballReply(std::uint32_t result,
                                                std::uint32_t lucci,
                                                std::uint32_t item) {
  PacketWriter writer;
  WriteHeader(writer, 1U);
  writer.WriteU32(result);
  if (result >= 10U) writer.WriteU32(lucci);
  if (result == 12U) writer.WriteU32(item);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeStartFishingReply(std::uint32_t result) {
  PacketWriter writer;
  WriteHeader(writer, 10U);
  writer.WriteU32(result);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLiftFishingReply(
    std::uint32_t result, std::int32_t paid, std::uint32_t lucci,
    std::uint16_t count, std::span<const std::uint32_t> items) {
  const auto expected = result == 11U || result == 13U ? 1U
                        : result == 12U                ? 2U
                                                       : 0U;
  if (result >= 10U && items.size() != expected)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "fishing reward count does not match result"};
  PacketWriter writer;
  WriteHeader(writer, 11U);
  writer.WriteU32(result);
  if (result >= 10U) {
    writer.WriteI32(paid);
    writer.WriteU32(lucci);
    writer.WriteU16(count);
    for (const auto item : items) writer.WriteU32(item);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeFortuneReply(std::uint32_t result,
                                             std::uint64_t paper,
                                             std::uint32_t lucci,
                                             std::uint32_t item) {
  PacketWriter writer;
  WriteHeader(writer, 12U);
  writer.WriteU32(result);
  writer.WriteU64(paper);
  if (result == 10U) {
    writer.WriteU32(lucci);
    writer.WriteU32(item);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLobbyMinigameSync(
    std::uint32_t player, const LobbyMinigameSync& sync) {
  PacketWriter writer;
  writer.WriteU32(player);
  writer.WriteU16(static_cast<std::uint16_t>(sync.kind));
  writer.WriteU32(sync.value);
  return writer.Release();
}

}  // namespace bf::protocol
