#include "protocol/shop/treasure_messages.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

#include "protocol/codec/packet_io.h"
#include "protocol/shop/shop_catalog_messages.h"

namespace bf::protocol {
namespace {

// AB4E50 exposes only groups 0..3, and AB4950 opens group 3 initially.
// The published rule is one flat pool without rarity tiers. Put the entire
// pool in that initial group; a reward ordinal is not a native group key.
constexpr std::uint32_t kPreviewGroup = 3U;

[[noreturn]] void Invalid(std::string_view message) {
  throw ProtocolError{ProtocolErrorCode::kInvalidValue, message};
}

std::uint32_t CheckedU32(std::uint64_t value, std::string_view message) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

std::uint32_t CheckedCount(std::size_t value, std::string_view message) {
  return CheckedU32(value, message);
}

void WriteStockVector(
    PacketWriter& writer,
    const std::array<application::ServiceRewardRule, 5U>& rewards) {
  writer.WriteU32(
      CheckedCount(rewards.size(), "treasure reward count exceeds uint32"));
  for (const auto& reward : rewards) {
    writer.WriteU32(CheckedU32(reward.stock_id.Value(),
                               "treasure stock ID exceeds uint32"));
  }
}

}  // namespace

TreasureStateRequest DecodeTreasureStateRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  reader.RequireEnd();
  return {};
}

TreasureOpenRequest DecodeTreasureOpenRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  TreasureOpenRequest request{.needle_item_id = reader.ReadU32(),
                              .primary_position = reader.ReadU8(),
                              .positions = {}};
  const auto count = reader.ReadCount(5U);
  request.positions.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    request.positions.push_back(reader.ReadU32());
  }
  reader.RequireEnd();
  return request;
}

