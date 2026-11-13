#include "sessions/couple_flow.h"

#include <algorithm>

#include "features/chat/user_message.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/social/couple_messages.h"

namespace bf::runtime {
namespace {
using application::ErrorCode;
using application::ServiceError;

void Send(CoupleFlowResult& result, domain::PlayerId player,
          std::uint16_t opcode, std::vector<std::uint8_t> body) {
  result.messages.deliveries.push_back(
      {.recipient = player,
       .message = {.opcode = opcode, .body = std::move(body)}});
}

CoupleFlowResult Reply(domain::PlayerId player, std::uint32_t code,
                       std::u16string_view name) {
  CoupleFlowResult result;
  Send(result, player, 941U, protocol::EncodeCoupleResult(code, name));
  return result;
}

void Notice(CoupleFlowResult& result, domain::PlayerId player,
            std::string_view message) {
  Send(result, player, 109U,
       protocol::EncodeNoticeResponse(protocol::Utf8ToUtf16(message)));
}

bool Available(const std::optional<WorldSessionState>& state) {
  return state && (state->phase == WorldSessionPhase::kMenu ||
                   state->phase == WorldSessionPhase::kPlaza ||
                   state->phase == WorldSessionPhase::kRoomWaiting);
}

bool Owns(const application::PlayerMenuSnapshot& profile, std::uint32_t ring) {
  return std::ranges::any_of(profile.items, [&](const auto& item) {
    return item.item_id.Value() == ring && item.quantity > 0 &&
           (!item.expires_at || *item.expires_at > profile.observed_at);
  });
}

void Answer(CoupleFlowResult& result, const application::CoupleFormCommand& c,
            bool accepted) {
  const auto body = protocol::EncodeCoupleAnswer(
      accepted, protocol::Utf8ToUtf16(c.actor_name),
      protocol::Utf8ToUtf16(c.partner_name));
  Send(result, c.actor, 942U, body);
  Send(result, c.partner, 942U, body);
}
}  // namespace

CoupleFlowResult CoupleFlow::CancelLocked(domain::PlayerId player,
                                          SessionHandle handle) {
  CoupleFlowResult result;
  std::erase_if(invitations_, [&](const auto& entry) {
    const auto& i = entry.second;
    if ((i.command.actor == player && i.inviter_session == handle) ||
        (i.command.partner == player && i.recipient_session == handle)) {
      result.expected_sessions[i.command.actor] = i.inviter_session;
      result.expected_sessions[i.command.partner] = i.recipient_session;
      Answer(result, i.command, false);
      return true;
    }
    return false;
  });
  return result;
}

CoupleFlowResult CoupleFlow::Disconnect(domain::PlayerId player,
                                        SessionHandle handle) {
  std::scoped_lock lock{mutex_};
  return CancelLocked(player, handle);
}

application::ServiceResult<CoupleFlowResult> CoupleFlow::Refresh(
    CoupleFlowResult result, const application::CoupleChange& change) {
  for (auto player : {change.actor, change.partner}) {
    auto loaded = profiles_.Load(player);
    if (const auto* error = std::get_if<ServiceError>(&loaded)) {
      // The transaction already committed. Deliver its result and tell the
      // client to reload; never turn an applied purchase into an apparent
      // retryable failure.
      Notice(result, player,
             "情侣关系已保存，资料刷新暂时失败，请重新登录查看");
      static_cast<void>(error);
      continue;
    }
    auto profile = std::get<application::PlayerMenuSnapshot>(std::move(loaded));
    Send(result, player, 105U, protocol::EncodePlayerProfileResponse(profile));
    Send(result, player, 802U,
         protocol::EncodeInventorySnapshotResponse(profile));
    Send(result, player, 806U, protocol::EncodeEquippedItemsResponse(profile));
    Send(result, player, 944U, protocol::EncodeCouplePartner(profile.couple));
    result.changed.push_back(std::move(profile));
  }
  return result;
}

application::ServiceResult<CoupleFlowResult> CoupleFlow::IdentityChanged(
    domain::PlayerId player, SessionHandle handle,
    const std::function<void(CoupleFlowResult&)>& publish) {
  std::scoped_lock lock{mutex_};
  auto result = CancelLocked(player, handle);
  auto loaded = profiles_.Load(player);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) return *error;
  const auto& self = std::get<application::PlayerMenuSnapshot>(loaded);
  if (self.couple) {
    // Only couple fields are updated in world caches; equipment and readiness
    // remain current.
    auto partner = profiles_.Load(self.couple->partner_id);
    if (const auto* error = std::get_if<ServiceError>(&partner)) return *error;
    auto peer = std::get<application::PlayerMenuSnapshot>(std::move(partner));
    Send(result, peer.player_id, 944U,
         protocol::EncodeCouplePartner(peer.couple));
    result.changed.push_back(std::move(peer));
  }
  for (const auto& delivery : result.messages.deliveries)
    if (const auto current = sessions_.CurrentPlayerSession(delivery.recipient))
      result.expected_sessions.try_emplace(delivery.recipient, *current);
  publish(result);
  return result;
}

