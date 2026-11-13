#include "features/social/social_query.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bf::application {
namespace {

constexpr std::size_t kMaximumSocialRosterRecords = 301U;
constexpr std::size_t kMaximumSocialInvitations = 100U;

[[nodiscard]] ServiceError InvalidRequest(std::string_view message) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::string{message}};
}

[[nodiscard]] ServiceError ContractError(std::string_view detail) {
  return ServiceError{
      .code = ErrorCode::kInternal,
      .safe_message =
          std::string{"社交只读仓储返回了无效状态："} + std::string{detail}};
}

[[nodiscard]] ServiceError MissingPlayer() {
  return ServiceError{.code = ErrorCode::kInvalidState,
                      .safe_message = "玩家尚未建立可读取的社交资料"};
}

[[nodiscard]] bool ValidAccount(std::string_view account_name) {
  return domain::SocialTarget::Parse(account_name).has_value();
}

[[nodiscard]] bool KnownRosterState(SocialRosterState state) noexcept {
  switch (state) {
    case SocialRosterState::kSearchResult:
    case SocialRosterState::kFriend:
    case SocialRosterState::kIncomingRequest:
    case SocialRosterState::kOutgoingRequest:
    case SocialRosterState::kBlocked:
      return true;
  }
  return false;
}

[[nodiscard]] std::optional<ServiceError> ValidateOverview(
    const SocialOverviewSnapshot& snapshot,
    const SocialOverviewRequest& request) {
  if (snapshot.owner_player_id != request.player_id) {
    return ContractError("名单所属玩家与查询条件不一致");
  }
  if (snapshot.roster.size() > kMaximumSocialRosterRecords) {
    return ContractError("合并名单超过好友、申请、屏蔽与单个搜索结果上限");
  }
  if (snapshot.invitations.size() > kMaximumSocialInvitations) {
    return ContractError("收到申请超过业务上限");
  }

  std::unordered_map<std::uint64_t, SocialRosterState> states;
  std::unordered_map<std::uint64_t, std::string> account_keys_by_player;
  std::unordered_set<std::string> account_keys;
  states.reserve(snapshot.roster.size());
  account_keys_by_player.reserve(snapshot.roster.size());
  account_keys.reserve(snapshot.roster.size());
  std::uint64_t previous_persistent_id = 0U;
  bool saw_search_result = false;
  bool found_search_account = false;
  const domain::SocialTarget* search_account =
      request.search_account.has_value() ? &request.search_account.value()
                                         : nullptr;
  for (const auto& record : snapshot.roster) {
    const auto parsed = domain::SocialTarget::Parse(record.account_name);
    const auto* parsed_account = parsed ? &*parsed : nullptr;
    if (!record.player_id.IsValid() || record.player_id == request.player_id ||
        parsed_account == nullptr || !KnownRosterState(record.state)) {
      return ContractError("名单包含无效身份或账号名");
    }
    if (!states.emplace(record.player_id.Value(), record.state).second) {
      return ContractError("名单包含重复玩家");
    }
    if (!account_keys.insert(parsed_account->Key()).second) {
      return ContractError("名单包含重复账号唯一键");
    }
    account_keys_by_player.emplace(record.player_id.Value(),
                                   parsed_account->Key());
    found_search_account =
        found_search_account ||
        (search_account != nullptr &&
         search_account->Matches(record.player_id, record.account_name));
    if (record.state == SocialRosterState::kSearchResult) {
      if (saw_search_result || search_account == nullptr ||
          !search_account->Matches(record.player_id, record.account_name)) {
        return ContractError("搜索结果与请求账号不一致或重复");
      }
      saw_search_result = true;
      continue;
    }
    if (saw_search_result ||
        record.player_id.Value() <= previous_persistent_id) {
      return ContractError("持久关系未按玩家标识升序或位于搜索结果之后");
    }
    previous_persistent_id = record.player_id.Value();
  }
  if (search_account != nullptr && !found_search_account) {
    return ContractError("搜索请求没有返回目标现有关系或搜索项");
  }

  std::unordered_set<std::uint64_t> invitation_senders;
  invitation_senders.reserve(snapshot.invitations.size());
  for (const auto& invitation : snapshot.invitations) {
    if (!invitation.sender_player_id.IsValid() ||
        invitation.sender_player_id == request.player_id ||
        !ValidAccount(invitation.sender_account_name) ||
        !invitation_senders.insert(invitation.sender_player_id.Value())
             .second) {
      return ContractError("收到申请包含无效或重复发起者");
    }
    const auto state = states.find(invitation.sender_player_id.Value());
    if (state == states.end() ||
        state->second != SocialRosterState::kIncomingRequest) {
      return ContractError("收到申请没有对应的 incoming 名单状态");
    }
    const auto sender_account =
        domain::SocialTarget::Parse(invitation.sender_account_name);
    const auto* parsed_sender = sender_account ? &*sender_account : nullptr;
    const auto account_key =
        account_keys_by_player.find(invitation.sender_player_id.Value());
    if (parsed_sender == nullptr ||
        account_key == account_keys_by_player.end() ||
        parsed_sender->Key() != account_key->second) {
      return ContractError("收到申请身份与名单账号不一致");
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ServiceError> ValidateConversation(
    const SocialConversationSnapshot& snapshot,
    const SocialHistoryRequest& request) {
  const auto actor_account =
      domain::SocialTarget::Parse(snapshot.actor_account_name);
  const auto peer_account =
      domain::SocialTarget::Parse(snapshot.peer_account_name);
  const auto* parsed_actor = actor_account ? &*actor_account : nullptr;
  const auto* parsed_peer = peer_account ? &*peer_account : nullptr;
  if (snapshot.actor_player_id != request.player_id ||
      !snapshot.peer_player_id.IsValid() ||
      snapshot.peer_player_id == request.player_id || parsed_actor == nullptr ||
      parsed_peer == nullptr ||
      !request.target_account.Matches(snapshot.peer_player_id,
                                      snapshot.peer_account_name)) {
    return ContractError("会话双方身份与查询条件不一致");
  }
  if (parsed_actor->Key() == parsed_peer->Key()) {
    return ContractError("会话双方使用了相同账号唯一键");
  }
  if (!snapshot.relationship_allowed) {
    if (!snapshot.messages.empty()) {
      return ContractError("无权读取的会话仍返回了正文");
    }
    return ServiceError{.code = ErrorCode::kConflict,
                        .safe_message = "该账号不是好友"};
  }
  if (snapshot.messages.size() > request.limit) {
    return ContractError("会话记录超过请求上限");
  }

  std::uint64_t previous_message_id = 0U;
  for (const auto& message : snapshot.messages) {
    if (!message.message_id.IsValid() ||
        message.message_id.Value() <= previous_message_id) {
      return ContractError("会话消息游标无效或未严格升序");
    }
    previous_message_id = message.message_id.Value();
    const bool actor_to_peer =
        message.sender_player_id == snapshot.actor_player_id &&
        message.target_player_id == snapshot.peer_player_id;
    const bool peer_to_actor =
        message.sender_player_id == snapshot.peer_player_id &&
        message.target_player_id == snapshot.actor_player_id;
    if (!actor_to_peer && !peer_to_actor) {
      return ContractError("会话消息不属于查询双方");
    }
  }
  return std::nullopt;
}

}  // namespace

SocialQueryService::SocialQueryService(SocialSnapshotReader& reader) noexcept
    : reader_(reader) {}

ServiceResult<SocialOverviewSnapshot> SocialQueryService::LoadOverview(
    const SocialOverviewRequest& request) const {
  if (!request.player_id.IsValid()) {
    return InvalidRequest("社交名单请求缺少有效玩家标识");
  }
  auto result = reader_.LoadOverview(request);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto snapshot =
      std::get<std::optional<SocialOverviewSnapshot>>(std::move(result));
  if (!snapshot.has_value()) {
    return MissingPlayer();
  }
  if (auto error = ValidateOverview(*snapshot, request)) {
    return *error;
  }
  return std::move(*snapshot);
}

ServiceResult<SocialConversationSnapshot> SocialQueryService::LoadHistory(
    const SocialHistoryRequest& request) const {
  if (!request.player_id.IsValid() || request.limit == 0U ||
      request.limit > kMaximumPrivateHistoryMessages) {
    return InvalidRequest("私聊历史请求的玩家或数量无效");
  }
  auto result = reader_.LoadHistory(request);
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  auto snapshot =
      std::get<std::optional<SocialConversationSnapshot>>(std::move(result));
  if (!snapshot.has_value()) {
    return InvalidRequest("没有这个本地账号；对方需先登录一次");
  }
  if (auto error = ValidateConversation(*snapshot, request)) {
    return *error;
  }
  return std::move(*snapshot);
}

}  // namespace bf::application
