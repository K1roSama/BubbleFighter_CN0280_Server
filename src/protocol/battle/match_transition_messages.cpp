#include "protocol/battle/match_transition_messages.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

constexpr std::size_t kMaximumTextCodeUnits = 256U;
constexpr float kMaximumAbsoluteCoordinate = 10'000'000.0F;

std::uint32_t ContributionScore(
    const domain::MatchPerformance& performance) noexcept {
  return static_cast<std::uint32_t>(
      std::min(static_cast<std::uint64_t>(performance.bubbles) +
                   2ULL * performance.pangs + 2ULL * performance.helps,
               static_cast<std::uint64_t>(domain::kNativeSignedDwordMaximum)));
}

std::uint32_t NativeEvaluation(
    const domain::MatchPerformance& performance) noexcept {
  const auto points = ContributionScore(performance);
  // 用户确认的初始规则；原版 Rank_list 图集的 A/B/C/D 帧为 3/4/5/6。
  return points >= 20U ? 3U : points >= 10U ? 4U : points >= 5U ? 5U : 6U;
}

[[nodiscard]] std::string ReadText(PacketReader& reader) {
  return Utf16ToUtf8(reader.ReadUtf16(kMaximumTextCodeUnits));
}

void WriteText(PacketWriter& writer, std::string_view text) {
  const auto utf16 = Utf8ToUtf16(text);
  if (utf16.size() > kMaximumTextCodeUnits) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                        "match transition text exceeds 256 UTF-16 code units"};
  }
  writer.WriteUtf16(utf16);
}

[[nodiscard]] bool ReadBooleanDword(PacketReader& reader) {
  const auto value = reader.ReadU32();
  if (value > 1U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "match transition boolean DWORD must be zero or one"};
  }
  return value != 0U;
}

[[nodiscard]] std::array<std::uint32_t, kMatchRuleFieldCount> ReadMatchRules(
    PacketReader& reader) {
  std::array<std::uint32_t, kMatchRuleFieldCount> result{};
  for (auto& value : result) {
    value = reader.ReadU32();
  }
  return result;
}

void ValidateStartKeys(std::string_view map_key, std::string_view mode_key) {
  if (map_key.empty() || mode_key.empty()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "match start map and mode keys must not be empty"};
  }
}

[[nodiscard]] std::vector<std::uint32_t> ReadU32Vector(PacketReader& reader) {
  const auto count = reader.ReadCount(kMapAssetVectorLimit);
  std::vector<std::uint32_t> result;
  result.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    result.push_back(reader.ReadU32());
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t> ReadU8Vector(PacketReader& reader) {
  const auto count = reader.ReadCount(kMapAssetVectorLimit);
  const auto bytes = reader.ReadBytes(count);
  return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::vector<MapAssetPosition> ReadPositionVector(
    PacketReader& reader) {
  const auto count = reader.ReadCount(kMapAssetVectorLimit);
  std::vector<MapAssetPosition> result;
  result.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    MapAssetPosition position{
        .x = reader.ReadF32(), .y = reader.ReadF32(), .z = reader.ReadF32()};
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) ||
        std::abs(position.x) > kMaximumAbsoluteCoordinate ||
        std::abs(position.y) > kMaximumAbsoluteCoordinate ||
        std::abs(position.z) > kMaximumAbsoluteCoordinate) {
      throw ProtocolError{
          ProtocolErrorCode::kInvalidValue,
          "map asset position is non-finite or outside the recovered bound"};
    }
    result.push_back(position);
  }
  return result;
}

[[nodiscard]] std::vector<std::string> ReadTextVector(PacketReader& reader) {
  const auto count = reader.ReadCount(kMapAssetVectorLimit);
  std::vector<std::string> result;
  result.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    result.push_back(ReadText(reader));
  }
  return result;
}

}  // namespace

