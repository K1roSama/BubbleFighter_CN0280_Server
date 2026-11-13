#include "protocol/battle/extreme_racing_messages.h"

#include <array>
#include <cmath>
#include <limits>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {
namespace {
constexpr auto kMaximumInt =
    static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());

void Require(bool valid) {
  if (!valid)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "invalid extreme racing field"};
}

void RequireStage(std::uint32_t stage) { Require(stage >= 1U && stage <= 5U); }

void RequireObject(std::uint32_t object) {
  Require(object > 0U && object <= kMaximumInt);
}

std::uint32_t ReadWord(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto value = reader.ReadU32();
  reader.RequireEnd();
  return value;
}
}  // namespace

std::uint32_t DecodeExtremeRacingFinish(std::span<const std::uint8_t> body) {
  const auto stage = ReadWord(body);
  RequireStage(stage);
  return stage;
}

std::uint32_t DecodeExtremeRacingCoin(std::span<const std::uint8_t> body) {
  const auto object = ReadWord(body);
  RequireObject(object);
  return object;
}

ExtremeRacingCheckpoint DecodeExtremeRacingCheckpoint(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const ExtremeRacingCheckpoint result{reader.ReadU32(), reader.ReadU32(),
                                       reader.ReadU32()};
  reader.RequireEnd();
  RequireObject(result.actor_object);
  RequireStage(result.stage);
  Require(result.checkpoint <= 9U);
  return result;
}

ExtremeRacingCannonProjectile DecodeExtremeRacingCannonProjectile(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  ExtremeRacingCannonProjectile result;
  result.actor_object = reader.ReadU32();
  result.weapon = reader.ReadU16();
  for (auto& component : result.position) component = reader.ReadF32();
  for (auto& component : result.direction) component = reader.ReadF32();
  result.cannon_object = reader.ReadU32();
  result.sequence = reader.ReadU32();
  reader.RequireEnd();
  RequireObject(result.actor_object);
  RequireObject(result.cannon_object);
  Require(result.weapon != 0U);
  for (const auto component : result.position)
    Require(std::isfinite(component));
  for (const auto component : result.direction)
    Require(std::isfinite(component));
  // The room must compare direction with the authorized cannon, including its
  // norm.
  Require(result.direction != std::array<float, 3U>{});
  return result;
}

std::vector<std::uint8_t> EncodeExtremeRacingTimer(ExtremeRacingTimer timer,
                                                   std::uint32_t duration_ms,
                                                   bool active) {
  Require(timer == ExtremeRacingTimer::kCountdown ||
          timer == ExtremeRacingTimer::kRetire ||
          timer == ExtremeRacingTimer::kStage);
  Require(duration_ms <= kMaximumInt && (!active || duration_ms > 0U));
  // 521390 starts another timer with duration - 1000 for the countdown
  // animation.
  Require(timer != ExtremeRacingTimer::kCountdown || !active ||
          duration_ms >= 1000U);
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(timer));
  writer.WriteU32(duration_ms);
  writer.WriteU32(active ? 1U : 0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeExtremeRacingStage(std::int32_t context,
                                                   std::uint32_t stage) {
  RequireStage(stage);
  PacketWriter writer;
  writer.WriteI32(context);
  writer.WriteU32(stage);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeExtremeRacingInitialization(
    const ExtremeRacingInitialization& initialization) {
  Require(initialization.coins.size() <= 1024U &&
          initialization.cannon_count <= 1024U &&
          initialization.grenade_count <= 1024U);
  // Local defensive transport limits, not inferred original map object counts.
  RequireObject(initialization.cannon_base);
  RequireObject(initialization.coin_base);
  RequireObject(initialization.grenade_base);
  Require(initialization.coin_base <=
          kMaximumInt - initialization.coins.size());
  for (const auto& coin : initialization.coins) {
    Require(coin.type < kMaximumInt);  // 5C73C0 receives type + 1.
    RequireStage(coin.stage);
  }
  PacketWriter writer;
  writer.WriteU32(initialization.cannon_base);
  writer.WriteU32(initialization.cannon_count);
  writer.WriteU32(initialization.coin_base);
  writer.WriteU32(static_cast<std::uint32_t>(initialization.coins.size()));
  for (unsigned field = 0U; field < 2U; ++field) {
    writer.WriteU32(static_cast<std::uint32_t>(initialization.coins.size()));
    auto object = initialization.coin_base;
    for (const auto& coin : initialization.coins) {
      writer.WriteU32(object++);
      writer.WriteU32(field == 0U ? coin.type : coin.stage);
    }
  }
  writer.WriteU32(initialization.grenade_base);
  writer.WriteU32(initialization.grenade_count);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeExtremeRacingRecords(
    ExtremeRacingUpdateKind kind,
    std::span<const ExtremeRacingRecordUpdate> updates) {
  Require(static_cast<std::uint32_t>(kind) <= 2U && !updates.empty() &&
          updates.size() <= 36U);
  std::array<bool, 36U> seats{};
  PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(kind));
  writer.WriteU32(static_cast<std::uint32_t>(updates.size()));
  for (const auto& update : updates) {
    Require(update.seat < seats.size() && !seats[update.seat] &&
            !update.fields.empty() && update.fields.size() <= 7U);
    seats[update.seat] = true;
    std::array<bool, 7U> fields{};
    writer.WriteU16(update.seat);
    writer.WriteU32(static_cast<std::uint32_t>(update.fields.size()));
    for (const auto& entry : update.fields) {
      const auto field = static_cast<std::uint8_t>(entry.field);
      Require(field < fields.size() && !fields[field]);
      fields[field] = true;
      if (entry.field == ExtremeRacingField::kInactive)
        Require(entry.value == 0 || entry.value == 1);
      if (entry.field == ExtremeRacingField::kIndicator)
        Require(entry.value >= std::numeric_limits<std::int16_t>::min() &&
                entry.value <= std::numeric_limits<std::int16_t>::max());
      writer.WriteU8(field);
      writer.WriteI32(entry.value);
    }
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeExtremeRacingHazards(
    const ExtremeRacingHazards& hazards) {
  Require(hazards.floor_patterns[0] <= 9U && hazards.floor_patterns[1] <= 9U);
  Require(hazards.vanishing_routes.size() <=
          1024U);  // Local defensive transport limit.
  PacketWriter writer;
  writer.WriteU32(hazards.floor_patterns[0]);
  writer.WriteU32(hazards.floor_patterns[1]);
  writer.WriteU32(hazards.grenade_seed);
  writer.WriteU32(static_cast<std::uint32_t>(hazards.vanishing_routes.size()));
  for (const auto route : hazards.vanishing_routes) {
    Require(route <=
            kMaximumInt);  // Native 5CCF00 compares these as signed indices.
    writer.WriteU32(route);
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeExtremeRacingCannonFire(
    const ExtremeRacingCannonFire& fire) {
  Require(fire.group <= 2U);
  RequireObject(fire.cannon_object);
  Require(fire.weapon > 0U &&
          fire.weapon <= std::numeric_limits<std::uint16_t>::max());
  Require(std::isfinite(fire.speed_multiplier) && fire.speed_multiplier > 0.0F);
  PacketWriter writer;
  writer.WriteU32(fire.group);
  writer.WriteU32(fire.cannon_object);
  writer.WriteU32(fire.weapon);
  writer.WriteF32(fire.speed_multiplier);
  return writer.Release();
}

}  // namespace bf::protocol
