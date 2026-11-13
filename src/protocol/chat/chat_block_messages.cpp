#include "protocol/chat/chat_block_messages.h"

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
application::ChatBlockTarget DecodeChatBlockTarget(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto kind = reader.ReadU8();
  if (kind != 0x3aU && kind != 0xbaU)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid chat block identity marker"};
  application::ChatBlockTarget result;
  if (kind == 0x3aU) {
    result.player_id = reader.ReadU32();
    if (result.player_id == 0U)
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "invalid chat block identity"};
  }
  const auto name = reader.ReadUtf16(32U);
  if (name.empty() || name.find(u'\0') != std::u16string::npos)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid chat block name"};
  result.nickname = Utf16ToUtf8(name);
  reader.RequireEnd();
  return result;
}

namespace {
void WriteEntry(PacketWriter& writer,
                const application::ChatBlockEntry& entry) {
  const auto name = Utf8ToUtf16(entry.nickname);
  if (entry.player_id == 0U || name.empty() || name.size() > 32U ||
      name.find(u'\0') != std::u16string::npos)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid stored chat block"};
  writer.WriteU32(entry.player_id);
  writer.WriteUtf16(name);
}
}  // namespace

std::vector<std::uint8_t> EncodeChatBlockList(
    std::span<const application::ChatBlockEntry> entries) {
  if (entries.size() > 50U)
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "chat block capacity exceeded"};
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) WriteEntry(writer, entry);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeChatBlockAdded(
    const application::ChatBlockEntry& entry) {
  PacketWriter writer;
  writer.WriteU8(0x3aU);
  WriteEntry(writer, entry);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeChatBlockRemoved(
    const application::ChatBlockEntry& entry) {
  PacketWriter writer;
  WriteEntry(writer, entry);
  return writer.Release();
}
}  // namespace bf::protocol
