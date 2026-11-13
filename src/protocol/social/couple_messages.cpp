#include "protocol/social/couple_messages.h"

#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"

namespace bf::protocol {
namespace {
bool Boolean(PacketReader& reader) {
  const auto value = reader.ReadU32();
  if (value > 1U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid couple boolean"};
  return value != 0U;
}

void U32Vector(PacketReader& reader, std::size_t limit) {
  const auto count = reader.ReadCount(limit);
  static_cast<void>(reader.ReadBytes(static_cast<std::size_t>(count) * 4U));
}
}  // namespace

CoupleInviteRequest DecodeCoupleInvite(std::span<const std::uint8_t> body) {
  PacketReader r{body};
  CoupleInviteRequest result{.confirmed = Boolean(r),
                             .target = r.ReadUtf16(32U),
                             .ring = r.ReadU32(),
                             .message = r.ReadUtf16(256U),
                             .equipment_owner = r.ReadU32()};
  // EquippedItemList 7F0130, ItemAttr 6DD6B0, ItemExpiredTime 7F2B40.
  static_cast<void>(r.ReadU32());  // character
  static_cast<void>(r.ReadU32());  // color
  U32Vector(r, 128U);
  static_cast<void>(r.ReadBytes(r.ReadCount(128U)));  // byte vector, not DWORDs
  const auto attributes = r.ReadCount(128U);
  for (std::uint32_t i = 0; i < attributes; ++i) {
    static_cast<void>(r.ReadU32());
    static_cast<void>(r.ReadU32());
    U32Vector(r, 128U);
    static_cast<void>(r.ReadU32());
    static_cast<void>(r.ReadUtf16(256U));
  }
  const auto expiry = r.ReadCount(128U);
  static_cast<void>(r.ReadBytes(static_cast<std::size_t>(expiry) * 12U));
  r.RequireEnd();
  return result;
}

CoupleAnswerRequest DecodeCoupleAnswer(std::span<const std::uint8_t> body) {
  PacketReader r{body};
  CoupleAnswerRequest result{.flag = Boolean(r),
                             .accepted = Boolean(r),
                             .inviter = r.ReadUtf16(32U),
                             .recipient = r.ReadUtf16(32U)};
  r.RequireEnd();
  return result;
}

bool DecodeCoupleCancel(std::span<const std::uint8_t> body) {
  PacketReader r{body};
  const bool result = Boolean(r);
  r.RequireEnd();
  return result;
}

std::u16string DecodeCoupleBreakup(std::span<const std::uint8_t> body) {
  PacketReader r{body};
  auto result = r.ReadUtf16(32U);
  r.RequireEnd();
  return result;
}

CoupleRingRequest DecodeCoupleRing(std::span<const std::uint8_t> body) {
  PacketReader r{body};
  CoupleRingRequest result{.ring = r.ReadU32(), .flag = Boolean(r)};
  r.RequireEnd();
  return result;
}

std::vector<std::uint8_t> EncodeCoupleInvite(
    const application::PlayerMenuSnapshot& inviter,
    std::u16string_view recipient, std::u16string_view message) {
  PacketWriter w;
  w.WriteU32(1U);
  w.WriteUtf16(Utf8ToUtf16(inviter.nickname));
  w.WriteUtf16(recipient);
  w.WriteUtf16(message);
  w.WriteBytes(EncodeEquippedItemsResponse(inviter));
  w.WriteBytes(EncodeFighterRecord(inviter));
  return w.Release();
}

std::vector<std::uint8_t> EncodeCoupleResult(std::uint32_t result,
                                             std::u16string_view target) {
  PacketWriter w;
  w.WriteU32(result);
  w.WriteUtf16(target);
  return w.Release();
}

std::vector<std::uint8_t> EncodeCoupleAnswer(bool accepted,
                                             std::u16string_view inviter,
                                             std::u16string_view recipient) {
  PacketWriter w;
  w.WriteU32(accepted ? 1U : 0U);
  w.WriteUtf16(inviter);
  w.WriteUtf16(recipient);
  return w.Release();
}

std::vector<std::uint8_t> EncodeCoupleBreakup(std::u16string_view actor,
                                              std::u16string_view partner,
                                              std::uint32_t result) {
  PacketWriter w;
  w.WriteUtf16(actor);
  w.WriteUtf16(partner);
  w.WriteU32(result);
  return w.Release();
}

std::vector<std::uint8_t> EncodeCouplePartner(
    const std::optional<application::CoupleProfileView>& couple) {
  PacketWriter w;
  w.WriteU32(couple ? 1U : 0U);
  w.WriteU32(couple ? static_cast<std::uint32_t>(couple->partner_id.Value())
                    : 0U);
  w.WriteUtf16(couple ? Utf8ToUtf16(couple->partner_nickname) : u"");
  return w.Release();
}

std::vector<std::uint8_t> EncodeCoupleRingResult(std::int32_t result,
                                                 std::uint32_t ring,
                                                 bool flag) {
  PacketWriter w;
  w.WriteI32(result);
  w.WriteU32(ring);
  w.WriteU32(flag ? 1U : 0U);
  return w.Release();
}
}  // namespace bf::protocol