TreasureClaimRequest DecodeTreasureClaimRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const TreasureClaimRequest request{.choice = reader.ReadU32(),
                                     .position = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

TreasurePreviewRequest DecodeTreasurePreviewRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const TreasurePreviewRequest request{.needle_item_id = reader.ReadU32()};
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeTreasurePreviewResponse(
    std::uint32_t needle_item_id,
    const application::BubbleServiceRules& rules) {
  // AB5270 displays group probability in tenths of a percent.
  const bool enabled =
      std::ranges::any_of(rules.needles, [needle_item_id](const auto& needle) {
        return needle.item_id.Value() == needle_item_id;
      });
  const auto count = enabled ? CheckedCount(rules.reward_pool.size(),
                                            "treasure pool too large")
                             : 0U;
  PacketWriter writer;
  writer.WriteU32(4U);
  for (std::uint32_t group = 0U; group < 4U; ++group) {
    writer.WriteU32(count != 0U && group == kPreviewGroup ? 1000U : 0U);
  }
  writer.WriteU32(needle_item_id);
  writer.WriteU32(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    writer.WriteU32(kPreviewGroup);
    writer.WriteU32(1U);
    writer.WriteU32(CheckedU32(rules.reward_pool[index].stock_id.Value(),
                               "treasure stock too large"));
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTreasureStateResponse(
    const application::TreasureStateSnapshot& state,
    const application::BubbleServiceRules& rules,
    const application::ShopCardView& needle_card) {
  if (!state.player_id.IsValid() ||
      state.needle_counts.size() != rules.needles.size() ||
      needle_card.item_id.Value() != 100382U) {
    Invalid("treasure state identity, needle count or card is invalid");
  }
  PacketWriter writer;
  writer.WriteU32(CheckedCount(state.needle_counts.size(),
                               "treasure needle count exceeds uint32"));
  for (std::size_t index = 0U; index < state.needle_counts.size(); ++index) {
    const auto& count = state.needle_counts[index];
    if (count.item_id != rules.needles[index].item_id) {
      Invalid("treasure needle state order differs from published rules");
    }
    writer.WriteU32(
        CheckedU32(count.item_id.Value(), "treasure needle ID exceeds uint32"));
    writer.WriteU32(count.quantity);
  }
  writer.WriteU32(CheckedCount(rules.needles.size(),
                               "treasure preview count exceeds uint32"));
  for (const auto& needle : rules.needles) {
    writer.WriteU32(CheckedU32(needle.item_id.Value(),
                               "treasure needle ID exceeds uint32"));
    writer.WriteU32(CheckedCount(rules.reward_pool.size(),
                                 "treasure reward pool exceeds uint32"));
    for (std::size_t index = 0U; index < rules.reward_pool.size(); ++index) {
      writer.WriteU32(kPreviewGroup);
      writer.WriteU32(1U);
      writer.WriteU32(CheckedU32(rules.reward_pool[index].stock_id.Value(),
                                 "treasure preview stock exceeds uint32"));
    }
  }
  const auto super =
      std::ranges::find(state.needle_counts, rules.super_needle_item_id,
                        &application::TreasureNeedleCount::item_id);
  if (super == state.needle_counts.end()) {
    Invalid("treasure state misses super needle");
  }
  writer.WriteU32(CheckedU32(rules.super_needle_item_id.Value(),
                             "treasure super needle exceeds uint32"));
  writer.WriteU32(super->quantity);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteU32(state.bubble_balance);
  const auto card = EncodeShopCard(needle_card);
  writer.WriteBytes(card);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTreasureRevealResponse(
    const application::TreasureDrawSnapshot& draw) {
  if (draw.draw_id == 0U || !draw.player_id.IsValid() ||
      !draw.needle_item_id.IsValid() || draw.positions[0] > 4U ||
      draw.positions[1] > 4U || draw.positions[0] == draw.positions[1] ||
      draw.selection_duration <= std::chrono::seconds::zero() ||
      draw.selection_duration.count() >
          std::numeric_limits<std::uint32_t>::max()) {
    Invalid("treasure reveal snapshot is invalid");
  }
  std::set<std::uint64_t> stocks;
  for (const auto& reward : draw.rewards) {
    if (!reward.stock_id.IsValid() ||
        !stocks.insert(reward.stock_id.Value()).second) {
      Invalid("treasure reveal stocks are invalid or duplicate");
    }
  }
  PacketWriter writer;
  writer.WriteU8(7U);
  writer.WriteU8(0U);
  writer.WriteU32(CheckedU32(draw.needle_item_id.Value(),
                             "treasure needle ID exceeds uint32"));
  writer.WriteU32(draw.needle_quantity_after);
  writer.WriteU32(0U);
  writer.WriteU32(draw.bubble_balance_after);
  writer.WriteU8(0U);
  WriteStockVector(writer, draw.rewards);
  writer.WriteU32(CheckedCount(draw.positions.size(),
                               "treasure position count exceeds uint32"));
  for (const auto position : draw.positions) {
    writer.WriteU32(position);
  }
  writer.WriteU32(static_cast<std::uint32_t>(draw.selection_duration.count()));
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTreasureClaimResponse(
    const application::TreasureClaimResult& result) {
  if (!result.player_id.IsValid() || result.draw_id == 0U ||
      result.position > 4U) {
    Invalid("treasure claim result is invalid");
  }
  PacketWriter writer;
  writer.WriteU8(8U);
  writer.WriteU8(static_cast<std::uint8_t>(result.position));
  writer.WriteU32(CheckedCount(result.stocks.size(),
                               "treasure stock count exceeds uint32"));
  for (const auto stock : result.stocks) {
    if (!stock.IsValid()) {
      Invalid("treasure claim contains invalid stock");
    }
    writer.WriteU32(
        CheckedU32(stock.Value(), "treasure stock ID exceeds uint32"));
  }
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTreasureOpenFailure() {
  PacketWriter writer;
  writer.WriteU8(2U);
  writer.WriteU8(0U);
  for (std::size_t index = 0U; index < 4U; ++index) {
    writer.WriteU32(0U);
  }
  writer.WriteU8(0U);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  writer.WriteU32(0U);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeTreasureClaimFailure() {
  PacketWriter writer;
  writer.WriteU8(1U);
  writer.WriteU8(0U);
  writer.WriteU32(0U);
  return writer.Release();
}

}  // namespace bf::protocol
