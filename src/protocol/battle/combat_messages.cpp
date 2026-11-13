#include "protocol/battle/combat_messages.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

#include "features/chat/user_message.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

constexpr float kMaximumCoordinateMagnitude = 10'000'000.0F;

[[nodiscard]] std::uint8_t ReadU8At(std::span<const std::uint8_t> body,
                                    std::size_t offset) {
  return body[offset];
}

[[nodiscard]] std::uint16_t ReadU16At(std::span<const std::uint8_t> body,
                                      std::size_t offset) {
  return static_cast<std::uint16_t>(body[offset]) |
         (static_cast<std::uint16_t>(body[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t ReadU32At(std::span<const std::uint8_t> body,
                                      std::size_t offset) {
  return static_cast<std::uint32_t>(body[offset]) |
         (static_cast<std::uint32_t>(body[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(body[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(body[offset + 3U]) << 24U);
}

[[nodiscard]] float ReadF32At(std::span<const std::uint8_t> body,
                              std::size_t offset) {
  return std::bit_cast<float>(ReadU32At(body, offset));
}

[[nodiscard]] float HalfToFloat(std::uint16_t bits) noexcept {
  const auto sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
  auto exponent = static_cast<std::uint32_t>((bits >> 10U) & 0x1FU);
  auto mantissa = static_cast<std::uint32_t>(bits & 0x03FFU);
  std::uint32_t result = 0U;
  if (exponent == 0U) {
    if (mantissa == 0U) {
      result = sign;
    } else {
      exponent = 113U;
      while ((mantissa & 0x0400U) == 0U) {
        mantissa <<= 1U;
        --exponent;
      }
      mantissa &= 0x03FFU;
      result = sign | (exponent << 23U) | (mantissa << 13U);
    }
  } else if (exponent == 0x1FU) {
    result = sign | 0x7F800000U | (mantissa << 13U);
  } else {
    result = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
  }
  return std::bit_cast<float>(result);
}

void RequireSize(std::span<const std::uint8_t> body, std::size_t expected,
                 std::string_view message) {
  if (body.size() != expected) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue, message};
  }
}

void RequireFinite(float value, std::string_view message) {
  if (!std::isfinite(value) || std::abs(value) > kMaximumCoordinateMagnitude) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue, message};
  }
}

void RequireBoolean(std::uint32_t value, std::string_view message) {
  if (value > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue, message};
  }
}

[[nodiscard]] std::array<float, 3U> ReadPositionF32(
    std::span<const std::uint8_t> body, std::size_t offset) {
  std::array<float, 3U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = ReadF32At(body, offset + (index * sizeof(float)));
    RequireFinite(result[index], "native position is not finite or bounded");
  }
  return result;
}

[[nodiscard]] std::array<float, 3U> ReadPositionHalf(
    std::span<const std::uint8_t> body, std::size_t offset) {
  std::array<float, 3U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] =
        HalfToFloat(ReadU16At(body, offset + (index * sizeof(std::uint16_t))));
    RequireFinite(result[index], "compressed native position is not finite");
  }
  return result;
}

}  // namespace

bool IsNativeActionOpcode(std::uint16_t opcode) noexcept {
  switch (opcode) {
    case 32898U:
    case 32895U:
    case 32778U:
    case 32930U:
    case 751U:
    case 33279U:
    case 33276U:
    case 33277U:
    case 33269U:
    case 33270U:
    case 33271U:
    case 33272U:
    case 33273U:
    case 33274U:
    case 32828U:
    case 32829U:
    case 32815U:
    case 32816U:
    case 32830U:
    case 32831U:
    case 32887U:
    case 32932U:
    case 32933U:
    case 32833U:
    case 32909U:
    case 25U:
    case 710U:
    case 930U:
    case 32889U:
    case 103U:
    case 713U:
    case 32905U:
    case 32791U:
    case 32792U:
    case 32794U:
    case 32795U:
    case 32796U:
    case 32837U:
    case 32769U:
    case 32779U:
    case 32780U:
    case 32781U:
    case 32782U:
    case 32783U:
    case 32784U:
    case 32785U:
    case 32786U:
    case 32788U:
    case 32789U:
    case 32790U:
    case 32800U:
    case 32804U:
    case 32809U:
    case 32821U:
    case 32834U:
    case 32810U:
    case 32811U:
    case 32812U:
    case 32813U:
    case 32885U:
    case 32882U:
    case 32925U:
      return true;
    default:
      return false;
  }
}

bool IsNativeWeaponObjectOpcode(std::uint16_t opcode) noexcept {
  return opcode == 33281U || opcode == 32772U || opcode == 32900U ||
         opcode == 33282U || opcode == 32907U || opcode == 32774U ||
         opcode == 32818U;
}

