#include "protocol/battle/special_mode_messages.h"

#include <bit>
#include <cmath>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
std::optional<CaptainForm> GetCaptainForm(std::uint32_t item) noexcept {
  switch (item) {
    case 230U:
      return CaptainForm{230U, 1U, 701U, 7211U};
    case 231U:
      return CaptainForm{231U, 2U, 702U, 7213U};
    case 235U:
      return CaptainForm{235U, 1U, 704U, 7220U};
    case 236U:
      return CaptainForm{236U, 2U, 705U, 7220U};
    default:
      return std::nullopt;
  }
}

namespace {
// 7E6A40 emits two slots; 7E6C50 walks Character+1208/+1240 in 32-byte steps.
// A third known item dereferences unrelated Character storage at +1272.
constexpr std::size_t kCheckpointItemSlots = 2U;

[[noreturn]] void Invalid() {
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "invalid special mode payload"};
}

void ValidateSeat(std::uint32_t seat) {
  // 此服务端的房间协议和准备快照最多有 16 个玩家席位。
  if (seat >= 16U) {
    Invalid();
  }
}

void ValidateRespawn(const SpecialModeRespawn& value) {
  ValidateSeat(value.seat);
  const auto base = 1'000'000U + value.seat * 100'000U;
  // 7E6C50 forces 81BE00/800CD0 to write this float; the native setter has no
  // finite check.
  if (!std::isfinite(std::bit_cast<float>(value.hp_bits)) ||
      value.next_object <= base || value.next_object > base + 99'999U ||
      value.item_slots.size() > kCheckpointItemSlots ||
      (value.selected_item_slot <= 1U &&
       value.selected_item_slot >= value.item_slots.size())) {
    Invalid();
  }
  for (const auto component : value.position) {
    if (!std::isfinite(component) || std::abs(component) > 1.0e7F) {
      Invalid();
    }
  }
}
}  // namespace

NativeMoleState DecodeObservedMoleState(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const NativeMoleState state{reader.ReadU32(), reader.ReadU32()};
  reader.RequireEnd();
  if (state.state != 0U && state.state != 12U) Invalid();
  // Object identity and ChrMole type must be validated by a future registered
  // object source, not inferred from a character, pirate NPC or arbitrary ID.
  return state;
}

namespace {
std::array<float, 3U> MoleVector(protocol::PacketReader& reader) {
  std::array<float, 3U> values{};
  for (auto& value : values) {
    value = reader.ReadF32();
    // Same local numeric safety bound as other native vec3 decoders; not an
    // assertion about the original server's collision or distance policy.
    if (!std::isfinite(value) || std::abs(value) > 1.0e7F) Invalid();
  }
  return values;
}

NativeMoleCoinDrop MoleDrop(std::span<const std::uint8_t> body,
                            bool alternate) {
  PacketReader reader{body};
  NativeMoleCoinDrop value{MoleVector(reader), MoleVector(reader),
                           reader.ReadU32(), reader.ReadU32(),
                           reader.ReadU32()};
  reader.RequireEnd();
  if (alternate ? value.kind != 1U || value.variant != 1U
                : (value.kind != 1U && value.kind != 2U) || value.variant != 6U)
    Invalid();
  return value;
}
}  // namespace

NativeMoleSpawn DecodeObservedMoleSpawn(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NativeMoleSpawn value{reader.ReadU32(), reader.ReadU32(), reader.ReadU64(),
                        reader.ReadU32(), MoleVector(reader)};
  reader.RequireEnd();
  if (value.slot >= 64U) Invalid();
  return value;
}

NativeMoleCoinDrop DecodeObservedMoleCoinDrop(
    std::span<const std::uint8_t> body) {
  return MoleDrop(body, false);
}

NativeMoleCoinDrop DecodeObservedMoleAlternateDrop(
    std::span<const std::uint8_t> body) {
  return MoleDrop(body, true);
}

NativeMoleCoinSpawn DecodeObservedMoleCoinSpawn(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  NativeMoleCoinSpawn value{reader.ReadU32(), reader.ReadU32(),
                            MoleVector(reader), MoleVector(reader),
                            reader.ReadU32()};
  reader.RequireEnd();
  if (!ObservedMoleCoinAmount(value.kind)) Invalid();
  return value;
}

