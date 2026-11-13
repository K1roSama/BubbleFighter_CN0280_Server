#include "protocol/battle/native_progression_messages.h"

#include "protocol/codec/packet_io.h"
#include "protocol/inventory/inventory_messages.h"

namespace bf::protocol {

NativeTitleClaimRequest DecodeNativeTitleClaimRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NativeTitleClaimRequest request{.self = domain::PlayerId{reader.ReadU32()},
                                  .title_id = reader.ReadU32()};
  reader.RequireEnd();
  if (!request.self.IsValid() || request.title_id == 0U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "native title claim requires self and title"};
  }
  return request;
}

NativeTitleSetRequest DecodeNativeTitleSetRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NativeTitleSetRequest request{.self = domain::PlayerId{reader.ReadU32()},
                                .action_flag = reader.ReadU32(),
                                .title_id = reader.ReadU32()};
  reader.RequireEnd();
  if (!request.self.IsValid() || request.title_id == 0U ||
      request.action_flag > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "native title set fields are invalid"};
  }
  return request;
}

std::vector<std::uint8_t> EncodeNativeTitleSetUnavailable() {
  return EncodeNativeTitleSetResponse(-1);
}

std::vector<std::uint8_t> EncodeNativeTitleSetResponse(std::int32_t result) {
  // B6E3F0 consumes all three DWORDs even in its generic failure branch.
  // It does not establish the semantic meaning of the last two DWORDs.
  PacketWriter writer;
  writer.WriteI32(result);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeNativeMedalSnapshot(
    const application::PlayerMenuSnapshot& snapshot) {
  if (!snapshot.medal_progress) {
    return EncodeFighterTitleFailure(snapshot.player_id,
                                     FighterTitleFailure::kUnavailable);
  }
  if (snapshot.medal_progress->size() > 4096U) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "native medal ledger exceeds bound"};
  }
  std::vector<FighterMedalProgress> medals;
  medals.reserve(snapshot.medal_progress->size());
  bool unavailable = false;
  std::uint32_t previous_key = 0U;
  for (const auto& medal : *snapshot.medal_progress) {
    if (medal.count == 0U || medal.medal_key <= previous_key ||
        medal.medal_key % 10'000U == 0U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidFrame,
                          "invalid native medal ledger row"};
    }
    previous_key = medal.medal_key;
    if (medal.count > 0x7FFFFFFFU) {
      unavailable = true;
    } else {
      medals.push_back({.medal_id = medal.medal_key,
                        .count = static_cast<std::uint32_t>(medal.count)});
    }
  }
  // 合法64位账本不能截断为客户端有符号DWORD，也不能阻断无关菜单资料。
  if (unavailable) {
    return EncodeFighterTitleFailure(snapshot.player_id,
                                     FighterTitleFailure::kUnavailable);
  }
  return EncodeFighterTitleResponse(snapshot.player_id, medals);
}

}  // namespace bf::protocol
