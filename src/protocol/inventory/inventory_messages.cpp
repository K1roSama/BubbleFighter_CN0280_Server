#include "protocol/inventory/inventory_messages.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "features/inventory/life_item.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/login/gateway_messages.h"

namespace bf::protocol {
namespace {

constexpr double kPermanentDisplayDays = 36'500.0;
constexpr double kSecondsPerDay = 86'400.0;
constexpr double kMaximumFiniteDisplayDays = 10'000.0;

[[noreturn]] void InvalidOutgoing(std::string_view message) {
  throw ProtocolError{ProtocolErrorCode::kInvalidFrame, message};
}

[[nodiscard]] std::uint32_t CheckedU32(std::uint64_t value,
                                       std::string_view message) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t CheckedU32(std::int64_t value,
                                       std::string_view message) {
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

void WriteEmptyVector(PacketWriter& writer) { writer.WriteU32(0U); }

void WriteZeroU32(PacketWriter& writer, std::size_t count) {
  for (std::size_t index = 0U; index < count; ++index) {
    writer.WriteU32(0U);
  }
}

[[nodiscard]] double RemainingDays(
    const application::PlayerMenuSnapshot& snapshot,
    const application::PlayerItemView& item) {
  if (!item.expires_at.has_value()) {
    return kPermanentDisplayDays;
  }
  const auto remaining =
      std::chrono::duration<double>{*item.expires_at - snapshot.observed_at}
          .count() /
      kSecondsPerDay;
  if (!std::isfinite(remaining) || remaining <= 0.0 ||
      remaining > kMaximumFiniteDisplayDays) {
    InvalidOutgoing(
        "inventory expiry cannot be represented by the verified client display");
  }
  return remaining;
}

void ValidateSnapshotIdentity(const application::PlayerMenuSnapshot& snapshot) {
  if (!snapshot.player_id.IsValid() || snapshot.nickname.empty()) {
    InvalidOutgoing("player menu snapshot has no valid identity");
  }
}

void WriteItemAttributes(PacketWriter& writer,
                         const application::PlayerItemView& item) {
  if (!domain::ValidNativeItemAttributes(item.item_id.Value(),
                                         item.attributes) ||
      (!item.attributes.empty() &&
       (item.quantity != 1 || item.expires_at || item.pet_growth ||
        item.slot_code.empty()))) {
    InvalidOutgoing(
        "inventory dynamic attributes do not match their owning permanent item");
  }
  writer.WriteU32(static_cast<std::uint32_t>(item.attributes.size()) +
                  (item.pet_growth.has_value() ? 1U : 0U));
  for (const auto& attribute : item.attributes) {
    writer.WriteU32(attribute.source_item_id);
    writer.WriteU32(attribute.definition_id);
    writer.WriteU32(static_cast<std::uint32_t>(attribute.values.size()));
    for (const auto value : attribute.values)
      writer.WriteU32(static_cast<std::uint32_t>(value));
    writer.WriteU32(attribute.category);
    writer.WriteUtf16(Utf8ToUtf16(attribute.label));
  }
  if (!item.pet_growth.has_value()) {
    return;
  }
  const auto& growth = *item.pet_growth;
  if (item.slot_code != "pet" || growth.level > 2U ||
      growth.experience > 100U) {
    InvalidOutgoing(
        "inventory pet growth is outside the verified native range");
  }
  std::uint64_t date_ticks = 0U;
  if (growth.fed_until.has_value()) {
    const double date = UnixSecondsToOleDate(
        std::chrono::duration<double>{growth.fed_until->time_since_epoch()}
            .count());
    if (!std::isfinite(date) || date < 0.0 || date > 1'000'000.0) {
      InvalidOutgoing("pet feeding deadline exceeds native OLE date range");
    }
    date_ticks = static_cast<std::uint64_t>(std::ceil(date * 100'000.0));
  }
  // 6DD740 属性实例、目录索引、DWORD vector、分类、wstring；7B0F00 读取属性 7
  // 的四项。
  writer.WriteU32(CheckedU32(item.item_instance_id.Value(),
                             "pet attribute ID exceeds uint32"));
  writer.WriteU32(7U);
  writer.WriteU32(4U);
  writer.WriteU32(growth.level);
  writer.WriteU32(growth.experience);
  writer.WriteU32(static_cast<std::uint32_t>(date_ticks / 100'000U));
  writer.WriteU32(static_cast<std::uint32_t>(date_ticks % 100'000U));
  writer.WriteU32(0U);
  writer.WriteUtf16(u"");
}

// 470120 reads two complete FighterInfo objects even when S2C125 reports
// failure. Only fields tied to recovered UI uses are populated. Unmapped
// mode-specific records remain pending protocol work; they are not aggregate
// "all games" totals.
void WriteFighterRecord(PacketWriter& writer,
                        const application::PlayerMenuSnapshot* snapshot) {
  writer.WriteU32(snapshot ? CheckedU32(snapshot->player_id.Value(),
                                        "fighter ID exceeds uint32")
                           : 0U);
  writer.WriteUtf16(snapshot ? Utf8ToUtf16(snapshot->nickname) : u"");
  writer.WriteU32(snapshot
                      ? static_cast<std::uint32_t>(snapshot->initial_fighter)
                      : 0U);  // +32: model fallback
  writer.WriteU32(snapshot ? CheckedU32(snapshot->progress.level,
                                        "fighter level exceeds uint32")
                           : 0U);  // +36
  writer.WriteU32(0U);             // +40 escapes
  const auto* couple =
      snapshot && snapshot->couple ? &*snapshot->couple : nullptr;
  writer.WriteU32(couple ? 1U : 0U);  // +44
  writer.WriteF64(
      couple ? UnixSecondsToOleDate(std::chrono::duration<double>(
                                        couple->since.time_since_epoch())
                                        .count())
             : 0.0);                                    // +48
  writer.WriteU32(couple ? couple->ring_item_id : 0U);  // +56, BDFA80 -> D30C50
  writer.WriteU32(0U);                          // +60, reserved event flag
  std::array<std::uint32_t, 49U> statistics{};  // native offsets +64 .. +256
  auto add = [&statistics](std::size_t offset, std::uint64_t amount) {
    auto& value = statistics[(offset - 64U) / 4U];
    constexpr auto maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    if (amount > maximum - value) {
      throw ProtocolError{ProtocolErrorCode::kLengthLimit,
                          "fighter statistics exceed signed DWORD"};
    }
    value += static_cast<std::uint32_t>(amount);
  };
  if (snapshot) {
    for (const auto& mode : snapshot->mode_statistics) {
      // This server groups its team-pang variants on the native team page.
      // Star-team has its own original UI page; other modes must not leak here.
      std::size_t base = 0U;
      switch (mode.mode_id) {
        case 1U:
        case 3U:
        case 12U:
        case 14U:
        case 21U:
        case 27U:
          base = 68U;
          break;
        case 6U:
          base = 128U;
          break;
        case 8U:
        case 28U:
          // BDB170 -> lblTab4ZombiePlayCount. Role-specific victories cannot be
          // reconstructed from the ordinary win/loss totals in this snapshot.
          add(164U, mode.matches);
          continue;
        case 16U:
          // BD3150/BDB170: lbTab7GamesPlayed, lbTab7WinCnt, lbTab7LoseCnt.
          // Pirate/Horde modes 101/102 have their own rules and do not enter
          // here.
          add(212U, mode.matches);
          add(216U, mode.wins);
          add(220U, mode.losses);
          continue;
        default:
          continue;
      }
      add(base, mode.wins);
      add(base + 4U, mode.losses);
      add(base + 8U, mode.draws);
      add(base + 12U, mode.bubbles);
      add(base + 16U, mode.pangs);
      add(base + 20U, mode.helps);
    }
  }
  for (const auto value : statistics) writer.WriteU32(value);
  WriteEmptyVector(writer);  // +268 EloRatingData
  writer.WriteU32(snapshot ? snapshot->rating : 1'000U);
  writer.WriteU32(1U);
  writer.WriteU32(0U);
  WriteZeroU32(writer, 2U);  // ArenaLevel
  WriteZeroU32(writer, 2U);
  WriteZeroU32(writer, 2U);  // PersonalGameLevel
  WriteZeroU32(writer, 8U);  // SocialSystemInfo, absent on this server
  writer.WriteU64(0U);
  writer.WriteU32(0U);
  WriteEmptyVector(writer);
  WriteZeroU32(writer, 8U);
  WriteEmptyVector(writer);
  writer.WriteU32(0U);
  writer.WriteUtf16(snapshot ? Utf8ToUtf16(snapshot->clan_name) : u"");  // +436
  writer.WriteU32(snapshot ? snapshot->clan_mark : 0U);                  // +368
}

}  // namespace

ShopCatalogRequest DecodeShopCatalogRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const ShopCatalogRequest request{.cached_revision = reader.ReadU64(),
                                   .selection = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

CashShopCatalogRequest DecodeCashShopCatalogRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const CashShopCatalogRequest request{.cached_revision = reader.ReadU64()};
  reader.RequireEnd();
  return request;
}

InventorySnapshotRequest DecodeInventorySnapshotRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
  return {};
}

WeaponPresetRequest DecodeWeaponPresetRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const WeaponPresetRequest request{.object_id = reader.ReadU32(),
                                    .preset = reader.ReadU8()};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeWeaponPresetResult(std::uint8_t preset,
                                                   bool success) {
  PacketWriter writer;
  writer.WriteU8(preset);
  writer.WriteU32(success ? 0U : 1U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeWeaponPresetsResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  if (snapshot.active_weapon_preset > 2U) {
    InvalidOutgoing("active weapon preset exceeds verified client pages");
  }
  PacketWriter writer;
  writer.WriteU8(snapshot.active_weapon_preset);
  writer.WriteU32(CheckedU32(snapshot.player_id.Value(),
                             "preset player ID exceeds uint32"));
  writer.WriteU32(5U);
  for (std::size_t index = 0U; index < snapshot.weapon_presets.size();
       ++index) {
    const auto& preset = snapshot.weapon_presets[index];
    writer.WriteU8(0U);
    writer.WriteU8(index == 0U || (index < 3U && preset.enabled) ? 1U : 0U);
    writer.WriteU32(
        CheckedU32(preset.primary.Value(), "preset primary exceeds uint32"));
    writer.WriteU32(CheckedU32(preset.secondary.Value(),
                               "preset secondary exceeds uint32"));
  }
  WriteEmptyVector(writer);
  return writer.Release();
}

CashBalanceRequest DecodeCashBalanceRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
  return {};
}

EquipmentChangeRequest DecodeEquipmentChangeRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const EquipmentChangeRequest request{.item_id = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

ExpiredItemDiscardRequest DecodeExpiredItemDiscardRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  ExpiredItemDiscardRequest r{reader.ReadU32()};
  reader.RequireEnd();
  if (r.native_item_key == 0U || r.native_item_key > 2'147'483'647U)
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "expired discard native key is not signed-positive"};
  return r;
}

std::vector<std::uint8_t> EncodeExpiredItemDiscardResponse(
    std::uint32_t native_key, bool success) {
  if (native_key == 0U || native_key > 2'147'483'647U)
    InvalidOutgoing("expired discard native key invalid");
  PacketWriter writer;
  writer.WriteU32(native_key);
  writer.WriteU32(success ? 0U : 1U);
  return writer.Release();
}

PurchaseOfferRequest DecodePurchaseOfferRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const PurchaseOfferRequest request{.stock_id = reader.ReadU32(),
                                     .gift_flag = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeEquipmentAppliedResponse(
    const EquipmentAppliedResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.result);
  writer.WriteU32(response.item_id);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeEquipmentRemovedResponse(
    const EquipmentRemovedResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.result);
  return writer.Release();
}

std::vector<std::uint8_t> EncodePurchaseOfferResponse(
    const PurchaseOfferResponse& response) {
  PacketWriter writer;
  writer.WriteU32(response.stock_id);
  writer.WriteU32(response.result);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeInventorySnapshotResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  PacketWriter writer;
  writer.WriteU32(
      CheckedU32(snapshot.items.size() + snapshot.expired_items.size(),
                 "inventory vector exceeds uint32 count"));
  std::vector<std::pair<const application::PlayerItemView*, bool>> rows;
  for (const auto& item : snapshot.items) rows.emplace_back(&item, false);
  for (const auto& item : snapshot.expired_items)
    rows.emplace_back(&item, true);
  std::ranges::sort(rows, {}, [](const auto& row) {
    return row.first->item_instance_id.Value();
  });
  for (const auto& [row, expired] : rows) {
    const auto& item = *row;
    const bool depleted_life =
        item.quantity == 0 && domain::IsLifeItem(item.item_id.Value()) &&
        !item.expires_at && !item.equipped && !item.pet_growth && !expired;
    if (!item.item_id.IsValid() || (item.quantity <= 0 && !depleted_life)) {
      InvalidOutgoing("inventory item has invalid ID or quantity");
    }
    writer.WriteU32(CheckedU32(item.item_id.Value(),
                               "item ID exceeds legacy protocol range"));
    writer.WriteU32(CheckedU32(item.quantity,
                               "item quantity exceeds legacy protocol range"));
    if (expired &&
        (!item.expires_at || *item.expires_at > snapshot.observed_at ||
         *item.expires_at <= item.acquired_at))
      InvalidOutgoing("expired inventory row lacks a valid expired timestamp");
    writer.WriteF64(expired ? 0.0 : RemainingDays(snapshot, item));
    writer.WriteUtf16(u"");
    WriteItemAttributes(writer, item);
    writer.WriteU8(static_cast<std::uint8_t>((item.equipped ? 1U : 0U) |
                                             (expired ? 2U : 0U)));
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeEquippedItemsResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  std::uint32_t active_character = 0U;
  std::size_t equipped_count = 0U;
  for (const auto& item : snapshot.items) {
    if (!item.equipped) {
      continue;
    }
    if (item.quantity <= 0)
      InvalidOutgoing("depleted item cannot enter native equipment");
    ++equipped_count;
    const auto item_id =
        CheckedU32(item.item_id.Value(), "equipped item ID exceeds uint32");
    if (item.slot_code == "character") {
      if (active_character != 0U) {
        InvalidOutgoing("snapshot contains more than one equipped character");
      }
      active_character = item_id;
    }
  }

  PacketWriter writer;
  writer.WriteU32(CheckedU32(snapshot.player_id.Value(),
                             "player ID exceeds legacy protocol"));
  writer.WriteU32(active_character);
  writer.WriteU32(snapshot.color_code);
  writer.WriteU32(
      CheckedU32(equipped_count, "equipment vector exceeds uint32 count"));
  for (const auto& item : snapshot.items) {
    if (item.equipped) {
      writer.WriteU32(
          CheckedU32(item.item_id.Value(), "equipped item ID exceeds uint32"));
    }
  }
  WriteEmptyVector(writer);
  WriteEmptyVector(writer);
  WriteEmptyVector(writer);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeCashBalanceResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  PacketWriter writer;
  writer.WriteU32(0U);
  writer.WriteU32(CheckedU32(snapshot.wallets.bubble_cash,
                             "bubble cash exceeds legacy protocol range"));
  writer.WriteU32(0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodePlayerProfileResponse(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  PacketWriter writer;
  writer.WriteU32(snapshot.color_code);
  writer.WriteU32(
      CheckedU32(snapshot.progress.experience, "experience exceeds uint32"));
  writer.WriteU32(
      CheckedU32(snapshot.progress.level_base, "level base exceeds uint32"));
  writer.WriteU32(CheckedU32(snapshot.progress.level_next,
                             "level threshold exceeds uint32"));
  writer.WriteU32(CheckedU32(snapshot.wallets.lucci, "lucci exceeds uint32"));
  writer.WriteU32(
      CheckedU32(snapshot.wallets.bubble_cash, "bubble cash exceeds uint32"));
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteUtf16(u"");
  writer.WriteU16(0U);
  writer.WriteU32(0U);

  WriteFighterRecord(writer, &snapshot);
  return writer.Release();
}

FighterInfoRequest DecodeFighterInfoRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  FighterInfoRequest request{.target = domain::PlayerId{reader.ReadU32()},
                             .requester = domain::PlayerId{reader.ReadU32()}};
  reader.RequireEnd();
  if (!request.target.IsValid() || !request.requester.IsValid()) {
    throw ProtocolError{ProtocolErrorCode::kInvalidValue,
                        "fighter query requires nonzero identities"};
  }
  return request;
}

FighterTitleRequest DecodeFighterTitleRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  FighterTitleRequest request{.target = domain::PlayerId{reader.ReadU32()}};
  reader.RequireEnd();
  if (!request.target.IsValid()) {
    throw ProtocolError{
        ProtocolErrorCode::kInvalidValue,
        "fighter title query requires a nonzero target identity"};
  }
  return request;
}

std::vector<std::uint8_t> EncodeFighterInfoResponse(
    const application::PlayerMenuSnapshot* snapshot,
    const application::PlayerMenuSnapshot* partner) {
  if (snapshot) ValidateSnapshotIdentity(*snapshot);
  PacketWriter writer;
  writer.WriteU32(snapshot ? 1U : 0U);
  WriteFighterRecord(writer, snapshot);
  WriteFighterRecord(writer, partner);
  // The native profile dialog compares this with the nickname from S2C3.
  writer.WriteUtf16(snapshot ? Utf8ToUtf16(snapshot->nickname) : u"");
  writer.WriteUtf16(u"");  // parsed but not used by B78070
  writer.WriteU32(snapshot ? CheckedU32(snapshot->progress.experience,
                                        "fighter XP exceeds uint32")
                           : 0U);
  writer.WriteU32(snapshot ? CheckedU32(snapshot->progress.level_base,
                                        "fighter base exceeds uint32")
                           : 0U);
  writer.WriteU32(snapshot ? CheckedU32(snapshot->progress.level_next,
                                        "fighter threshold exceeds uint32")
                           : 0U);
  writer.WriteU32(snapshot ? snapshot->color_code
                           : 0U);  // BDB170 -> 859570 character color
  return writer.Release();
}

std::vector<std::uint8_t> EncodeFighterRecord(
    const application::PlayerMenuSnapshot& snapshot) {
  ValidateSnapshotIdentity(snapshot);
  PacketWriter writer;
  WriteFighterRecord(writer, &snapshot);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeFighterTitleResponse(
    domain::PlayerId target, std::span<const FighterMedalProgress> medals) {
  if (!target.IsValid()) {
    InvalidOutgoing("fighter medal query requires a target");
  }
  PacketWriter writer;
  writer.WriteU32(CheckedU32(target.Value(), "fighter target exceeds uint32"));
  writer.WriteU32(1U);
  writer.WriteU32(
      CheckedU32(medals.size(), "fighter medal vector exceeds uint32"));
  std::uint32_t previous = 0U;
  for (const auto& medal : medals) {
    if (medal.medal_id <= previous || medal.count > 0x7FFFFFFFU) {
      InvalidOutgoing(
          "fighter medals require ascending IDs and nonnegative signed counts");
    }
    writer.WriteU32(medal.medal_id);
    writer.WriteU32(medal.count);
    previous = medal.medal_id;
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeFighterTitleFailure(
    domain::PlayerId target, FighterTitleFailure failure) {
  if (!target.IsValid() || (failure != FighterTitleFailure::kUnavailable &&
                            failure != FighterTitleFailure::kTargetMissing)) {
    InvalidOutgoing("invalid fighter medal failure");
  }
  PacketWriter writer;
  writer.WriteU32(CheckedU32(target.Value(), "fighter target exceeds uint32"));
  writer.WriteU32(static_cast<std::uint32_t>(failure));
  writer.WriteU32(0U);
  return writer.Release();
}

}  // namespace bf::protocol
