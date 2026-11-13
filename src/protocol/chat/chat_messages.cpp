#include "protocol/chat/chat_messages.h"

#include <cstddef>

#include "features/chat/megaphone_event.h"
#include "features/chat/user_message.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kMaximumChatSenderCodeUnits = 32U;

}  // namespace

MegaphoneRequest DecodeMegaphoneRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  MegaphoneRequest request{
      .type = reader.ReadU32(),
      .text = reader.ReadUtf16(domain::kMaximumUserMessageCodeUnits),
      .selection = reader.ReadU32(),
      .client_color = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeMegaphoneResponse(std::u16string_view sender,
                                                  std::u16string_view text,
                                                  std::uint32_t item) {
  const auto definition = domain::GetMegaphoneDefinition(item);
  if (!definition) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unverified megaphone item"};
  }
  if (sender.empty() || sender.size() > kMaximumChatSenderCodeUnits ||
      text.empty() || text.size() > domain::kMaximumUserMessageCodeUnits) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "megaphone text exceeds native limits"};
  }
  PacketWriter writer;
  writer.WriteI16(-3);
  writer.WriteUtf16(sender);
  writer.WriteU32(definition->type);
  writer.WriteU32(0U);
  writer.WriteUtf16(u"");
  writer.WriteUtf16(text);
  writer.WriteU32(definition->color);
  writer.WriteU32(item);
  writer.WriteU32(0U);
  return writer.Release();
}

NearbyChatRequest DecodeNearbyChatRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NearbyChatRequest request{
      .channel = reader.ReadU32(),
      .text = reader.ReadUtf16(domain::kMaximumUserMessageCodeUnits)};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeNearbyChatResponse(
    const NearbyChatResponse& response) {
  if ((response.sender_slot < 0 && response.sender_slot != -3) ||
      response.sender_slot > 255) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "nearby chat sender slot exceeds published range"};
  }
  if (response.sender_name.size() > kMaximumChatSenderCodeUnits ||
      response.text.size() > domain::kMaximumUserMessageCodeUnits) {
    throw ProtocolError{
        ProtocolErrorCode::kLengthLimit,
        "nearby chat sender or message exceeds published limit"};
  }

  PacketWriter writer;
  writer.WriteI16(response.sender_slot);
  writer.WriteUtf16(response.sender_name);
  writer.WriteU32(response.team_only ? 1U : 0U);
  writer.WriteU32(0U);
  // B480C0 only invokes GameStage bubbles/keyword motions when this tag is "e".
  // The following string remains the exact user text, including its case.
  writer.WriteUtf16(response.sender_slot >= 0 ? u"e" : u"");
  writer.WriteUtf16(response.text);
  writer.WriteU32(response.color);
  writer.WriteU32(response.color);
  writer.WriteU32(0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCleanChatEnvelope(
    std::uint64_t sender, bool punished, bool bypass,
    std::span<const std::uint8_t> native_body) {
  if (sender == 0U || sender > 0xffffffffU || native_body.size() < 6U ||
      native_body.size() > 4088U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid clean chat metadata"};
  }
  PacketWriter writer;
  writer.WriteU8(1U);
  writer.WriteU8(punished ? 1U : 0U);
  writer.WriteU8(bypass ? 1U : 0U);
  writer.WriteU8(0U);
  writer.WriteU32(static_cast<std::uint32_t>(sender));
  writer.WriteBytes(native_body);
  return writer.Release();
}

}  // namespace bf::protocol