std::vector<std::uint8_t> EncodeMatchStopResponse(std::uint8_t winner,
                                                  std::uint32_t mode,
                                                  bool timed_out) {
  if (winner > 2U && !((mode == 8U || mode == 28U) && winner == 34U) &&
      !(mode == 16U && winner == 35U)) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "native final winner is invalid"};
  }
  PacketWriter writer;
  writer.WriteU32(0U);
  // A779E0/A77B70: reason 2 selects time-over/FighterWin unconditionally.
  // Normal infection completion derives the winner from converted characters.
  writer.WriteU32((mode == 8U || mode == 28U) ? (timed_out ? 2U : 1U) : winner);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeNativeScoreResponse(
    std::span<const NativeScorePlayer> players, std::uint8_t winner,
    std::optional<std::array<std::uint32_t, 2U>> round_scores) {
  const bool pve =
      !players.empty() && std::ranges::all_of(players, [](const auto& row) {
        return row.mode == 16U;
      });
  const bool infection =
      !players.empty() && std::ranges::all_of(players, [](const auto& row) {
        return row.mode == 8U || row.mode == 28U;
      });
  const bool weapon_master = std::ranges::any_of(
      players, [](const auto& row) { return row.mode == 26U; });
  if (weapon_master &&
      ((winner != 255U && winner != 0U) || round_scores.has_value() ||
       !std::ranges::all_of(players,
                            [](const auto& row) { return row.mode == 26U; }))) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "weapon master requires an individual roster without team round scores"};
  }
  if (players.empty() || players.size() > 36U ||
      (winner > 2U && winner != 255U && !(infection && winner == 34U) &&
       !(pve && winner == 35U))) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "native score roster or winner is invalid"};
  }
  std::vector<NativeScorePlayer> rows{players.begin(), players.end()};
  std::ranges::sort(rows, {}, &NativeScorePlayer::seat);
  std::set<std::uint32_t> player_ids;
  std::uint64_t mask = 0U;
  std::array<std::uint64_t, 2U> scores{};
  for (const auto& row : rows) {
    if ((row.mode == 26U) != row.weapon_master.has_value()) {
      throw ProtocolError{
          ProtocolErrorCode::kInvalidValue,
          "weapon master score requires explicit mode progress"};
    }
    if (row.weapon_master) ValidateWeaponMasterProgress(*row.weapon_master);
    if (row.seat >= 36U || row.team < 1U ||
        (row.team > 2U &&
         !((row.mode == 13U || row.mode == 23U) && row.team <= 16U) &&
         !(infection && row.team == 34U) && !(pve && row.team == 35U)) ||
        row.player_id == 0U || row.level == 0U || row.level > 1000U ||
        row.nickname.empty() || !player_ids.insert(row.player_id).second ||
        (mask & (std::uint64_t{1} << row.seat)) != 0U) {
      throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                          "native score player is invalid"};
    }
    mask |= std::uint64_t{1} << row.seat;
    if (row.mode == 13U || row.mode == 23U)
      continue;  // Individual factions are not red/blue aggregate score
                 // columns.
    scores[row.team == 34U   ? 1U
           : row.team == 35U ? 0U
                             : row.team - 1U] +=
        row.mode == 5U || row.mode == 6U || row.mode == 7U
            ? row.performance.objective_points
            : row.performance.pangs;
  }
  auto podium = rows;
  std::ranges::sort(podium, [](const auto& left, const auto& right) {
    const auto key = [](const auto& row) {
      const auto points = row.mode == 26U ? row.weapon_master->experience
                          : row.mode == 5U || row.mode == 6U || row.mode == 7U
                              ? row.performance.objective_points
                              : row.performance.pangs;
      return std::tuple{
          row.mode == 23U && row.surviving,
          points,
          row.performance.bubbles,
          row.performance.helps,
          std::numeric_limits<std::uint32_t>::max() - row.performance.deaths,
          35U - row.seat};
    };
    return key(left) > key(right);
  });
  PacketWriter writer;
  writer.WriteU8(2U);
  writer.WriteU8(winner);
  writer.WriteU32(round_scores.has_value() && winner != 255U ? 1U : 0U);
  for (std::size_t team = 0U; team < scores.size(); ++team) {
    const bool soccer = rows.front().mode == 11U;
    const auto score =
        soccer && round_scores ? (*round_scores)[team] : scores[team];
    const auto bounded = static_cast<std::uint32_t>(std::min(
        score, static_cast<std::uint64_t>(domain::kNativeSignedDwordMaximum)));
    writer.WriteU32(bounded);
    writer.WriteU32(bounded);
    // 482880 的第三列写入 ScoreData+308/+312；A4CAC0 读取它绘制回合胜场。
    writer.WriteU32(!soccer && round_scores.has_value() ? (*round_scores)[team]
                                                        : 0U);
  }
  writer.WriteU64(mask);
  for (const auto& row : rows) {
    writer.WriteU32(1U);
    writer.WriteU32(row.player_id);
    WriteText(writer, row.nickname);
    writer.WriteU8(row.team);
    // 482880 的目标 ScorePlayer 偏移；非连续字段必须保持原生读取顺序。
    constexpr std::array<std::uint16_t, 27U> offsets{
        56U,  60U,  64U,  68U,  72U,  76U,  80U,  84U,  88U,
        92U,  96U,  100U, 104U, 108U, 128U, 268U, 132U, 136U,
        140U, 308U, 312U, 316U, 320U, 324U, 328U, 332U, 372U};
    for (const auto offset : offsets) {
      std::uint32_t value = 0U;
      switch (offset) {
        case 56U:
          value = row.reward.has_value() ? row.reward->progression.level
                                         : row.level;
          break;
        case 60U:
          value = row.performance.bubbles;
          break;
        case 64U:
          value = row.performance.pangs;
          break;
        case 80U:
          // 个人贡献分与团队击爆胜负分分开：泡封 1，击爆 2，营救 2。
          value = row.mode == 5U || row.mode == 6U || row.mode == 7U
                      ? row.performance.objective_points
                      : ContributionScore(row.performance);
          break;
        case 128U:
          value = row.performance.loot_lucci +
                  (row.mode == 7U ? row.performance.objective_points : 0U);
          break;
        case 132U:
          value = row.mode == 5U || row.mode == 6U
                      ? row.performance.objective_points
                      : 0U;
          break;
        case 68U:
          value = row.performance.deaths;
          break;
        case 72U:
          value = row.performance.helps;
          break;
        case 308U:
          value = static_cast<std::uint32_t>(
              std::ranges::find(podium, row.player_id,
                                &NativeScorePlayer::player_id) -
              podium.begin());
          break;
        case 312U:
        case 316U:
        case 320U:
        case 324U:
        case 328U:
        case 332U:
          if (row.reward.has_value()) {
            const auto& reward = *row.reward;
            const std::int64_t amount =
                offset == 312U   ? reward.progression.level_base
                : offset == 316U ? reward.progression.experience
                : offset == 320U ? reward.progression.level_next
                : offset == 324U ? reward.applied_award.experience
                : offset == 328U ? reward.applied_award.lucci
                                 : static_cast<std::int64_t>(
                                       reward.progression.level > row.level);
            if (amount < 0 || amount > domain::kNativeSignedDwordMaximum) {
              throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                                  "native score reward exceeds range"};
            }
            value = static_cast<std::uint32_t>(amount);
          }
          break;
        default:
          break;
      }
      writer.WriteU32(std::min(value, static_cast<std::uint32_t>(
                                          domain::kNativeSignedDwordMaximum)));
    }
    writer.WriteU32(0U);
    WriteText(writer, {});
    constexpr std::array<std::uint16_t, 8U> tail_offsets{
        156U, 412U, 416U, 336U, 236U, 240U, 272U, 352U};
    for (const auto offset : tail_offsets) {
      writer.WriteU32(offset == 336U
                          ? (winner == 255U ? 0xFFFFFFFFU
                                            : NativeEvaluation(row.performance))
                          : 0U);
    }
    writer.WriteU16(0U);
    for (std::size_t index = 0U; index < 3U; ++index) {
      writer.WriteU32(0U);
    }
    writer.WriteU8(0U);
    writer.WriteU8(0U);
    // +444、ArenaLevel 两个 DWORD、+276..284 三个 DWORD。
    for (std::size_t index = 0U; index < 6U; ++index) {
      writer.WriteU32(0U);
    }
    const auto progress = row.weapon_master.value_or(WeaponMasterProgress{});
    writer.WriteU32(progress.level);       // ScorePlayer+288
    writer.WriteU32(progress.experience);  // ScorePlayer+292
    writer.WriteU32(progress.level_base);  // ScorePlayer+296
    writer.WriteU32(progress.level_next);  // ScorePlayer+300
    writer.WriteU32(progress.assists);  // 4E07D0 -> lbAssistCnt / lbMyAssistCnt
  }
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    writer.WriteU32(0U);
  }
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    writer.WriteU32(0xFFFFFFFFU);
  }
  return writer.Release();
}