bool IsNativeStormOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32899U || opcode == 32910U || opcode == 32911U ||
         opcode == 32916U;
}

NativeStormRequest DecodeNativeStorm(std::uint16_t opcode,
                                     std::span<const std::uint8_t> body) {
  NativeStormRequest result;
  if (opcode == 32899U || opcode == 32910U) {
    RequireSize(body, 19U, "native storm creation size is invalid");
    result.actor_object_id = ReadU32At(body, 0U);
    result.weapon = ReadU16At(body, 4U);
    result.team = ReadU8At(body, 6U);
    result.target = ReadU32At(body, 7U);
    result.type = opcode == 32899U ? 124U : ReadU32At(body, 11U);
    result.cookie = ReadU32At(body, opcode == 32899U ? 11U : 15U);
    // 32899 +15 is an opaque timer token. 32910 +15 is the damager cookie.
  } else if (opcode == 32911U || opcode == 32916U) {
    RequireSize(body, 8U, "native storm update size is invalid");
    result.actor_object_id = ReadU32At(body, opcode == 32916U ? 4U : 0U);
    if (opcode == 32911U)
      result.type = ReadU32At(body, 4U);
    else
      result.cookie = ReadU32At(body, 0U);
  } else {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown native storm opcode"};
  }
  return result;
}

NativeWeaponObjectRequest DecodeNativeWeaponObject(
    std::uint16_t opcode, std::span<const std::uint8_t> body) {
  NativeWeaponObjectRequest result;
  switch (opcode) {
    case 32774U:
      // 7C49A0 -> 7D6260 deletes a GameObject; ownership is resolved from the
      // ledger.
      RequireSize(body, 4U, "native projectile destruction size is invalid");
      result.kind = NativeWeaponObjectKind::kDestroyProjectile;
      result.cookie = ReadU32At(body, 0U);
      break;
    case 32818U:
      // 80EFB0 -> 7CD5D0 -> 7DF9A0/810AD0: grenade, attached actor, local vec3.
      RequireSize(body, 20U, "native grenade attachment size is invalid");
      result.kind = NativeWeaponObjectKind::kAttachGrenade;
      result.cookie = ReadU32At(body, 0U);
      result.target_object_id = ReadU32At(body, 4U);
      static_cast<void>(ReadPositionF32(body, 8U));
      break;
    case 33281U:
    case 32772U:
      RequireSize(body, opcode == 33281U ? 15U : 51U,
                  "native weapon creation size is invalid");
      result.kind = opcode == 33281U ? NativeWeaponObjectKind::kMelee
                                     : NativeWeaponObjectKind::kBlast;
      result.actor_object_id = ReadU32At(body, 0U);
      result.template_id = ReadU16At(body, 4U);
      result.attack_variant = std::bit_cast<std::int8_t>(ReadU8At(body, 6U));
      result.cookie = ReadU32At(body, opcode == 33281U ? 7U : 43U);
      if (opcode == 32772U) {
        for (std::size_t offset = 7U; offset < 43U; offset += sizeof(float)) {
          RequireFinite(ReadF32At(body, offset),
                        "native blast vector is invalid");
        }
      }
      break;
    case 32900U:
      RequireSize(body, 48U, "native detonation size is invalid");
      result.kind = NativeWeaponObjectKind::kDetonate;
      result.source_cookie = ReadU32At(body, 0U);
      result.cookie = ReadU32At(body, 40U);
      for (std::size_t offset = 4U; offset < 40U; offset += sizeof(float)) {
        RequireFinite(ReadF32At(body, offset),
                      "native detonation vector is invalid");
      }
      break;
    case 33282U:
      RequireSize(body, 20U, "native weapon impact size is invalid");
      result.kind = NativeWeaponObjectKind::kHit;
      result.cookie = ReadU32At(body, 0U);
      result.target_object_id = ReadU32At(body, 4U);
      static_cast<void>(ReadPositionF32(body, 8U));
      break;
    case 32907U:
      // 7C4B00 -> 7D63A0 resolves a GoDamager cookie, including GoGrenade.
      RequireSize(body, 4U, "native weapon destroy size is invalid");
      result.kind = NativeWeaponObjectKind::kDestroy;
      result.cookie = ReadU32At(body, 0U);
      break;
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native weapon opcode is not recovered"};
  }
  return result;
}

bool IsNativeHitOpcode(std::uint16_t opcode) noexcept {
  return opcode == 32776U || opcode == 32924U;
}

bool IsNativeLifecycleOpcode(std::uint16_t opcode) noexcept {
  switch (opcode) {
    case 32787U:
    case 32801U:
    case 32805U:
    case 32806U:
    case 32835U:
    case 32836U:
    case 32921U:
      return true;
    default:
      return false;
  }
}

