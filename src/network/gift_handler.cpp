#include "network/gift_handler.h"

#include <exception>
#include <utility>

#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"

namespace bf::infrastructure::network {
namespace {
using application::ServiceError;
using runtime::OutboundMessage;
// 458660 将 1900-01-01 写入 qword_14C48E0；CEC7F9 用此值判断永久礼物。
constexpr double kNoDate = 2.0;

std::uint32_t GiftDisplayId(const application::GiftView& gift) {
  // historical-export/current-bytes: 7AFB10 returns kind 3 for Stock, kind 4
  // for StockSet. CEC120/CECE50 resolve both sale IDs; a 100xxx inventory
  // template is not a sale scalar. Preserve the original DWORD width, including
  // zero on a failed claim.
  return gift.stock;
}

OutboundMessage CountMessage(std::uint32_t unread) {
  protocol::PacketWriter writer;
  writer.WriteU32(1U);
  writer.WriteU32(unread);
  return {.opcode = 817U, .body = writer.Release()};
}

void WriteReceived(protocol::PacketWriter& writer,
                   const application::GiftView& gift) {
  writer.WriteU32(gift.id);
  writer.WriteF64(
      protocol::UnixSecondsToOleDate(static_cast<double>(gift.created_at)));
  writer.WriteU32(static_cast<std::uint32_t>(gift.sender.Value()));
  writer.WriteUtf16(protocol::Utf8ToUtf16(gift.sender_name));
  writer.WriteU32(GiftDisplayId(gift));
  writer.WriteU16(gift.state);
  writer.WriteUtf16(protocol::Utf8ToUtf16(gift.message));
  writer.WriteF64(kNoDate);  // 礼物领取截止日；物品自身时限从领取事务开始。
}

void WriteSent(protocol::PacketWriter& writer,
               const application::GiftView& gift) {
  writer.WriteU32(gift.id);
  writer.WriteF64(
      protocol::UnixSecondsToOleDate(static_cast<double>(gift.created_at)));
  writer.WriteF64(gift.processed_at
                      ? protocol::UnixSecondsToOleDate(
                            static_cast<double>(*gift.processed_at))
                      : kNoDate);
  writer.WriteU32(static_cast<std::uint32_t>(gift.recipient.Value()));
  writer.WriteUtf16(protocol::Utf8ToUtf16(gift.recipient_name));
  writer.WriteU32(GiftDisplayId(gift));
  writer.WriteU16(gift.state);
  writer.WriteU16(gift.thank_kind);
  writer.WriteUtf16(protocol::Utf8ToUtf16(gift.message));
  writer.WriteUtf16(protocol::Utf8ToUtf16(gift.thanks));
}
}  // namespace

bool IsGiftRequest(std::uint16_t opcode) noexcept {
  return opcode == 815U || opcode == 816U ||
         (opcode >= 818U && opcode <= 822U) ||
         (opcode >= 824U && opcode <= 826U) || opcode == 830U;
}

GiftRequest DecodeGiftRequest(const protocol::Packet& packet) {
  GiftRequest request{.opcode = packet.opcode};
  protocol::PacketReader reader{packet.body};
  switch (packet.opcode) {
    case 815U:
    case 816U:
      request.id = reader.ReadU32();
      request.count = reader.ReadU32();
      break;
    case 818U:
      request.recipient = protocol::Utf16ToUtf8(reader.ReadUtf16(32U));
      request.extra = protocol::Utf16ToUtf8(reader.ReadUtf16(64U));
      request.id = reader.ReadU32();
      request.message = protocol::Utf16ToUtf8(reader.ReadUtf16(256U));
      break;
    case 819U:
    case 820U:
    case 822U:
    case 824U:
    case 825U:
      request.id = reader.ReadU32();
      break;
    case 821U:
      request.id = reader.ReadU32();
      request.message = protocol::Utf16ToUtf8(reader.ReadUtf16(256U));
      request.count = reader.ReadU32();
      break;
    case 826U:
      request.message = protocol::Utf16ToUtf8(reader.ReadUtf16(64U));
      break;
    case 830U:
      break;
    default:
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "unknown gift request"};
  }
  reader.RequireEnd();
  return request;
}