std::optional<std::uint32_t> ObservedMoleCoinAmount(
    std::uint32_t kind) noexcept {
  if (kind == 1U) return 1U;
  if (kind == 2U) return 100U;
  return std::nullopt;
}

NativeMoleCoinPickup DecodeObservedMoleCoinPickup(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const NativeMoleCoinPickup value{reader.ReadU32(), reader.ReadU32()};
  reader.RequireEnd();
  return value;
}

NativeMoleCoinStat DecodeObservedMoleCoinStat(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const NativeMoleCoinStat value{reader.ReadU8(), reader.ReadU32(),
                                 reader.ReadU8()};
  reader.RequireEnd();
  if (value.event != 10U && value.event != 13U) Invalid();
  return value;
}

CaptainTransformRequest DecodeCaptainTransformRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto object = reader.ReadU32();
  const auto form = GetCaptainForm(reader.ReadU32());
  reader.RequireEnd();
  if (!form) Invalid();
  return {object, *form};
}

CaptainDefeatReport DecodeCaptainDefeatReport(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  if (reader.ReadU8() != 27U) Invalid();
  CaptainDefeatReport result{reader.ReadU32(), reader.ReadU32(),
                             reader.ReadU8(), reader.ReadU8(), reader.ReadU8()};
  if (reader.ReadU16() != 0U || reader.ReadU16() != 27U) Invalid();
  reader.RequireEnd();
  ValidateSeat(result.victim_seat);
  ValidateSeat(result.attacker_seat);
  return result;
}

bool IsPveEventOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32897U || opcode == 923U || opcode == 925U ||
         opcode == 926U || opcode == 929U || opcode == 729U ||
         opcode == 32896U || opcode == 32914U || opcode == 32908U ||
         opcode == 32913U;
}

PveItemDrop DecodePveItemDrop(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  PveItemDrop result;
  for (auto& value : result.position) value = reader.ReadF32();
  for (auto& value : result.velocity) value = reader.ReadF32();
  result.type = reader.ReadU32();
  result.team = reader.ReadU8();
  result.lifetime_ms = reader.ReadU32();
  reader.RequireEnd();
  return result;
}

void ValidatePveEvent(std::uint16_t opcode,
                      std::span<const std::uint8_t> body) {
  if (opcode == 729U) {
    static_cast<void>(DecodePveItemDrop(body));
    return;
  }
  PacketReader reader{body};
  if (opcode == 32913U) {
    // 7E53A0 -> 7E5520 -> 91ACB0: global Boss AI state, parameter (not actor
    // IDs).
    const auto state = reader.ReadU32();
    const auto parameter = reader.ReadU32();
    reader.RequireEnd();
    if ((state != 0U && state != 8U && state != 9U && state != 10U &&
         state != 11U) ||
        (state == 0U && parameter != 0xffffffffU) ||
        (state == 9U && parameter > 1U))
      Invalid();
    return;
  }
  if (opcode == 32908U) {
    // 91A2B0 -> 7E5280/7EEEF0: Boss, target, DWORD false, destination vec3.
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
    if (reader.ReadU32() != 0U) Invalid();
    for (unsigned index = 0U; index < 3U; ++index) {
      const auto value = reader.ReadF32();
      if (!std::isfinite(value) || std::abs(value) > 1.0e7F) Invalid();
    }
    reader.RequireEnd();
    return;
  }
  if (opcode == 32896U || opcode == 32914U) {
    // 90A130 subtracts FLOAT damage from a shared gauge; 919A10 writes Boss HP.
    // The shared gauge can cross zero; negative HP has no valid receiver use.
    const auto value = reader.ReadF32();
    reader.RequireEnd();
    if (!std::isfinite(value) || std::abs(value) > 1.0e7F ||
        (opcode == 32914U && value < 0.0F))
      Invalid();
    return;
  }
  static_cast<void>(reader.ReadU32());
  if (opcode == 929U) {
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU32());
  } else if (opcode == 32897U)
    static_cast<void>(reader.ReadU32());
  else if (opcode != 923U && opcode != 925U && opcode != 926U)
    Invalid();
  reader.RequireEnd();
}

bool IsSpecialModeClaimOpcode(std::uint16_t opcode) noexcept {
  return opcode == 725U || opcode == 726U;
}

SpecialModeFinishClaim DecodeSpecialModeFinishClaim(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  SpecialModeFinishClaim result{.value = reader.ReadU32()};
  reader.RequireEnd();
  if (result.value > 1U) {
    Invalid();
  }
  return result;
}

