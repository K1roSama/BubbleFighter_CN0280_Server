#include "protocol/social/social_messages.h"

#include <array>
#include <limits>
#include <string_view>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kMaximumSocialActionCodeUnits = 16U;
constexpr std::size_t kMaximumSocialAccountCodeUnits = 32U;
constexpr std::size_t kMaximumSocialMessageCodeUnits = 256U;
constexpr std::size_t kMaximumLegacyLabelCodeUnits = 64U;

struct SocialActionName final {
  std::u16string_view name;
  SocialAction action;
};

constexpr std::array kSocialActions{
    SocialActionName{.name = u"native", .action = SocialAction::kNative},
    SocialActionName{.name = u"list", .action = SocialAction::kList},
    SocialActionName{.name = u"search", .action = SocialAction::kSearch},
    SocialActionName{.name = u"history", .action = SocialAction::kHistory},
    SocialActionName{.name = u"message", .action = SocialAction::kMessage},
    SocialActionName{.name = u"add", .action = SocialAction::kAdd},
    SocialActionName{.name = u"accept", .action = SocialAction::kAccept},
    SocialActionName{.name = u"decline", .action = SocialAction::kDecline},
    SocialActionName{.name = u"remove", .action = SocialAction::kRemove},
    SocialActionName{.name = u"block", .action = SocialAction::kBlock},
    SocialActionName{.name = u"unblock", .action = SocialAction::kUnblock},
};

[[nodiscard]] SocialAction ParseAction(std::u16string_view name) {
  for (const auto& candidate : kSocialActions) {
    if (candidate.name == name) {
      return candidate.action;
    }
  }
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "social action is not published"};
}

[[nodiscard]] std::uint32_t LegacyPlayerId(domain::PlayerId player_id) {
  if (!player_id.IsValid() ||
      player_id.Value() > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "social player ID exceeds legacy protocol range"};
  }
  return static_cast<std::uint32_t>(player_id.Value());
}

void RequireTextLimit(std::u16string_view value, std::size_t maximum,
                      std::string_view message) {
  if (value.size() > maximum) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
}

void WriteAccount(PacketWriter& writer, std::u16string_view value) {
  RequireTextLimit(value, kMaximumSocialAccountCodeUnits,
                   "social account name exceeds limit");
  writer.WriteUtf16(value);
}

void WriteMessage(PacketWriter& writer, std::u16string_view value) {
  RequireTextLimit(value, kMaximumSocialMessageCodeUnits,
                   "social message exceeds limit");
  writer.WriteUtf16(value);
}

}  // namespace

SocialRequest DecodeSocialRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto action_name = reader.ReadUtf16(kMaximumSocialActionCodeUnits);
  SocialRequest request{
      .action = ParseAction(action_name),
      .account_name = reader.ReadUtf16(kMaximumSocialAccountCodeUnits),
      .text = reader.ReadUtf16(kMaximumSocialMessageCodeUnits)};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeSocialRosterResponse(
    std::u16string_view status, std::span<const SocialRosterEntry> entries) {
  RequireTextLimit(status, kMaximumSocialMessageCodeUnits,
                   "social status exceeds limit");
  if (entries.size() > kMaximumSocialRosterEntries) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "social roster exceeds limit"};
  }
  PacketWriter writer;
  writer.WriteUtf16(status);
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    if (entry.relationship_state > 4U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "social relationship state is not published"};
    }
    writer.WriteU32(LegacyPlayerId(entry.player_id));
    WriteAccount(writer, entry.account_name);
    writer.WriteU32(entry.relationship_state);
    writer.WriteU32(entry.online ? 1U : 0U);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeSocialInvitationResponse(
    const SocialInvitationPayload& payload) {
  PacketWriter writer;
  writer.WriteU32(LegacyPlayerId(payload.sender_player_id));
  WriteAccount(writer, payload.sender_account_name);
  WriteMessage(writer, payload.request_message);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeNativePrivateMessageResponse(
    const NativePrivateMessagePayload& payload) {
  PacketWriter writer;
  writer.WriteU32(LegacyPlayerId(payload.peer_player_id));
  WriteAccount(writer, payload.peer_account_name);
  WriteAccount(writer, payload.speaker_account_name);
  WriteMessage(writer, payload.message);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLegacyPrivateMessageResponse(
    const LegacyPrivateMessagePayload& payload) {
  RequireTextLimit(payload.label, kMaximumLegacyLabelCodeUnits,
                   "legacy private label exceeds limit");
  PacketWriter writer;
  writer.WriteUtf16(payload.label);
  WriteMessage(writer, payload.message);
  return writer.Release();
}

}  // namespace bf::protocol