bool IsNativeRespawnOpcode(std::uint16_t opcode) noexcept {
  return opcode == 704U || opcode == 705U || opcode == 32926U;
}

bool IsNativeRescueOpcode(std::uint16_t opcode) noexcept {
  return opcode == 33283U || opcode == 32904U || opcode == 33284U;
}

NativeRescueRequest DecodeNativeRescue(std::uint16_t opcode,
                                       std::span<const std::uint8_t> body) {
  if (!IsNativeRescueOpcode(opcode)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "unknown native rescue opcode"};
  }
  RequireSize(body, opcode == 32904U ? 12U : 9U,
              "native rescue size is invalid");
  if (opcode == 32904U) {
    const auto health = ReadF32At(body, 8U);
    RequireFinite(health, "native rescue health is invalid");
    if (health < 0.0F) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "negative native rescue health"};
    }
  } else {
    const auto kind = ReadU8At(body, 8U);
    if (kind != 0U && kind != 1U && kind != 3U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native rescue kind is invalid"};
    }
  }
  return {.victim = ReadU32At(body, opcode == 33284U ? 4U : 0U),
          .helper = ReadU32At(body, opcode == 33284U ? 0U : 4U),
          .kind = opcode == 32904U ? std::uint8_t{0U} : ReadU8At(body, 8U)};
}

void ValidateNativeCombatMessage(std::uint16_t opcode,
                                 std::span<const std::uint8_t> body) {
  if (IsNativeStormOpcode(opcode)) {
    static_cast<void>(DecodeNativeStorm(opcode, body));
    return;
  }
  if (IsNativeRescueOpcode(opcode)) {
    static_cast<void>(DecodeNativeRescue(opcode, body));
    return;
  }
  if (IsNativeWeaponObjectOpcode(opcode)) {
    static_cast<void>(DecodeNativeWeaponObject(opcode, body));
    return;
  }
  if (IsNativeActionOpcode(opcode)) {
    static_cast<void>(DecodeNativeAction(opcode, body));
    return;
  }
  if (IsNativeHitOpcode(opcode)) {
    static_cast<void>(DecodeNativeHit(opcode, body));
    return;
  }
  if (IsNativeLifecycleOpcode(opcode)) {
    static_cast<void>(DecodeNativeLifecycle(opcode, body));
    return;
  }
  if (IsNativeRespawnOpcode(opcode)) {
    static_cast<void>(DecodeNativeRespawn(opcode, body));
    return;
  }
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "native combat opcode is not recovered"};
}