void ValidateSpecialModeObjectiveClaim(std::span<const std::uint8_t> body) {
  PacketReader{body}.RequireEnd();
}

void ValidateInfectionTransitionRequest(std::span<const std::uint8_t> body) {
  PacketReader{body}.RequireEnd();
}

void ValidateSpecialModeClaim(std::uint16_t opcode,
                              std::span<const std::uint8_t> body) {
  if (opcode == 725U) {
    static_cast<void>(DecodeSpecialModeFinishClaim(body));
  } else if (opcode == 726U) {
    ValidateSpecialModeObjectiveClaim(body);
  } else {
    Invalid();
  }
}

std::vector<std::uint8_t> EncodeInfectionTeamResponse(std::uint32_t mode,
                                                      std::uint32_t seat,
                                                      std::uint8_t team) {
  ValidateSeat(seat);
  const bool pve = mode == 16U && (team == 2U || team == 35U);
  const bool infection =
      (mode == 8U || mode == 28U) && (team == 1U || team == 34U);
  if (!pve && !infection) {
    Invalid();
  }
  PacketWriter writer;
  writer.WriteU32(seat);
  writer.WriteU8(team);
  return writer.Release();
}

SpecialModeRespawn DecodeSpecialModeRespawn(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  SpecialModeRespawn result;
  result.seat = reader.ReadU32();
  result.movement_state = reader.ReadU32();
  result.hp_bits = reader.ReadU32();
  result.field_12 = reader.ReadU32();
  for (auto& component : result.position) {
    component = reader.ReadF32();
  }
  result.next_object = reader.ReadU32();
  result.selected_item_slot = reader.ReadU32();
  const auto count = reader.ReadU32();
  if (count > kCheckpointItemSlots) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "special mode weapon count exceeds limit"};
  }
  for (std::uint32_t i = 0U; i < count; ++i) {
    result.item_slots.push_back(reader.ReadU32());
  }
  result.field_48 = reader.ReadU32();
  result.field_52 = reader.ReadU32();
  reader.RequireEnd();
  ValidateRespawn(result);
  return result;
}

NativeObjectCheckpointRequest DecodeNativeObjectCheckpointRequest(
    std::span<const std::uint8_t> body,
    std::uint32_t authenticated_sender_seat) {
  ValidateSeat(authenticated_sender_seat);
  if (body.size() > 48U + 4U * kCheckpointItemSlots) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "native object checkpoint exceeds two weapon slots"};
  }
  PacketReader reader{body};
  const auto recipient = reader.ReadU32();
  ValidateSeat(recipient);
  // 2018 and982 have the same field widths but distinct first-field semantics.
  // Rewrite only a private copy for the existing strict sender-range decoder.
  auto response = std::vector<std::uint8_t>{body.begin(), body.end()};
  for (unsigned shift = 0U; shift < 4U; ++shift)
    response[shift] =
        static_cast<std::uint8_t>(authenticated_sender_seat >> (shift * 8U));
  return {recipient, DecodeSpecialModeRespawn(response)};
}

std::vector<std::uint8_t> EncodeSpecialModeRespawn(
    const SpecialModeRespawn& response) {
  ValidateRespawn(response);
  PacketWriter writer;
  writer.WriteU32(response.seat);
  writer.WriteU32(response.movement_state);
  writer.WriteU32(response.hp_bits);
  writer.WriteU32(response.field_12);
  for (const auto component : response.position) {
    writer.WriteF32(component);
  }
  writer.WriteU32(response.next_object);
  writer.WriteU32(response.selected_item_slot);
  writer.WriteU32(static_cast<std::uint32_t>(response.item_slots.size()));
  for (const auto item : response.item_slots) {
    writer.WriteU32(item);
  }
  writer.WriteU32(response.field_48);
  writer.WriteU32(response.field_52);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCaptainSkillResponse(std::uint32_t seat) {
  ValidateSeat(seat);
  PacketWriter writer;
  writer.WriteU32(seat);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCaptainOptionResponse(bool enabled) {
  PacketWriter writer;
  writer.WriteU32(enabled ? 1U : 0U);
  return writer.Release();
}

std::uint32_t DecodeCaptainRestoreRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto seat = reader.ReadU32();
  reader.RequireEnd();
  ValidateSeat(seat);
  return seat;
}
}  // namespace bf::protocol