StartMatchRequest DecodeStartMatchRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  StartMatchRequest request{.force = ReadBooleanDword(reader),
                            .map_key = ReadText(reader),
                            .display_name = ReadText(reader),
                            .mode_key = ReadText(reader),
                            .map_value = reader.ReadU32(),
                            .rules = ReadMatchRules(reader),
                            .final_flag = reader.ReadU8()};
  reader.RequireEnd();
  ValidateStartKeys(request.map_key, request.mode_key);
  return request;
}

StartRotatingMatchRequest DecodeStartRotatingMatchRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  StartRotatingMatchRequest request;
  request.force = ReadBooleanDword(reader);
  for (auto& map_key : request.map_keys) {
    map_key = ReadText(reader);
  }
  request.display_name = ReadText(reader);
  request.mode_key = ReadText(reader);
  request.map_value = reader.ReadU32();
  request.rules = ReadMatchRules(reader);
  request.final_flag = reader.ReadU8();
  reader.RequireEnd();
  ValidateStartKeys(request.map_keys.front(), request.mode_key);
  return request;
}

RoomConfigurationRequest DecodeRoomConfigurationRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  RoomConfigurationRequest request;
  request.display_name = ReadText(reader);
  request.map_key = ReadText(reader);
  request.auxiliary_label = ReadText(reader);
  request.family_key = ReadText(reader);
  request.option_319 = ReadBooleanDword(reader);
  request.option_320 = ReadBooleanDword(reader);
  request.selected_duration_ms = reader.ReadU32();
  request.score_target = reader.ReadU32();
  for (auto& value : request.reserved) {
    value = reader.ReadU32();
  }
  request.map_value = reader.ReadU32();
  request.mode_id = reader.ReadU32();
  request.submode_id = reader.ReadU32();
  request.automatic_option = ReadBooleanDword(reader);
  reader.RequireEnd();
  return request;
}

