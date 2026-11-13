#include "sessions/social_command_flow.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "protocol/codec/text_codec.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/social/social_messages.h"

namespace bf::runtime {
namespace {

constexpr std::uint16_t kRosterOpcode = 65011U;
constexpr std::uint16_t kLegacyPrivateMessageOpcode = 65012U;
constexpr std::uint16_t kInvitationOpcode = 65013U;
constexpr std::uint16_t kNativePrivateMessageOpcode = 65015U;
constexpr std::uint16_t kNoticeOpcode = 109U;
constexpr std::size_t kMaximumCommandsPerWindow = 8U;
constexpr auto kCommandWindow = std::chrono::seconds{10};

[[nodiscard]] application::ServiceError InvalidInput(std::string_view message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = std::string{message}};
}

[[nodiscard]] application::ServiceError ContractError(
    std::string_view message) {
  return application::ServiceError{.code = application::ErrorCode::kInternal,
                                   .safe_message = std::string{message}};
}

[[nodiscard]] application::ServiceResult<domain::SocialTarget> ParseAccount(
    std::u16string_view text) {
  const auto utf8 = protocol::Utf16ToUtf8(text);
  auto result = domain::SocialTarget::Parse(utf8);
  if (result) return *result;
  return InvalidInput("请输入有效的游戏昵称");
}

[[nodiscard]] application::ServiceResult<domain::UserMessage> ParseMessage(
    std::u16string_view text) {
  auto result = domain::UserMessage::Parse(text);
  if (const auto* message = std::get_if<domain::UserMessage>(&result)) {
    return *message;
  }
  return InvalidInput(domain::UserMessageValidationErrorMessage(
      std::get<domain::UserMessageValidationError>(result)));
}

[[nodiscard]] std::vector<protocol::SocialRosterEntry> ProtocolRoster(
    const application::SocialOverviewSnapshot& overview,
    const SocialPresenceView& presence) {
  std::vector<protocol::SocialRosterEntry> result;
  result.reserve(overview.roster.size());
  for (const auto& record : overview.roster) {
    result.push_back(protocol::SocialRosterEntry{
        .player_id = record.player_id,
        .account_name = protocol::Utf8ToUtf16(record.account_name),
        .relationship_state = static_cast<std::uint32_t>(record.state),
        .online = presence.Lookup(record.player_id).online});
  }
  return result;
}

void AppendRoster(SocialCommandResult& result, domain::PlayerId recipient,
                  const application::SocialOverviewSnapshot& overview,
                  const SocialPresenceView& presence,
                  std::u16string_view status) {
  const auto roster = ProtocolRoster(overview, presence);
  result.deliveries.push_back(
      {.recipient = recipient,
       .message = {
           .opcode = kRosterOpcode,
           .body = protocol::EncodeSocialRosterResponse(status, roster)}});
}

void AppendInvitations(SocialCommandResult& result, domain::PlayerId recipient,
                       const application::SocialOverviewSnapshot& overview) {
  for (const auto& invitation : overview.invitations) {
    const auto account = protocol::Utf8ToUtf16(invitation.sender_account_name);
    const std::u16string_view message = invitation.request_message.has_value()
                                            ? invitation.request_message->Text()
                                            : std::u16string_view{};
    result.deliveries.push_back(
        {.recipient = recipient,
         .message = {.opcode = kInvitationOpcode,
                     .body = protocol::EncodeSocialInvitationResponse(
                         {.sender_player_id = invitation.sender_player_id,
                          .sender_account_name = account,
                          .request_message = message})}});
  }
}

[[nodiscard]] bool Displayable(
    const application::ServiceError& error) noexcept {
  return error.code == application::ErrorCode::kInvalidInput ||
         error.code == application::ErrorCode::kInvalidState ||
         error.code == application::ErrorCode::kPermissionDenied ||
         error.code == application::ErrorCode::kConflict ||
         error.code == application::ErrorCode::kOverloaded;
}

[[nodiscard]] std::optional<application::FriendAction> FriendAction(
    protocol::SocialAction action) noexcept {
  switch (action) {
    case protocol::SocialAction::kAdd:
      return application::FriendAction::kAdd;
    case protocol::SocialAction::kAccept:
      return application::FriendAction::kAccept;
    case protocol::SocialAction::kDecline:
      return application::FriendAction::kDecline;
    case protocol::SocialAction::kRemove:
      return application::FriendAction::kRemove;
    case protocol::SocialAction::kBlock:
      return application::FriendAction::kBlock;
    case protocol::SocialAction::kUnblock:
      return application::FriendAction::kUnblock;
    case protocol::SocialAction::kNative:
    case protocol::SocialAction::kList:
    case protocol::SocialAction::kSearch:
    case protocol::SocialAction::kHistory:
    case protocol::SocialAction::kMessage:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::u16string_view RelationshipStatus(
    application::FriendAction action) noexcept {
  switch (action) {
    case application::FriendAction::kAdd:
      return u"好友申请已发送，等待对方接受";
    case application::FriendAction::kAccept:
      return u"已成为好友";
    case application::FriendAction::kDecline:
      return u"已拒绝申请";
    case application::FriendAction::kRemove:
      return u"已删除好友";
    case application::FriendAction::kBlock:
      return u"已屏蔽";
    case application::FriendAction::kUnblock:
      return u"已解除屏蔽";
  }
  return u"好友关系已更新";
}

}  // namespace

SocialCommandFlow::SocialCommandFlow(
    SocialActorIdentity actor, application::SocialSnapshotLoader& snapshots,
    application::FriendRelationshipChanger& relationships,
    application::PrivateMessageSender& private_messages,
    const SocialPresenceView& presence)
    : actor_(std::move(actor)),
      snapshots_(snapshots),
      relationships_(relationships),
      private_messages_(private_messages),
      presence_(presence) {
  if (!actor_.player_id.IsValid()) {
    throw std::invalid_argument{"social actor player ID must be valid"};
  }
}

application::ServiceResult<SocialCommandResult> SocialCommandFlow::Handle(
    const protocol::Packet& packet, std::chrono::steady_clock::time_point now) {
  if (packet.opcode != 65010U) {
    return InvalidInput("当前社交流只接受 C2S65010");
  }
  const auto request = protocol::DecodeSocialRequest(packet.body);
  if (!ConsumeQuota(now)) {
    return DisplayBusinessError(
        application::ServiceError{.code = application::ErrorCode::kOverloaded,
                                  .safe_message = "发送过快，请稍后再试"});
  }

  switch (request.action) {
    case protocol::SocialAction::kNative:
      return HandleOverview(request, true);
    case protocol::SocialAction::kList:
    case protocol::SocialAction::kSearch:
      return HandleOverview(request, false);
    case protocol::SocialAction::kHistory:
      return HandleHistory(request);
    case protocol::SocialAction::kMessage:
      return HandlePrivateMessage(request);
    case protocol::SocialAction::kAdd:
    case protocol::SocialAction::kAccept:
    case protocol::SocialAction::kDecline:
    case protocol::SocialAction::kRemove:
    case protocol::SocialAction::kBlock:
    case protocol::SocialAction::kUnblock:
      return HandleRelationshipChange(request);
  }
  return ContractError("社交协议动作未被运行时覆盖");
}

bool SocialCommandFlow::NativeSocialEnabled() const noexcept {
  return native_social_;
}

application::ServiceResult<SocialCommandResult>
SocialCommandFlow::HandleOverview(const protocol::SocialRequest& request,
                                  bool enable_native) {
  if (enable_native) {
    native_social_ = true;
  }

  application::SocialOverviewRequest query{.player_id = actor_.player_id};
  std::u16string_view status;
  if (request.action == protocol::SocialAction::kSearch) {
    auto account = ParseAccount(request.account_name);
    if (const auto* error = std::get_if<application::ServiceError>(&account)) {
      return DisplayBusinessError(*error);
    }
    query.search_account = std::get<domain::SocialTarget>(std::move(account));
    status = u"搜索结果";
  }

  auto overview = snapshots_.LoadOverview(query);
  if (const auto* error = std::get_if<application::ServiceError>(&overview)) {
    return *error;
  }
  SocialCommandResult result;
  AppendRoster(result, actor_.player_id,
               std::get<application::SocialOverviewSnapshot>(overview),
               presence_, status);
  if (request.action != protocol::SocialAction::kSearch && native_social_) {
    AppendInvitations(result, actor_.player_id,
                      std::get<application::SocialOverviewSnapshot>(overview));
  }
  return result;
}

application::ServiceResult<SocialCommandResult>
SocialCommandFlow::HandleHistory(const protocol::SocialRequest& request) {
  auto account = ParseAccount(request.account_name);
  if (const auto* error = std::get_if<application::ServiceError>(&account)) {
    return DisplayBusinessError(*error);
  }
  const application::SocialHistoryRequest query{
      .player_id = actor_.player_id,
      .target_account = std::get<domain::SocialTarget>(std::move(account)),
      .limit = application::kMaximumPrivateHistoryMessages};
  auto history = snapshots_.LoadHistory(query);
  if (const auto* error = std::get_if<application::ServiceError>(&history)) {
    if (Displayable(*error)) {
      return DisplayBusinessError(*error);
    }
    return *error;
  }
  const auto& conversation =
      std::get<application::SocialConversationSnapshot>(history);
  if (conversation.messages.empty()) {
    auto overview = snapshots_.LoadOverview({.player_id = actor_.player_id});
    if (const auto* error = std::get_if<application::ServiceError>(&overview)) {
      return *error;
    }
    SocialCommandResult result;
    AppendRoster(result, actor_.player_id,
                 std::get<application::SocialOverviewSnapshot>(overview),
                 presence_, u"暂无聊天记录");
    return result;
  }

  SocialCommandResult result;
  const auto peer_name = protocol::Utf8ToUtf16(conversation.peer_account_name);
  for (const auto& message : conversation.messages) {
    const auto& speaker_name =
        message.sender_player_id == conversation.actor_player_id
            ? conversation.actor_account_name
            : conversation.peer_account_name;
    const auto speaker = protocol::Utf8ToUtf16(speaker_name);
    if (native_social_) {
      result.deliveries.push_back(
          {.recipient = actor_.player_id,
           .message = {.opcode = kNativePrivateMessageOpcode,
                       .body = protocol::EncodeNativePrivateMessageResponse(
                           {.peer_player_id = conversation.peer_player_id,
                            .peer_account_name = peer_name,
                            .speaker_account_name = speaker,
                            .message = message.message.Text()})}});
    } else {
      result.deliveries.push_back(
          {.recipient = actor_.player_id,
           .message = {
               .opcode = kLegacyPrivateMessageOpcode,
               .body = protocol::EncodeLegacyPrivateMessageResponse(
                   {.label = speaker, .message = message.message.Text()})}});
    }
  }
  return result;
}

application::ServiceResult<SocialCommandResult>
SocialCommandFlow::HandlePrivateMessage(
    const protocol::SocialRequest& request) {
  auto account = ParseAccount(request.account_name);
  if (const auto* error = std::get_if<application::ServiceError>(&account)) {
    return DisplayBusinessError(*error);
  }
  auto message = ParseMessage(request.text);
  if (const auto* error = std::get_if<application::ServiceError>(&message)) {
    return DisplayBusinessError(*error);
  }
  const auto target_account = std::get<domain::SocialTarget>(account);
  const auto message_value = std::get<domain::UserMessage>(message);
  auto sent = private_messages_.Send({.sender_player_id = actor_.player_id,
                                      .target_account_name = target_account,
                                      .message = message_value});
  if (const auto* error = std::get_if<application::ServiceError>(&sent)) {
    if (Displayable(*error)) {
      return DisplayBusinessError(*error);
    }
    return *error;
  }
  const auto& committed = std::get<application::PrivateMessageResult>(sent);
  const auto committed_account =
      domain::SocialTarget::Parse(committed.target_account_name);
  const auto* parsed_committed_account =
      committed_account ? &*committed_account : nullptr;
  if (!committed.message_id.IsValid() ||
      !committed.target_player_id.IsValid() ||
      committed.target_player_id == actor_.player_id ||
      parsed_committed_account == nullptr ||
      !target_account.Matches(committed.target_player_id,
                              committed.target_account_name)) {
    return ContractError("私聊用例返回的提交身份与请求不一致");
  }
  const auto target_name = protocol::Utf8ToUtf16(committed.target_account_name);
  const auto actor_name = protocol::Utf8ToUtf16(
      committed.sender_nickname.empty() ? actor_.account_name.DisplayName()
                                        : committed.sender_nickname);
  const auto target_presence = presence_.Lookup(committed.target_player_id);

  SocialCommandResult result;
  if (target_presence.online) {
    if (target_presence.extended && target_presence.native_social) {
      result.deliveries.push_back(
          {.recipient = committed.target_player_id,
           .message = {.opcode = kNativePrivateMessageOpcode,
                       .body = protocol::EncodeNativePrivateMessageResponse(
                           {.peer_player_id = actor_.player_id,
                            .peer_account_name = actor_name,
                            .speaker_account_name = actor_name,
                            .message = message_value.Text()})}});
    } else if (target_presence.extended) {
      result.deliveries.push_back(
          {.recipient = committed.target_player_id,
           .message = {
               .opcode = kLegacyPrivateMessageOpcode,
               .body = protocol::EncodeLegacyPrivateMessageResponse(
                   {.label = actor_name, .message = message_value.Text()})}});
    }
    const auto notice =
        std::u16string{u"【私聊】"} + actor_name + u"：" + message_value.Text();
    result.deliveries.push_back(
        {.recipient = committed.target_player_id,
         .message = {.opcode = kNoticeOpcode,
                     .body = protocol::EncodeNoticeResponse(notice)}});
  }

  if (native_social_) {
    result.deliveries.push_back(
        {.recipient = actor_.player_id,
         .message = {.opcode = kNativePrivateMessageOpcode,
                     .body = protocol::EncodeNativePrivateMessageResponse(
                         {.peer_player_id = committed.target_player_id,
                          .peer_account_name = target_name,
                          .speaker_account_name = actor_name,
                          .message = message_value.Text()})}});
  } else {
    const auto label = std::u16string{u"我 → "} + target_name;
    result.deliveries.push_back(
        {.recipient = actor_.player_id,
         .message = {.opcode = kLegacyPrivateMessageOpcode,
                     .body = protocol::EncodeLegacyPrivateMessageResponse(
                         {.label = label, .message = message_value.Text()})}});
  }
  return result;
}

application::ServiceResult<SocialCommandResult>
SocialCommandFlow::HandleRelationshipChange(
    const protocol::SocialRequest& request) {
  const auto action = FriendAction(request.action);
  if (!action.has_value()) {
    return ContractError("关系命令没有对应应用动作");
  }
  auto account = ParseAccount(request.account_name);
  if (const auto* error = std::get_if<application::ServiceError>(&account)) {
    return DisplayBusinessError(*error);
  }

  std::optional<domain::UserMessage> request_message;
  if (*action == application::FriendAction::kAdd && !request.text.empty()) {
    auto message = ParseMessage(request.text);
    if (const auto* error = std::get_if<application::ServiceError>(&message)) {
      return DisplayBusinessError(*error);
    }
    request_message = std::get<domain::UserMessage>(std::move(message));
  }
  const auto requested_account =
      std::get<domain::SocialTarget>(std::move(account));
  auto changed =
      relationships_.Change({.actor_player_id = actor_.player_id,
                             .target_account_name = requested_account,
                             .action = *action,
                             .request_message = std::move(request_message)});
  if (const auto* error = std::get_if<application::ServiceError>(&changed)) {
    if (Displayable(*error)) {
      return DisplayBusinessError(*error);
    }
    return *error;
  }
  const auto& committed = std::get<application::FriendChangeResult>(changed);
  const auto committed_account =
      domain::SocialTarget::Parse(committed.target_account_name);
  const auto* parsed_committed_account =
      committed_account ? &*committed_account : nullptr;
  if (!committed.target_player_id.IsValid() ||
      committed.target_player_id == actor_.player_id ||
      parsed_committed_account == nullptr ||
      !requested_account.Matches(committed.target_player_id,
                                 committed.target_account_name)) {
    return ContractError("关系用例返回的提交身份与请求不一致");
  }

  auto actor_overview =
      snapshots_.LoadOverview({.player_id = actor_.player_id});
  if (const auto* error =
          std::get_if<application::ServiceError>(&actor_overview)) {
    return *error;
  }
  SocialCommandResult result;
  const auto target_presence = presence_.Lookup(committed.target_player_id);
  if (target_presence.online && target_presence.extended) {
    auto target_overview =
        snapshots_.LoadOverview({.player_id = committed.target_player_id});
    if (const auto* error =
            std::get_if<application::ServiceError>(&target_overview)) {
      return *error;
    }
    const auto& target_snapshot =
        std::get<application::SocialOverviewSnapshot>(target_overview);
    AppendRoster(result, committed.target_player_id, target_snapshot, presence_,
                 u"好友关系已更新");
    if (target_presence.native_social) {
      AppendInvitations(result, committed.target_player_id, target_snapshot);
    }
  }
  AppendRoster(result, actor_.player_id,
               std::get<application::SocialOverviewSnapshot>(actor_overview),
               presence_, RelationshipStatus(*action));
  return result;
}

application::ServiceResult<SocialCommandResult>
SocialCommandFlow::DisplayBusinessError(
    const application::ServiceError& error) {
  if (!Displayable(error)) {
    return error;
  }
  auto overview = snapshots_.LoadOverview({.player_id = actor_.player_id});
  if (const auto* query_error =
          std::get_if<application::ServiceError>(&overview)) {
    return *query_error;
  }
  SocialCommandResult result;
  AppendRoster(result, actor_.player_id,
               std::get<application::SocialOverviewSnapshot>(overview),
               presence_, protocol::Utf8ToUtf16(error.safe_message));
  return result;
}

bool SocialCommandFlow::ConsumeQuota(
    std::chrono::steady_clock::time_point now) {
  const auto cutoff = now - kCommandWindow;
  while (!command_times_.empty() && command_times_.front() <= cutoff) {
    command_times_.pop_front();
  }
  if (command_times_.size() >= kMaximumCommandsPerWindow) {
    return false;
  }
  command_times_.push_back(now);
  return true;
}

application::ServiceResult<SocialCommandResult> SocialPresenceChanged(
    domain::PlayerId player, application::SocialSnapshotLoader& snapshots,
    const SocialPresenceView& presence) {
  auto loaded = snapshots.LoadOverview({.player_id = player});
  if (const auto* error = std::get_if<application::ServiceError>(&loaded))
    return *error;
  SocialCommandResult result;
  for (const auto& peer :
       std::get<application::SocialOverviewSnapshot>(loaded).roster) {
    if (peer.state != application::SocialRosterState::kFriend ||
        !presence.Lookup(peer.player_id).extended)
      continue;
    auto other = snapshots.LoadOverview({.player_id = peer.player_id});
    if (const auto* error = std::get_if<application::ServiceError>(&other))
      return *error;
    AppendRoster(result, peer.player_id,
                 std::get<application::SocialOverviewSnapshot>(other), presence,
                 u"");
  }
  return result;
}

}  // namespace bf::runtime
