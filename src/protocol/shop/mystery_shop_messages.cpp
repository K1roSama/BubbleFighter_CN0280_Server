#include "protocol/shop/mystery_shop_messages.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"

namespace bf::protocol {
namespace {

[[noreturn]] void Invalid(std::string_view message) {
  throw ProtocolError{ProtocolErrorCode::kInvalidValue, message};
}

std::uint32_t CheckedU32(std::uint64_t value, std::string_view message) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw ProtocolError{ProtocolErrorCode::kLengthLimit, message};
  }
  return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> EncodeResponse(
    MysteryAction action, std::uint8_t status,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now, std::uint32_t reward_stock_id) {
  if (selection.generation == 0U ||
      selection.offers.size() != rules.selection_size ||
      rules.selection_size > 32U ||
      rules.refresh_quantity > std::numeric_limits<std::uint16_t>::max() ||
      ((status == 0U && action == MysteryAction::kPurchase) !=
       (reward_stock_id != 0U)) ||
      (status != 0U && reward_stock_id != 0U)) {
    Invalid("mystery shop response state is invalid");
  }
  if (reward_stock_id != 0U &&
      !std::ranges::any_of(selection.offers,
                           [reward_stock_id](const auto& offer) {
                             return offer.stock_id.Value() == reward_stock_id;
                           })) {
    Invalid("mystery reward must be a selected Stock ID, not an item template");
  }
  const auto remaining_raw =
      std::chrono::ceil<std::chrono::seconds>(selection.expires_at - now)
          .count();
  const auto remaining = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
      remaining_raw, 0, std::numeric_limits<std::uint16_t>::max()));
  PacketWriter writer;
  writer.WriteU8(static_cast<std::uint8_t>(action));
  writer.WriteU8(status);
  writer.WriteU8(15U);
  writer.WriteU32(
      CheckedU32(selection.generation, "mystery generation exceeds uint32"));
  writer.WriteU32(CheckedU32(rules.refresh_item_id.Value(),
                             "mystery refresh item exceeds uint32"));
  writer.WriteU16(static_cast<std::uint16_t>(rules.refresh_quantity));
  writer.WriteU32(1U);
  writer.WriteU16(remaining);
  writer.WriteU32(CheckedU32(selection.offers.size(),
                             "mystery offer count exceeds uint32"));
  for (const auto& offer : selection.offers) {
    writer.WriteU32(
        CheckedU32(offer.stock_id.Value(), "mystery stock exceeds uint32"));
    writer.WriteUtf16(Utf8ToUtf16(offer.item_name));
    writer.WriteU32(
        CheckedU32(offer.stock_id.Value(), "mystery stock exceeds uint32"));
    writer.WriteU16(0U);
  }
  writer.WriteU32(selection.purchased_mask);
  if (reward_stock_id != 0U) {
    writer.WriteU32(reward_stock_id);
  }
  return writer.Release();
}

}  // namespace

MysteryShopRequest DecodeMysteryShopRequest(
    std::span<const std::uint8_t> body) {
  PacketReader reader{body};
  const auto raw_action = reader.ReadU8();
  if (raw_action < static_cast<std::uint8_t>(MysteryAction::kOpen) ||
      raw_action > static_cast<std::uint8_t>(MysteryAction::kPurchase)) {
    Invalid("unknown mystery shop action");
  }
  MysteryShopRequest request{.action = static_cast<MysteryAction>(raw_action),
                             .generation = reader.ReadU32(),
                             .stock_id = 0U,
                             .value = 0U};
  if (request.action == MysteryAction::kQuery ||
      request.action == MysteryAction::kRefresh) {
    request.value = reader.ReadU16();
  } else if (request.action == MysteryAction::kPurchase) {
    request.stock_id = reader.ReadU32();
    request.value = reader.ReadU16();
  }
  reader.RequireEnd();
  return request;
}

std::vector<std::uint8_t> EncodeMysteryShopQueryResponse(
    const MysteryShopRequest& request,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now) {
  if (request.action != MysteryAction::kQuery) {
    Invalid("mystery background response requires a query");
  }
  if (request.generation != selection.generation) {
    return EncodeMysteryShopResponse(MysteryAction::kQuery, selection, rules,
                                     now);
  }
  // 47E1D0 使用字段位图；695CC0 在 bit 2 更新倒计时后将查询阈值重置为 -1。
  // 每次携带完整计时数据会令 69B0A0 每五秒再查询，进房后更会反复触发错误通知。
  PacketWriter writer;
  writer.WriteU8(static_cast<std::uint8_t>(MysteryAction::kQuery));
  writer.WriteU8(0U);
  writer.WriteU8(request.value == 0U ? 10U : 8U);
  if (request.value == 0U) {
    const auto seconds =
        std::chrono::ceil<std::chrono::seconds>(selection.expires_at - now)
            .count();
    writer.WriteU16(static_cast<std::uint16_t>(std::clamp<std::int64_t>(
        seconds, 0, std::numeric_limits<std::uint16_t>::max())));
  }
  writer.WriteU32(selection.purchased_mask);
  return writer.Release();
}

std::vector<std::uint8_t> EncodeMysteryShopResponse(
    MysteryAction action,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now, std::uint32_t reward_stock_id) {
  return EncodeResponse(action, 0U, selection, rules, now, reward_stock_id);
}

std::vector<std::uint8_t> EncodeMysteryShopFailure(
    MysteryAction action,
    const application::MysterySelectionSnapshot& selection,
    const application::MysteryShopServiceRules& rules,
    std::chrono::system_clock::time_point now) {
  return EncodeResponse(action, 1U, selection, rules, now, 0U);
}

}  // namespace bf::protocol
