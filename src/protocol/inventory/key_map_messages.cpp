#include "protocol/inventory/key_map_messages.h"

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
application::KeyMapSettings DecodeKeyMapRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto reset = reader.ReadU32();
  const auto count = reader.ReadU32();
  if (reset > 1U || count > application::kMaximumKeyBindings) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid native key map header"};
  }
  application::KeyMapSettings result{.reset = reset != 0U};
  for (std::uint32_t index = 0U; index < count; ++index) {
    auto action = Utf16ToUtf8(reader.ReadUtf16(32U));
    result.bindings.push_back(
        {.action = std::move(action), .key = reader.ReadU32()});
  }
  reader.RequireEnd();
  if (!application::ValidKeyMap(result)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid native key binding"};
  }
  return result;
}

std::vector<std::uint8_t> EncodeKeyMapResponse(
    bool apply, std::span<const application::KeyBinding> bindings) {
  PacketWriter writer;
  writer.WriteU32(apply ? 1U : 0U);
  writer.WriteU32(static_cast<std::uint32_t>(bindings.size()));
  for (const auto& binding : bindings) {
    writer.WriteUtf16(Utf8ToUtf16(binding.action));
    writer.WriteU32(binding.key);
  }
  return writer.Release();
}
}  // namespace bf::protocol