void ApplyRoomConfiguration(SceneConfiguration& destination,
                            const RoomConfigurationRequest& request) {
  destination.display_name = request.display_name;
  destination.map_key = request.map_key;
  destination.auxiliary_label = request.auxiliary_label;
  destination.family_key = request.family_key;
  destination.field_319 = request.option_319 ? 1U : 0U;
  destination.field_320 = request.option_320 ? 1U : 0U;
  destination.selected_duration_ms = request.selected_duration_ms;
  if (request.mode_id == 5U || request.mode_id == 6U) {
    destination.field_32 = request.selected_duration_ms;
  } else if (request.mode_id == 10U) {
    destination.field_28 = request.selected_duration_ms;
  } else {
    destination.field_24 = request.selected_duration_ms;
  }
  destination.score_target = request.score_target;
  destination.map_value = request.map_value;
  destination.mode_id = request.mode_id;
  destination.submode_id = request.submode_id;
  destination.field_12 = request.automatic_option ? 1U : 0U;
}

std::uint32_t ConfiguredMatchDurationMs(const SceneConfiguration& configuration,
                                        std::uint32_t fallback_ms) noexcept {
  // C2S513's selected duration is authoritative for this room. When omitted,
  // use the same cached slot as the native room dialog 98C040, not another
  // mode's slot or the duration frozen by the previous match.
  if (configuration.selected_duration_ms != 0U) {
    return configuration.selected_duration_ms;
  }
  std::uint32_t cached = configuration.field_24;
  switch (configuration.mode_id) {
    case 5U:
    case 6U:
      cached = configuration.field_32;
      break;
    case 10U:
      cached = configuration.field_28;
      break;
    case 8U:
    case 28U:
      return 180'000U;  // Native infection dialog defaults to three minutes.
    default:
      break;
  }
  return cached != 0U ? cached : fallback_ms;
}