application::ServiceResult<CoupleFlowResult> CoupleFlow::Handle(
    domain::PlayerId actor, SessionHandle session,
    const protocol::Packet& packet, std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point utc,
    const std::function<void(CoupleFlowResult&)>& publish) {
  std::scoped_lock lock{mutex_};
  auto result = HandleLocked(actor, session, packet, now, utc);
  if (auto* value = std::get_if<CoupleFlowResult>(&result)) {
    value->expected_sessions.try_emplace(actor, session);
    for (const auto& delivery : value->messages.deliveries)
      if (const auto target =
              sessions_.CurrentPlayerSession(delivery.recipient))
        value->expected_sessions.try_emplace(delivery.recipient, *target);
    if (publish)
      publish(
          *value);  // DB commit order also orders notices and world updates.
  }
  return result;
}

application::ServiceResult<CoupleFlowResult> CoupleFlow::HandleLocked(
    domain::PlayerId actor, SessionHandle session,
    const protocol::Packet& packet, std::chrono::steady_clock::time_point now,
    std::chrono::system_clock::time_point utc) {
  // A single consent ledger covers all gateway connections. A pending request
  // is never copied to a replacement session or restored after a server
  // restart.
  if (!sessions_.IsCurrentPlayerSession(actor, session))
    return ServiceError{.code = ErrorCode::kUnauthenticated,
                        .safe_message = "情侣请求会话已经失效"};
  std::erase_if(invitations_, [&](const auto& entry) {
    const auto& i = entry.second;
    return now >= i.expires ||
           !sessions_.IsCurrentPlayerSession(i.command.actor,
                                             i.inviter_session) ||
           !sessions_.IsCurrentPlayerSession(i.command.partner,
                                             i.recipient_session);
  });
  std::erase_if(last_invite_, [&](const auto& entry) {
    return now - entry.second >= std::chrono::seconds{10};
  });
  if (packet.opcode == 937U) {
    static_cast<void>(protocol::DecodeCoupleCancel(packet.body));
    return CancelLocked(actor, session);
  }
  if (packet.opcode == 938U) {
    const auto request = protocol::DecodeCoupleAnswer(packet.body);
    const auto found = invitations_.find(actor);
    if (found == invitations_.end()) {
      CoupleFlowResult result;
      Notice(result, actor, "情侣邀请已过期或取消，请重新邀请");
      return result;
    }
    const auto invite = found->second;
    if (request.flag || invite.recipient_session != session ||
        protocol::Utf16ToUtf8(request.inviter) != invite.command.actor_name ||
        protocol::Utf16ToUtf8(request.recipient) !=
            invite.command.partner_name) {
      CoupleFlowResult result;
      Notice(result, actor, "情侣邀请身份不匹配，请重新邀请");
      return result;
    }
    invitations_.erase(found);
    CoupleFlowResult result;
    result.expected_sessions[invite.command.actor] = invite.inviter_session;
    result.expected_sessions[invite.command.partner] = invite.recipient_session;
    if (!request.accepted) {
      Answer(result, invite.command, false);
      return result;
    }
    const auto sender = sessions_.WorldState(invite.command.actor),
               receiver = sessions_.WorldState(actor);
    if (!Available(sender) || !Available(receiver) ||
        sender->channel != receiver->channel) {
      Answer(result, invite.command, false);
      Notice(result, actor, "双方须在同一频道且未开始游戏");
      return result;
    }
    auto command = invite.command;
    command.now = utc;
    auto committed = store_.Form(command);
    if (const auto* error = std::get_if<ServiceError>(&committed)) {
      Answer(result, command, false);
      Notice(result, command.actor, error->safe_message);
      Notice(result, actor, error->safe_message);
      return result;
    }
    Answer(result, command, true);
    return Refresh(std::move(result),
                   std::get<application::CoupleChange>(committed));
  }
  auto loaded = profiles_.Load(actor);
  if (const auto* error = std::get_if<ServiceError>(&loaded)) return *error;
  const auto self =
      std::get<application::PlayerMenuSnapshot>(std::move(loaded));
  if (packet.opcode == 936U) {
    const auto request = protocol::DecodeCoupleInvite(packet.body);
    if (!request.confirmed) return CancelLocked(actor, session);
    if (request.equipment_owner != 0U &&
        request.equipment_owner != actor.Value())
      return Reply(actor, 6U, request.target);
    const auto target =
        domain::SocialTarget::Parse(protocol::Utf16ToUtf8(request.target));
    if (!target || target->GetPlayerId().IsValid())
      return Reply(actor, 2U, request.target);
    if (target->Matches(actor, self.nickname))
      return Reply(actor, 1U, request.target);
    if (self.couple) return Reply(actor, 10U, request.target);
    if (!Available(sessions_.WorldState(actor)))
      return Reply(actor, 15U, request.target);
    if (!application::CoupleDisplayRing(request.ring) ||
        !Owns(self, request.ring)) {
      auto result = Reply(actor, 6U, request.target);
      Notice(result, actor, "没有可使用的情侣戒指");
      return result;
    }
    if (!request.message.empty() &&
        !std::holds_alternative<domain::UserMessage>(
            domain::UserMessage::Parse(request.message)))
      return Reply(actor, 13U, request.target);
    if (last_invite_.contains(actor)) return Reply(actor, 11U, request.target);
    last_invite_[actor] = now;
    auto overview =
        social_.LoadOverview({.player_id = actor, .search_account = *target});
    if (const auto* error = std::get_if<ServiceError>(&overview)) {
      if (error->code == ErrorCode::kInvalidInput ||
          error->code == ErrorCode::kInvalidState)
        return Reply(actor, 2U, request.target);
      return *error;
    }
    const auto& records =
        std::get<application::SocialOverviewSnapshot>(overview).roster;
    const auto peer = std::ranges::find_if(records, [&](const auto& row) {
      return target->Matches(row.player_id, row.account_name);
    });
    if (peer == records.end()) return Reply(actor, 2U, request.target);
    const auto peer_session = sessions_.CurrentPlayerSession(peer->player_id);
    const auto peer_world = sessions_.WorldState(peer->player_id),
               self_world = sessions_.WorldState(actor);
    if (!peer_session || !peer_world || !self_world ||
        peer_world->channel != self_world->channel)
      return Reply(actor, 3U, request.target);
    if (!Available(peer_world))
      return Reply(
          actor, peer_world->phase == WorldSessionPhase::kRoomPlaying ? 4U : 7U,
          request.target);
    auto peer_loaded = profiles_.Load(peer->player_id);
    if (const auto* error = std::get_if<ServiceError>(&peer_loaded))
      return *error;
    const auto& peer_profile =
        std::get<application::PlayerMenuSnapshot>(peer_loaded);
    if (peer_profile.couple) return Reply(actor, 9U, request.target);
    if (const auto error = store_.CheckInvitation(actor, peer->player_id)) {
      if (error->code != ErrorCode::kPermissionDenied) return *error;
      return Reply(actor, 6U, request.target);
    }
    for (const auto& [unused, i] : invitations_) {
      static_cast<void>(unused);
      if (i.command.actor == actor || i.command.partner == actor ||
          i.command.actor == peer->player_id ||
          i.command.partner == peer->player_id)
        return Reply(actor, 11U, request.target);
    }
    if (invitations_.size() >= 4096U) return Reply(actor, 11U, request.target);
    application::CoupleFormCommand c{.actor = actor,
                                     .partner = peer->player_id,
                                     .actor_name = self.nickname,
                                     .partner_name = peer_profile.nickname,
                                     .invitation_ring = request.ring,
                                     .now = utc};
    invitations_.emplace(
        peer->player_id,
        Invitation{c, session, *peer_session, now + std::chrono::seconds{180}});
    auto result =
        Reply(actor, 8U, protocol::Utf8ToUtf16(peer_profile.nickname));
    result.expected_sessions[peer->player_id] = *peer_session;
    Send(result, peer->player_id, 940U,
         protocol::EncodeCoupleInvite(
             self, protocol::Utf8ToUtf16(peer_profile.nickname),
             request.message));
    return result;
  }
  if (packet.opcode == 939U) {
    const auto partner_name = protocol::DecodeCoupleBreakup(packet.body);
    CoupleFlowResult result;
    if (!self.couple ||
        protocol::Utf16ToUtf8(partner_name) != self.couple->partner_nickname) {
      Send(result, actor, 943U,
           protocol::EncodeCoupleBreakup(protocol::Utf8ToUtf16(self.nickname),
                                         partner_name, 1U));
      return result;
    }
    auto committed = store_.Breakup(actor, self.couple->partner_id);
    if (const auto* error = std::get_if<ServiceError>(&committed)) {
      Send(result, actor, 943U,
           protocol::EncodeCoupleBreakup(protocol::Utf8ToUtf16(self.nickname),
                                         partner_name, 1U));
      Notice(result, actor, error->safe_message);
      return result;
    }
    const auto& change = std::get<application::CoupleChange>(committed);
    Send(result, actor, 943U,
         protocol::EncodeCoupleBreakup(protocol::Utf8ToUtf16(change.actor_name),
                                       partner_name, 0U));
    Send(result, change.partner, 943U,
         protocol::EncodeCoupleBreakup(protocol::Utf8ToUtf16(change.actor_name),
                                       partner_name, 2U));
    return Refresh(std::move(result), change);
  }
  if (packet.opcode == 956U) {
    const auto request = protocol::DecodeCoupleRing(packet.body);
    CoupleFlowResult result;
    auto reject = [&](std::int32_t code) {
      Send(result, actor, 989U,
           protocol::EncodeCoupleRingResult(code, 0U, false));
      return result;
    };
    if (request.flag || !self.couple || !Available(sessions_.WorldState(actor)))
      return reject(-1);
    const auto peer_world = sessions_.WorldState(self.couple->partner_id),
               self_world = sessions_.WorldState(actor);
    if (!Available(peer_world) || !self_world ||
        peer_world->channel != self_world->channel)
      return reject(-2);
    application::CoupleFormCommand c{
        .actor = actor,
        .partner = self.couple->partner_id,
        .actor_name = self.nickname,
        .partner_name = self.couple->partner_nickname,
        .invitation_ring = request.ring,
        .now = utc};
    auto committed = store_.ChangeRing(c);
    if (const auto* error = std::get_if<ServiceError>(&committed)) {
      Notice(result, actor, error->safe_message);
      return reject(-1);
    }
    const auto& change = std::get<application::CoupleChange>(committed);
    auto refreshed = Refresh(std::move(result), change);
    if (auto* value = std::get_if<CoupleFlowResult>(&refreshed)) {
      // B7A510 rebuilds the inventory views immediately: publish new items
      // first.
      Send(*value, actor, 989U,
           protocol::EncodeCoupleRingResult(0, change.ring, false));
      Send(*value, change.partner, 989U,
           protocol::EncodeCoupleRingResult(0, change.ring, false));
    }
    return refreshed;
  }
  return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                      .safe_message = "不是已核实的情侣报文"};
}
}  // namespace bf::runtime