application::ServiceResult<runtime::EquipmentWorldResult> HandleGiftRequest(
    const GiftRequest& request, domain::PlayerId player, bool allowed,
    application::GiftStore& gifts, application::PlayerMenuSnapshotLoader& menus,
    runtime::PurchaseOperationIdSource& operations,
    runtime::SessionRegistry& sessions) {
  runtime::EquipmentWorldResult result;
  protocol::PacketWriter writer;
  const auto opcode = request.opcode;
  std::uint16_t response = 0U;
  std::optional<ServiceError> failure;
  bool assets_changed = false;
  if (opcode == 815U || opcode == 816U || opcode == 830U) {
    const bool sent = opcode == 816U;
    auto loaded = gifts.List(player, sent, opcode == 830U ? 0U : request.id,
                             opcode == 830U ? 1U : request.count);
    if (const auto* error = std::get_if<ServiceError>(&loaded)) return *error;
    const auto& page = std::get<application::GiftPage>(loaded);
    if (opcode == 830U) {
      result.replies.push_back(CountMessage(page.unread));
      return result;
    }
    writer.WriteU32(1U);
    writer.WriteU32(page.total);
    if (!sent) writer.WriteU32(page.unread);
    writer.WriteU32(static_cast<std::uint32_t>(page.gifts.size()));
    for (const auto& gift : page.gifts) {
      if (sent)
        WriteSent(writer, gift);
      else
        WriteReceived(writer, gift);
    }
    response = sent ? 821U : 820U;
  } else if (opcode == 822U) {
    auto loaded = gifts.Detail(player, request.id);
    if (const auto* error = std::get_if<ServiceError>(&loaded)) return *error;
    const auto& gift = std::get<application::GiftView>(loaded);
    const bool received = player == gift.recipient;
    auto equipped = menus.Load(received ? gift.sender : gift.recipient);
    if (const auto* error = std::get_if<ServiceError>(&equipped)) return *error;
    const auto& snapshot = std::get<application::PlayerMenuSnapshot>(equipped);
    writer.WriteU32(1U);
    writer.WriteU32(gift.id);
    writer.WriteUtf16(protocol::Utf8ToUtf16(received ? gift.sender_name
                                                     : gift.recipient_name));
    writer.WriteUtf16(
        protocol::Utf8ToUtf16(received ? gift.message : gift.thanks));
    writer.WriteU32(received ? 1U : gift.thank_kind);
    writer.WriteU32(static_cast<std::uint32_t>(snapshot.initial_fighter));
    writer.WriteBytes(protocol::EncodeEquippedItemsResponse(snapshot));
    response = 827U;
  } else {
    if (!allowed)
      failure = ServiceError{.code = application::ErrorCode::kInvalidState,
                             .safe_message = "当前阶段不能操作礼物或兑换券"};
    if (opcode == 818U) {
      response = 823U;
      if (!request.extra.empty())
        failure = ServiceError{.code = application::ErrorCode::kInvalidInput,
                               .safe_message = "赠送附加验证字段不受支持"};
      if (!failure) {
        auto next = operations.Next();
        if (const auto* error = std::get_if<ServiceError>(&next)) return *error;
        auto saved =
            gifts.Send({.operation = std::get<domain::OperationId>(next),
                        .sender = player,
                        .recipient = request.recipient,
                        .stock = domain::StockId{request.id},
                        .message = request.message});
        if (const auto* error = std::get_if<ServiceError>(&saved))
          failure = *error;
        else {
          assets_changed = true;
          const auto recipient =
              std::get<application::GiftView>(saved).recipient;
          auto page = gifts.List(recipient, false, 0U, 1U);
          if (const auto* value = std::get_if<application::GiftPage>(&page)) {
            // 离线通知失败不回滚已提交的礼物，重连后列表从持久化状态加载。
            static_cast<void>(sessions.Dispatch(runtime::SocialCommandResult{
                .deliveries = {{.recipient = recipient,
                                .message = CountMessage(value->unread)}}}));
          }
        }
      }
      writer.WriteI32(failure ? -1 : 1);
    } else if (opcode == 826U) {
      response = 831U;
      application::CouponResult coupon;
      if (!failure) {
        auto redeemed = gifts.Redeem(player, request.message);
        if (const auto* error = std::get_if<ServiceError>(&redeemed))
          failure = *error;
        else {
          coupon = std::get<application::CouponResult>(redeemed);
          assets_changed = true;
        }
      }
      writer.WriteI32(failure ? -1 : 1);
      writer.WriteU32(coupon.currency_amount);
      writer.WriteU32(coupon.item_count);
    } else {
      const auto action = opcode == 819U   ? application::GiftAction::kClaim
                          : opcode == 820U ? application::GiftAction::kDiscard
                          : opcode == 821U ? application::GiftAction::kThank
                          : opcode == 824U
                              ? application::GiftAction::kHideSent
                              : application::GiftAction::kHideReceived;
      application::GiftActionResult saved;
      if (request.count > 65535U)
        failure = ServiceError{.code = application::ErrorCode::kInvalidInput,
                               .safe_message = "感谢表情编号无效"};
      if (!failure) {
        auto changed = gifts.Act(player, request.id, action, request.message,
                                 static_cast<std::uint16_t>(request.count));
        if (const auto* error = std::get_if<ServiceError>(&changed))
          failure = *error;
        else {
          saved = std::get<application::GiftActionResult>(changed);
          assets_changed = opcode == 819U;
        }
      }
      response = opcode == 819U   ? 824U
                 : opcode == 820U ? 825U
                 : opcode == 821U ? 826U
                 : opcode == 824U ? 829U
                                  : 830U;
      writer.WriteI32(failure ? -1 : 1);
      if (opcode != 821U) writer.WriteU32(request.id);
      if (opcode == 819U) writer.WriteU32(GiftDisplayId(saved.gift));
      if (opcode == 819U || opcode == 820U || opcode == 825U)
        writer.WriteU32(saved.unread);
    }
  }
  if (failure && (failure->code == application::ErrorCode::kInternal ||
                  failure->code == application::ErrorCode::kStorageUnavailable))
    return *failure;
  result.replies.push_back({.opcode = response, .body = writer.Release()});
  if (failure) {
    result.replies.push_back(
        {.opcode = 109U,
         .body = protocol::EncodeNoticeResponse(
             protocol::Utf8ToUtf16(failure->safe_message))});
  }
  if (assets_changed) {
    // The durable send/claim/coupon receipt is already committed. A later
    // read/encoder failure must not erase 823/824/831 and encourage a second
    // native payment (818 has no request ID).
    try {
      auto loaded = menus.Load(player);
      if (std::holds_alternative<ServiceError>(loaded)) {
        result.replies.push_back(
            {.opcode = 109U,
             .body = protocol::EncodeNoticeResponse(
                 u"资产操作已提交，仓库刷新暂不可用；请稍后刷新，不要重复付款或使用道具。")});
        return result;
      }
      const auto& snapshot = std::get<application::PlayerMenuSnapshot>(loaded);
      // Encode both before appending either, so a failed encoder cannot publish
      // half a refresh.
      auto profile = protocol::EncodePlayerProfileResponse(snapshot);
      auto inventory = protocol::EncodeInventorySnapshotResponse(snapshot);
      result.replies.push_back({.opcode = 105U, .body = std::move(profile)});
      result.replies.push_back({.opcode = 802U, .body = std::move(inventory)});
    } catch (const std::exception&) {
      result.replies.push_back(
          {.opcode = 109U,
           .body = protocol::EncodeNoticeResponse(
               u"资产操作已提交，仓库刷新暂不可用；请稍后刷新，不要重复付款或使用道具。")});
    }
    // 礼物/兑换不会自动换装；不得因刷新失败改写已提交回执或重置房间准备。
  }
  return result;
}
}  // namespace bf::infrastructure::network