NativeActionRequest DecodeNativeAction(std::uint16_t opcode,
                                       std::span<const std::uint8_t> body) {
  NativeActionRequest result;
  switch (opcode) {
    case 32898U:
      // 7EDF70: DWORD, WORD, byte, two vec3, DWORD, byte, two DWORDs.
      RequireSize(body, 44U, "native explicit fire size is invalid");
      result.kind = NativeActionKind::kExplicitFire;
      result.actor_object_id = ReadU32At(body, 0U);
      result.explicit_fire =
          NativeExplicitFire{.weapon = ReadU16At(body, 4U),
                             .variant = static_cast<std::int8_t>(body[6U]),
                             .position = ReadPositionF32(body, 7U),
                             .direction = ReadPositionF32(body, 19U),
                             .target = ReadU32At(body, 31U),
                             .flags = body[35U],
                             .cookie = ReadU32At(body, 36U),
                             .token = ReadU32At(body, 40U) & 0xffffffU};
      break;
    case 32895U:
    case 32778U:
      RequireSize(body, 8U, "native scene control size is invalid");
      result.kind = NativeActionKind::kSceneObjectControl;
      result.actor_object_id = ReadU32At(body, 0U);
      result.scene_value = ReadU32At(body, 4U);
      break;
    case 32930U:
      // 7EDDF0 / 7EFA00: actor DWORD, item DWORD, position vec3.
      RequireSize(body, 20U, "native area cast size is invalid");
      result.kind = NativeActionKind::kAreaCast;
      result.actor_object_id = ReadU32At(body, 0U);
      result.area_cast = NativeAreaCast{.type = ReadU32At(body, 4U),
                                        .position = ReadPositionF32(body, 8U)};
      break;
    case 751U:
    case 33279U:
      RequireSize(body, opcode == 751U ? 24U : 16U,
                  "invalid survival trap request size");
      result.actor_object_id = ReadU32At(body, 0U);
      result.kind = NativeActionKind::kSurvivalTrap;
      if (opcode == 751U) result.position = ReadPositionF32(body, 12U);
      break;
    case 33276U:
    case 33277U: {
      RequireSize(body, 20U, "invalid survival melee size");
      NativeSurvivalMelee melee;
      melee.confirmation = opcode == 33277U;
      melee.attacker = ReadU32At(body, melee.confirmation ? 16U : 0U);
      melee.victim = ReadU32At(body, melee.confirmation ? 0U : 4U);
      melee.vector = ReadPositionF32(body, melee.confirmation ? 4U : 8U);
      if (melee.confirmation) {
        // E9BBF0 -> D3DXVec3Normalize: this is facing, never a movement sample.
        const auto& v = melee.vector;
        const float length_squared = (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2];
        if (std::abs(length_squared - 1.0F) > 0.02F)
          throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                              "invalid survival melee direction"};
      }
      result.kind = NativeActionKind::kSurvivalMelee;
      result.actor_object_id =
          melee.confirmation ? melee.victim : melee.attacker;
      result.survival_melee = melee;
      break;
    }
    case 33269U:
    case 33270U:
    case 33271U:
    case 33272U: {
      NativeSurvivalTreatment treatment;
      if (opcode == 33269U) {
        if (body.size() < 8U)
          throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                              "truncated survival effect list"};
        const auto count = ReadU32At(body, 4U);
        // Bound allocation before multiplication; the native vector stores
        // WORDs.
        if (count == 0U || count > 256U)
          throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                              "invalid survival effect count"};
        RequireSize(body, 8U + 2U * count, "invalid survival effect list size");
        for (std::uint32_t i = 0U; i < count; ++i) {
          const auto id = ReadU16At(body, 8U + 2U * i);
          if (std::ranges::find(treatment.expired_effects, id) !=
              treatment.expired_effects.end())
            throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                                "duplicate survival effect id"};
          treatment.expired_effects.push_back(id);
        }
      } else {
        RequireSize(body, opcode == 33272U ? 7U : 4U,
                    "invalid survival treatment size");
        if (opcode == 33272U) {
          treatment.effect_id = ReadU16At(body, 4U);
          treatment.stage = ReadU8At(body, 6U);
          if (treatment.stage > 5U)
            throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                                "invalid survival heal stage"};
        }
      }
      result.kind = NativeActionKind::kSurvivalTreatment;
      result.actor_object_id = ReadU32At(body, 0U);
      result.survival_treatment = std::move(treatment);
      break;
    }
    case 33273U:
    case 33274U:
      // 57FC30 -> 7CCB50/7CCBF0; 7C25D0 -> 842D40/843040. These
      // synchronize the Area32 Alliance pose, not team membership or healing.
      RequireSize(body, 4U, "native survival alliance pose size is invalid");
      result.kind = NativeActionKind::kSurvivalAlliancePose;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32932U:
      // 7E7EA0/7C4BA0 -> 7C4C60: victim, registered area-effect source.
      // Only the victim reports this transition; no damage amount is on the
      // wire.
      RequireSize(body, 8U, "native area reaction size is invalid");
      result.kind = NativeActionKind::kAreaHitReaction;
      result.actor_object_id = ReadU32At(body, 0U);
      result.reaction_source = ReadU32At(body, 4U);
      break;
    case 32933U: {
      // 580F30/7C4DD0 -> 7C4E90: local ChrPlayer HP replicated to remote
      // actors.
      RequireSize(body, 8U, "native player health size is invalid");
      const auto health = ReadF32At(body, 4U);
      if (!std::isfinite(health) || health < 0.0F || health > 1.0e7F)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native player health is invalid"};
      result.kind = NativeActionKind::kHealthReport;
      result.actor_object_id = ReadU32At(body, 0U);
      result.reported_health = health;
      break;
    }
    case 32887U: {
      // 7C5D10/7EE870 -> 7EFDA0/7DE570: object, nickname, text. 853590
      // resolves text through DAA1E0 into the original emotion animation table.
      PacketReader reader{body};
      result.actor_object_id = reader.ReadU32();
      static_cast<void>(Utf16ToUtf8(reader.ReadUtf16(256U)));
      const auto text = reader.ReadUtf16(domain::kMaximumUserMessageCodeUnits);
      reader.RequireEnd();
      if (!std::holds_alternative<domain::UserMessage>(
              domain::UserMessage::Parse(text)))
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native text emote is invalid"};
      result.kind = NativeActionKind::kTextEmote;
      break;
    }
    case 32828U:
    case 32829U:
      // 7CA3F0: actor, attribute, item type, duration; 7CA4D0: actor, item
      // type.
      RequireSize(body, opcode == 32828U ? 16U : 8U,
                  "native item effect size is invalid");
      result.kind = NativeActionKind::kItemEffect;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32831U:
    case 32909U:
      // 8485F0/850600 (ItemSpec 208) -> 7CA6F0 -> 7D0AF0. This only clears
      // the actor's area-effect flag; it cannot grant an effect, HP or an item.
      // 32831: 848990 -> 7CA790 -> 7D0B70 clears Character+2404 in the same
      // way.
      RequireSize(body, 4U, "native area effect end size is invalid");
      result.kind = NativeActionKind::kEffectEnd;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32815U:
    case 32816U:
      // 7C95A0/7C9660 -> 7D8FD0/7D9080: attacker, humiliated object.
      RequireSize(body, 8U, "native humiliation size is invalid");
      result.kind = NativeActionKind::kHumiliation;
      result.actor_object_id = ReadU32At(body, 0U);
      result.humiliation_target = ReadU32At(body, 4U);
      break;
    case 32830U:
      // 848B60 -> 7CA590 -> 7D0990/8491E0: finish the 126/134 persistent
      // effect.
      RequireSize(body, 8U, "native effect end size is invalid");
      result.kind = NativeActionKind::kEffectEnd;
      result.actor_object_id = ReadU32At(body, 0U);
      if (ReadU32At(body, 4U) != 126U && ReadU32At(body, 4U) != 134U)
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native effect end type is invalid"};
      break;
    case 32833U:
      // 7EECB0 expands the native BYTE to a DWORD; 7F0FF0 reads seven DWORDs.
      RequireSize(body, 28U, "native slip size is invalid");
      result.kind = NativeActionKind::kSlip;
      result.consumed_object = ReadU32At(body, 0U);
      result.actor_object_id = ReadU32At(body, 4U);
      RequireBoolean(ReadU32At(body, 8U), "native slip bubble flag is invalid");
      result.slip_bubbled = ReadU32At(body, 8U) != 0U;
      static_cast<void>(ReadPositionF32(body, 16U));
      break;
    case 32804U:
      // 7C8180/7EE3F0：DWORD 对象、WORD 特效、DWORD 阵营、三个 vec3、两个
      // DWORD。
      RequireSize(body, 54U, "native infection effect size is invalid");
      result.actor_object_id = ReadU32At(body, 0U);
      for (const auto offset : {10U, 22U, 34U})
        static_cast<void>(ReadPositionF32(body, offset));
      result.kind = NativeActionKind::kInfectionEffect;
      break;
    case 25U:
      // B7F120 -> B8D620/B8D6F0：834
      // 成功后的战斗连接通知，区别于网关空包战队请求。
      RequireSize(body, 5U, "native preset notification size is invalid");
      if (ReadU8At(body, 4U) > 4U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native preset index is invalid"};
      }
      result.actor_object_id = ReadU32At(body, 0U);
      result.kind = NativeActionKind::kOpaqueOwnedReport;
      break;
    case 32889U:
      // 7D84F0 在 32836 死亡动画完成后调用 7D87A0：16 字节摘要之和，非角色 ID。
      RequireSize(body, 4U, "native checksum report size is invalid");
      if (ReadU32At(body, 0U) > 16U * 255U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native checksum report exceeds byte sum"};
      }
      result.kind = NativeActionKind::kOpaqueReport;
      break;
    case 103U:
      RequireSize(body, 120U, "native opaque results report size is invalid");
      result.kind = NativeActionKind::kOpaqueReport;
      break;
    case 713U:
      RequireSize(body, 8U, "native opaque owned report size is invalid");
      result.actor_object_id = ReadU32At(body, 0U);
      result.kind = NativeActionKind::kOpaqueOwnedReport;
      break;
    case 32905U:
      // 582420 -> 7C45B0: local 401 entry reports object and computed initial
      // duration.
      RequireSize(body, 8U, "native respawn duration report size is invalid");
      result.actor_object_id = ReadU32At(body, 0U);
      result.respawn_duration_ms = ReadU32At(body, 4U);
      result.kind = NativeActionKind::kRespawnDurationReport;
      break;
    case 32791U:
    case 32792U:
    case 32794U:
    case 32795U:
    case 32796U:
    case 32837U: {
      const std::size_t size = opcode == 32791U   ? 12U
                               : opcode == 32794U ? 8U
                               : opcode == 32795U ? 28U
                               : opcode == 32796U ? 20U
                                                  : 4U;
      RequireSize(body, size, "native bubble action size is invalid");
      NativeBubbleAction action;
      if (opcode == 32791U || opcode == 32792U || opcode == 32837U) {
        action.attacker = ReadU32At(body, 0U);
        if (opcode == 32791U) {
          action.victim = ReadU32At(body, 4U);
          action.variant = ReadU32At(body, 8U);
        }
        result.actor_object_id = action.attacker;
      } else {
        const std::size_t offset = opcode == 32795U ? 4U : 0U;
        action.victim = ReadU32At(body, offset);
        action.attacker = ReadU32At(body, offset + 4U);
        result.actor_object_id =
            opcode == 32794U ? action.attacker : action.victim;
        if (opcode != 32794U) {
          static_cast<void>(ReadPositionF32(body, offset + 8U));
        }
        if (opcode == 32795U) {
          RequireBoolean(ReadU32At(body, 0U),
                         "native kick success flag is invalid");
          RequireBoolean(ReadU32At(body, 24U),
                         "native kick special flag is invalid");
          action.success = ReadU32At(body, 0U) != 0U;
          action.shot_burst = ReadU32At(body, 24U) != 0U;
        }
      }
      result.kind = NativeActionKind::kBubbleAction;
      result.bubble_action = action;
      break;
    }
    case 710U:
      RequireSize(body, 16U, "native opaque event report size is invalid");
      result.kind = NativeActionKind::kOpaqueReport;
      break;
    case 930U:
      // 旧服务只忽略此变长诊断报告。未恢复内部字段，不记录或广播负载。
      if (body.size() < 4U || body.size() > 8192U) {
        throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                            "native diagnostic report exceeds bound"};
      }
      result.kind = NativeActionKind::kOpaqueReport;
      break;
    case 32769U:
      RequireSize(body, 47U, "native transform size is invalid");
      result.kind = NativeActionKind::kTransform;
      result.actor_object_id = ReadU32At(body, 0U);
      for (std::size_t index = 0U; index < 5U; ++index) {
        RequireFinite(ReadF32At(body, 4U + (index * sizeof(float))),
                      "native transform is not finite or bounded");
      }
      RequireBoolean(ReadU32At(body, 38U), "native movement flag is invalid");
      result.position = ReadPositionF32(body, 4U);
      break;
    case 32779U:
      RequireSize(body, 8U, "native action flag size is invalid");
      result.kind = NativeActionKind::kActionFlag;
      result.actor_object_id = ReadU32At(body, 0U);
      RequireBoolean(ReadU32At(body, 4U), "native action flag is invalid");
      break;
    case 32780U:
      RequireSize(body, 10U, "native opaque action size is invalid");
      result.kind = NativeActionKind::kOpaqueTenBytes;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32781U:
      RequireSize(body, 12U, "native jump size is invalid");
      result.kind = NativeActionKind::kJump;
      result.actor_object_id = ReadU32At(body, 0U);
      RequireFinite(ReadF32At(body, 4U),
                    "native jump height is not finite or bounded");
      RequireBoolean(ReadU32At(body, 8U), "native jump flag is invalid");
      break;
    case 32782U:
      RequireSize(body, 4U, "native stop-shoot size is invalid");
      result.kind = NativeActionKind::kStopShoot;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32783U:
      RequireSize(body, 8U, "native object pair size is invalid");
      result.kind = NativeActionKind::kUnrecoveredObjectPair;
      result.actor_object_id = ReadU32At(body, 4U);
      break;
    case 32785U:
      RequireSize(body, 8U, "native grenade-hold size is invalid");
      result.kind = NativeActionKind::kHoldGrenade;
      result.actor_object_id = ReadU32At(body, 0U);
      RequireBoolean(ReadU32At(body, 4U),
                     "native grenade-hold flag is invalid");
      break;
    case 32784U:
      // 7C6820 -> 7EFD30/7DEF40: actor, signed integer height, enabled,
      // variant. Height is CVTTSS2SI output, not an IEEE754 float or a position
      // update.
      RequireSize(body, 16U, "native character-height control size is invalid");
      result.kind = NativeActionKind::kCharacterControl;
      result.actor_object_id = ReadU32At(body, 0U);
      RequireBoolean(ReadU32At(body, 8U),
                     "native character-height flag is invalid");
      RequireBoolean(ReadU32At(body, 12U),
                     "native character-height variant is invalid");
      break;
    case 32812U:
      // 7CA240 -> 7DF3C0: remove a character effect by its resource key.
      RequireSize(body, 8U, "native effect cancellation size is invalid");
      result.kind = NativeActionKind::kCharacterControl;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32882U:
      // 7CD900 -> 7DFE50: both boolean fields occupy a full DWORD on the wire.
      RequireSize(body, 12U, "native character-toggle size is invalid");
      result.kind = NativeActionKind::kCharacterControl;
      result.actor_object_id = ReadU32At(body, 0U);
      RequireBoolean(ReadU32At(body, 4U),
                     "native character-toggle flag is invalid");
      RequireBoolean(ReadU32At(body, 8U),
                     "native character-toggle option is invalid");
      break;
    case 32786U:
      RequireSize(body, 9U, "native wall action size is invalid");
      result.kind = NativeActionKind::kWallAction;
      result.actor_object_id = ReadU32At(body, 0U);
      if (ReadU32At(body, 4U) > 3U || ReadU8At(body, 8U) > 2U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native wall action is invalid"};
      }
      break;
    case 32800U:
      RequireSize(body, 12U, "native wall exit size is invalid");
      result.kind = NativeActionKind::kWallAction;
      result.actor_object_id = ReadU32At(body, 0U);
      if (ReadU32At(body, 4U) != 1U || ReadU32At(body, 8U) != 101U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native wall exit state is invalid"};
      }
      break;
    case 32809U:
    case 32834U: {
      RequireSize(body, 8U, "native wall state size is invalid");
      result.kind = NativeActionKind::kWallAction;
      result.actor_object_id = ReadU32At(body, 0U);
      const auto state = ReadU32At(body, 4U);
      if ((opcode == 32809U && state != 204U) ||
          (opcode == 32834U && (state < 201U || state > 209U))) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native wall state is invalid"};
      }
      break;
    }
    case 32821U:
      RequireSize(body, 6U, "native wall pose size is invalid");
      result.kind = NativeActionKind::kWallPose;
      result.actor_object_id = ReadU32At(body, 0U);
      // Build752 8FE950 sets both 3 and 4; 9028B0 serializes the pose as a
      // BYTE.
      if (ReadU8At(body, 4U) < 201U || ReadU8At(body, 4U) > 210U ||
          ReadU8At(body, 5U) > 4U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native wall pose is invalid: state=" +
                                std::to_string(ReadU8At(body, 4U)) +
                                " pose=" + std::to_string(ReadU8At(body, 5U))};
      }
      break;
    case 32788U:
      RequireSize(body, 24U, "native hit-reaction size is invalid");
      result.kind = NativeActionKind::kHitReaction;
      result.actor_object_id = ReadU32At(body, 0U);
      result.reaction_source = ReadU32At(body, 4U);
      static_cast<void>(
          ReadPositionF32(body, 8U));  // 原版受击方向，不更新玩家坐标。
      break;
    case 32789U:
    case 32790U:
      RequireSize(body, 4U, "native hit-recovery size is invalid");
      result.kind = NativeActionKind::kHitReaction;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32810U: {
      RequireSize(body, 20U, "native weapon-switch size is invalid");
      result.kind = NativeActionKind::kWeaponSwitch;
      result.actor_object_id = ReadU32At(body, 8U);
      const auto bar_index = ReadU32At(body, 4U);
      const auto select = ReadU32At(body, 16U);
      if (bar_index >= 5U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native weapon-switch bar is invalid"};
      }
      RequireBoolean(select, "native weapon-switch flag is invalid");
      result.weapon_switch =
          NativeWeaponSwitch{.template_id = ReadU32At(body, 0U),
                             .bar_index = bar_index,
                             .select = select != 0U};
      break;
    }
    case 32811U:
      RequireSize(body, 8U, "native opaque eight-byte action size is invalid");
      result.kind = NativeActionKind::kOpaqueEightBytes;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32813U:
      RequireSize(body, 4U, "native opaque four-byte action size is invalid");
      result.kind = NativeActionKind::kOpaqueFourBytes;
      result.actor_object_id = ReadU32At(body, 0U);
      break;
    case 32885U:
      RequireSize(body, 21U, "native compressed transform size is invalid");
      result.kind = NativeActionKind::kCompressedTransform;
      result.actor_object_id = ReadU32At(body, 0U);
      result.position = ReadPositionF32(body, 4U);
      break;
    case 32925U: {
      RequireSize(body, 32U, "native compressed fire size is invalid");
      result.kind = NativeActionKind::kCompressedFire;
      result.actor_object_id = ReadU32At(body, 0U);
      const auto bar_index = ReadU8At(body, 4U);
      if (bar_index >= 5U) {
        throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                            "native fire bar is invalid"};
      }
      for (std::size_t index = 0U; index < 6U; ++index) {
        RequireFinite(
            HalfToFloat(ReadU16At(body, 5U + (index * sizeof(std::uint16_t)))),
            "native compressed fire transform is not finite");
      }
      result.compressed_fire =
          NativeCompressedFire{.bar_index = bar_index,
                               .serial = ReadU32At(body, 22U),
                               .token = ReadU32At(body, 26U) & 0x00FFFFFFU};
      break;
    }
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native action opcode is not recovered"};
  }
  return result;
}