RotatingMapSelectionRequest DecodeRotatingMapSelectionRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  RotatingMapSelectionRequest request{.index = reader.ReadU32(),
                                      .family_key = ReadText(reader),
                                      .map_key = ReadText(reader)};
  reader.RequireEnd();
  if (request.index >= 3U) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "rotating map index must be zero, one or two"};
  }
  return request;
}

void ApplyRotatingMapSelection(SceneConfiguration& destination,
                               const RotatingMapSelectionRequest& request) {
  if (request.index >= destination.rotating_maps.size()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "rotating map index must be zero, one or two"};
  }
  auto& selected = destination.rotating_maps[request.index];
  selected.family_key = request.family_key;
  selected.map_key = request.map_key;
}

LoadingCompleteRequest DecodeLoadingCompleteRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  LoadingCompleteRequest request;
  for (auto& value : request.values) {
    value = reader.ReadU32();
  }
  reader.RequireEnd();
  return request;
}

MapAssetsRequest DecodeMapAssetsRequest(std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  MapAssetsRequest request;
  request.object_count = reader.ReadU32();
  request.field_0 = ReadPositionVector(reader);
  request.field_1 = ReadU32Vector(reader);
  request.field_2 = ReadU8Vector(reader);
  request.field_3 = ReadU32Vector(reader);
  request.field_4 = ReadU32Vector(reader);
  request.field_5 = ReadU32Vector(reader);
  request.field_6 = ReadPositionVector(reader);
  request.field_7 = ReadU32Vector(reader);
  request.field_8 = ReadU32Vector(reader);
  request.field_9 = ReadU32Vector(reader);
  request.field_10 = ReadPositionVector(reader);
  request.field_11 = ReadU32Vector(reader);
  request.field_12 = ReadU32Vector(reader);
  request.field_13 = ReadTextVector(reader);
  reader.RequireEnd();
  return request;
}

PlayerObjectIdentityRequest DecodePlayerObjectIdentityRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const PlayerObjectIdentityRequest request{.object_id = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeRoomConfigurationResponse(
    const SceneConfiguration& configuration) {
  PacketWriter writer;
  EncodeSceneConfiguration(writer, configuration);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeStartSceneResponse(
    const StartSceneResponse& response) {
  ValidateStartKeys(response.map_key, response.mode_key);
  PacketWriter writer;
  WriteText(writer, response.map_key);
  WriteText(writer, response.mode_key);
  writer.WriteU32(response.stage);
  writer.WriteU32(response.round_index);
  writer.WriteI8(response.field_0);
  writer.WriteU8(response.field_1);
  writer.WriteU32(response.field_2);
  writer.WriteU8(response.field_3);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeActivateSlotResponse(std::uint32_t slot) {
  PacketWriter writer;
  writer.WriteU32(slot);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeLoadingOptionsResponse(
    const LoadingOptionsResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.first ? 1U : 0U);
  writer.WriteU32(response.second ? 1U : 0U);
  writer.WriteU32(response.third ? 1U : 0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCountdownResponse(std::uint32_t duration_ms) {
  PacketWriter writer;
  writer.WriteU32(duration_ms);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeMatchBeginResponse(
    const MatchBeginResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.duration_ms);
  writer.WriteU32(response.scene_id);
  writer.WriteU32(response.reserved);
  return writer.Release();
}

}  // namespace bf::protocol