NativeHitRequest DecodeNativeHit(std::uint16_t opcode,
                                 std::span<const std::uint8_t> body) {
  NativeHitRequest result;
  if (opcode == 32776U) {
    RequireSize(body, 25U, "native uncompressed hit size is invalid");
    result.projectile_cookie = ReadU32At(body, 0U);
    result.target_object_id = ReadU32At(body, 4U);
    result.position = ReadPositionF32(body, 8U);
    result.kind = ReadU8At(body, 20U);
    const auto keep = ReadU32At(body, 21U);
    RequireBoolean(keep, "native hit keep flag is invalid");
    result.keep_projectile = keep != 0U;
    return result;
  }
  if (opcode == 32924U) {
    RequireSize(body, 19U, "native compressed hit size is invalid");
    result.projectile_cookie = ReadU32At(body, 0U);
    result.target_object_id = ReadU32At(body, 4U);
    result.position = ReadPositionHalf(body, 8U);
    result.kind = ReadU8At(body, 14U);
    const auto keep = ReadU32At(body, 15U);
    RequireBoolean(keep, "native hit keep flag is invalid");
    result.keep_projectile = keep != 0U;
    return result;
  }
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "native hit opcode is not recovered"};
}

NativeLifecycleRequest DecodeNativeLifecycle(
    std::uint16_t opcode, std::span<const std::uint8_t> body) {
  NativeLifecycleRequest result;
  switch (opcode) {
    case 32805U:
      // 582510 / 8BA990 -> 7C8430 -> 7D8380: self/environmental death,
      // no attacking character and no pang credit. The receiver enters state
      // 402.
      RequireSize(body, 4U, "native self-defeat size is invalid");
      result.kind = NativeLifecycleKind::kDefeat;
      result.direct_defeat = true;
      break;
    case 32787U:
      RequireSize(body, 40U, "native defeat size is invalid");
      result.kind = NativeLifecycleKind::kDefeat;
      result.actor_object_id = ReadU32At(body, 0U);
      result.related_object_id = ReadU32At(body, 4U);
      static_cast<void>(ReadPositionF32(body, 8U));
      RequireBoolean(ReadU32At(body, 24U),
                     "native defeat pang flag is invalid");
      RequireBoolean(ReadU32At(body, 28U), "native defeat flag is invalid");
      RequireBoolean(ReadU32At(body, 36U),
                     "native defeat extra flag is invalid");
      result.direct_defeat = ReadU32At(body, 24U) != 0U;
      return result;
    case 32801U:
      RequireSize(body, 4U, "native bubble-settled size is invalid");
      result.kind = NativeLifecycleKind::kBubbleSettled;
      break;
    case 32806U:
      RequireSize(body, 8U, "native recovery size is invalid");
      result.kind = NativeLifecycleKind::kRecovery;
      result.related_object_id = ReadU32At(body, 4U);
      break;
    case 32921U:
      RequireSize(body, 12U, "native protection size is invalid");
      result.kind = NativeLifecycleKind::kProtection;
      break;
    case 32835U:
      RequireSize(body, 4U, "native death-animation start size is invalid");
      result.kind = NativeLifecycleKind::kDeathAnimationStarted;
      break;
    case 32836U:
      RequireSize(body, 4U, "native death-animation finish size is invalid");
      result.kind = NativeLifecycleKind::kDeathAnimationFinished;
      break;
    default:
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native lifecycle opcode is not recovered"};
  }
  result.actor_object_id = ReadU32At(body, 0U);
  return result;
}

NativeRespawnRequest DecodeNativeRespawn(std::uint16_t opcode,
                                         std::span<const std::uint8_t> body) {
  if (opcode == 704U || opcode == 705U) {
    // 7D4810/7D7F90 select a native map spawn through A08CD0, then 7EA3A0
    // writes the actor and spawn index without the optional 704 DWORD flag.
    RequireSize(body, opcode == 705U ? 8U : 12U,
                "native respawn request size is invalid");
    const auto flag = opcode == 705U ? 0U : ReadU32At(body, 8U);
    RequireBoolean(flag, "native respawn request flag is invalid");
    return {.actor_object_id = ReadU32At(body, 0U),
            .respawn_point = ReadU32At(body, 4U),
            .flag = flag != 0U,
            .completion = false};
  }
  if (opcode == 32926U) {
    RequireSize(body, 9U, "native respawn completion size is invalid");
    const auto flag = ReadU8At(body, 8U);
    if (flag > 1U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native respawn completion flag is invalid"};
    }
    return {.actor_object_id = ReadU32At(body, 0U),
            .respawn_point = ReadU32At(body, 4U),
            .flag = flag != 0U,
            .completion = true};
  }
  throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                      "native respawn opcode is not recovered"};
}

std::vector<std::uint8_t> EncodeNativeRespawnAuthorization(
    std::uint32_t actor_object_id, std::uint32_t respawn_point) {
  PacketWriter writer;
  writer.WriteU32(actor_object_id);
  writer.WriteU32(respawn_point);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  return writer.Release();
}

}  // namespace bf::protocol
