#include "features/social/social_service.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace bf::application {
namespace {

ServiceError InvalidActor() {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = "社交请求缺少有效玩家标识"};
}

ServiceError Conflict(std::string_view message) {
  return ServiceError{.code = ErrorCode::kConflict,
                      .safe_message = std::string{message}};
}

ServiceError ContractError(std::string_view detail) {
  return ServiceError{.code = ErrorCode::kInternal,
                      .safe_message = std::string{"社交事务返回了无效状态："} +
                                      std::string{detail}};
}

ServiceResult<std::unique_ptr<SocialTransaction>> BeginTransaction(
    SocialUnitOfWork& unit_of_work) {
  auto transaction_result = unit_of_work.Begin();
  if (const auto* error = std::get_if<ServiceError>(&transaction_result)) {
    return *error;
  }
  auto transaction = std::get<std::unique_ptr<SocialTransaction>>(
      std::move(transaction_result));
  if (!transaction) {
    return ContractError("事务工厂返回空对象");
  }
  return transaction;
}

ServiceResult<SocialParticipants> LockAndValidate(
    SocialTransaction& transaction, const SocialTargetLookup& lookup) {
  auto participants_result = transaction.LockParticipants(lookup);
  if (const auto* error = std::get_if<ServiceError>(&participants_result)) {
    return *error;
  }
  auto participants =
      std::get<SocialParticipants>(std::move(participants_result));
  const auto account =
      domain::SocialTarget::Parse(participants.target_account_name);
  const auto* parsed_account = account ? &*account : nullptr;
  if (!participants.actor_player_id.IsValid() ||
      !participants.target_player_id.IsValid() ||
      participants.actor_player_id != lookup.actor_player_id ||
      participants.actor_player_id == participants.target_player_id ||
      parsed_account == nullptr ||
      !lookup.target_account_name.Matches(participants.target_player_id,
                                          participants.target_account_name)) {
    return ContractError("锁定身份与请求不一致");
  }
  return participants;
}

template <typename Value>
ServiceResult<Value> TakeResult(ServiceResult<Value> result) {
  if (const auto* error = std::get_if<ServiceError>(&result)) {
    return *error;
  }
  return std::get<Value>(std::move(result));
}

ServiceResult<bool> AddFriendRequest(
    SocialTransaction& transaction, const FriendChangeCommand& command,
    const SocialParticipants& participants,
    const SocialRelationshipSnapshot& relationship) {
  if (relationship.blocked_either_direction) {
    return Conflict("无法添加被屏蔽的账号");
  }
  if (relationship.are_friends) {
    return Conflict("已经是好友");
  }
  if (relationship.pending_request_pressure >= kMaximumPendingRequests) {
    return Conflict("待处理申请已达上限");
  }
  return TakeResult(transaction.InsertFriendRequestIfAbsent(
      FriendRequestWrite{.participants = participants,
                         .request_message = command.request_message}));
}

ServiceResult<bool> AcceptFriendRequest(
    SocialTransaction& transaction, const SocialParticipants& participants,
    const SocialRelationshipSnapshot& relationship) {
  if (relationship.blocked_either_direction) {
    return Conflict("账号已屏蔽");
  }
  if (relationship.are_friends) {
    return Conflict("已经是好友");
  }
  if (relationship.actor_friend_count >= kMaximumFriends ||
      relationship.target_friend_count >= kMaximumFriends) {
    return Conflict("好友已达100人上限");
  }
  if (!relationship.incoming_request_exists) {
    return Conflict("没有来自该账号的好友申请");
  }

  auto inserted_result = transaction.InsertFriendshipIfAbsent(participants);
  if (const auto* error = std::get_if<ServiceError>(&inserted_result)) {
    return *error;
  }
  if (!std::get<bool>(inserted_result)) {
    return ContractError("有效申请未能建立好友关系");
  }
  auto deleted_result = transaction.DeleteRequestsBetween(participants);
  if (const auto* error = std::get_if<ServiceError>(&deleted_result)) {
    return *error;
  }
  if (!std::get<bool>(deleted_result)) {
    return ContractError("接受申请后未能删除当前申请");
  }
  return true;
}

ServiceResult<bool> RemoveFriendship(SocialTransaction& transaction,
                                     const SocialParticipants& participants) {
  auto removed_result = transaction.DeleteFriendship(participants);
  if (const auto* error = std::get_if<ServiceError>(&removed_result)) {
    return *error;
  }
  auto requests_result = transaction.DeleteRequestsBetween(participants);
  if (const auto* error = std::get_if<ServiceError>(&requests_result)) {
    return *error;
  }
  return std::get<bool>(removed_result) || std::get<bool>(requests_result);
}

ServiceResult<bool> BlockPlayer(
    SocialTransaction& transaction, const SocialParticipants& participants,
    const SocialRelationshipSnapshot& relationship) {
  if (!relationship.actor_has_block &&
      relationship.actor_block_count >= kMaximumBlockedPlayers) {
    return Conflict("屏蔽名单已达100人上限");
  }
  auto requests_result = transaction.DeleteRequestsBetween(participants);
  if (const auto* error = std::get_if<ServiceError>(&requests_result)) {
    return *error;
  }
  auto block_result = transaction.InsertBlockIfAbsent(participants);
  if (const auto* error = std::get_if<ServiceError>(&block_result)) {
    return *error;
  }
  return std::get<bool>(requests_result) || std::get<bool>(block_result);
}

ServiceResult<bool> ApplyChange(
    SocialTransaction& transaction, const FriendChangeCommand& command,
    const SocialParticipants& participants,
    const SocialRelationshipSnapshot& relationship) {
  switch (command.action) {
    case FriendAction::kAdd:
      return AddFriendRequest(transaction, command, participants, relationship);
    case FriendAction::kAccept:
      return AcceptFriendRequest(transaction, participants, relationship);
    case FriendAction::kRemove:
      return RemoveFriendship(transaction, participants);
    case FriendAction::kBlock:
      return BlockPlayer(transaction, participants, relationship);
    case FriendAction::kDecline: {
      auto result = transaction.DeleteIncomingRequest(participants);
      if (const auto* error = std::get_if<ServiceError>(&result)) {
        return *error;
      }
      return std::get<bool>(result);
    }
    case FriendAction::kUnblock: {
      auto result = transaction.DeleteOwnedBlock(participants);
      if (const auto* error = std::get_if<ServiceError>(&result)) {
        return *error;
      }
      return std::get<bool>(result);
    }
  }
  return ContractError("未知好友动作");
}

}  // namespace

FriendRelationshipService::FriendRelationshipService(
    SocialUnitOfWork& unit_of_work) noexcept
    : unit_of_work_(unit_of_work) {}

ServiceResult<FriendChangeResult> FriendRelationshipService::Change(
    const FriendChangeCommand& command) const {
  if (!command.actor_player_id.IsValid()) {
    return InvalidActor();
  }
  if (command.action != FriendAction::kAdd &&
      command.request_message.has_value()) {
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "只有好友申请可以携带附言"};
  }

  auto begin_result = BeginTransaction(unit_of_work_);
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction =
      std::get<std::unique_ptr<SocialTransaction>>(std::move(begin_result));

  const SocialTargetLookup lookup{
      .actor_player_id = command.actor_player_id,
      .target_account_name = command.target_account_name};
  auto participants_result = LockAndValidate(*transaction, lookup);
  if (const auto* error = std::get_if<ServiceError>(&participants_result)) {
    return *error;
  }
  auto participants =
      std::get<SocialParticipants>(std::move(participants_result));

  auto relationship_result = transaction->InspectRelationship(participants);
  if (const auto* error = std::get_if<ServiceError>(&relationship_result)) {
    return *error;
  }
  const auto relationship =
      std::get<SocialRelationshipSnapshot>(relationship_result);
  auto changed_result =
      ApplyChange(*transaction, command, participants, relationship);
  if (const auto* error = std::get_if<ServiceError>(&changed_result)) {
    return *error;
  }
  const auto changed = std::get<bool>(changed_result);

  if (auto error = transaction->Commit()) {
    return *error;
  }
  return FriendChangeResult{
      .target_player_id = participants.target_player_id,
      .target_account_name = std::move(participants.target_account_name),
      .changed = changed};
}

PrivateMessageService::PrivateMessageService(
    SocialUnitOfWork& unit_of_work) noexcept
    : unit_of_work_(unit_of_work) {}

ServiceResult<PrivateMessageResult> PrivateMessageService::Send(
    const SendPrivateMessageCommand& command) const {
  if (!command.sender_player_id.IsValid()) {
    return InvalidActor();
  }

  auto begin_result = BeginTransaction(unit_of_work_);
  if (const auto* error = std::get_if<ServiceError>(&begin_result)) {
    return *error;
  }
  auto transaction =
      std::get<std::unique_ptr<SocialTransaction>>(std::move(begin_result));

  const SocialTargetLookup lookup{
      .actor_player_id = command.sender_player_id,
      .target_account_name = command.target_account_name};
  auto participants_result = LockAndValidate(*transaction, lookup);
  if (const auto* error = std::get_if<ServiceError>(&participants_result)) {
    return *error;
  }
  auto participants =
      std::get<SocialParticipants>(std::move(participants_result));

  auto relationship_result = transaction->InspectRelationship(participants);
  if (const auto* error = std::get_if<ServiceError>(&relationship_result)) {
    return *error;
  }
  const auto relationship =
      std::get<SocialRelationshipSnapshot>(relationship_result);
  if (relationship.blocked_either_direction || !relationship.are_friends) {
    return Conflict("只能给已接受申请且未屏蔽的好友发私聊");
  }

  auto message_result = transaction->InsertPrivateMessage(PrivateMessageWrite{
      .participants = participants, .message = command.message});
  if (const auto* error = std::get_if<ServiceError>(&message_result)) {
    return *error;
  }
  const auto message_id = std::get<domain::MessageId>(message_result);
  if (!message_id.IsValid()) {
    return ContractError("私聊写入没有返回有效消息游标");
  }
  if (auto error = transaction->Commit()) {
    return *error;
  }
  return PrivateMessageResult{
      .message_id = message_id,
      .target_player_id = participants.target_player_id,
      .target_account_name = std::move(participants.target_account_name),
      .sender_nickname = std::move(participants.actor_nickname)};
}

}  // namespace bf::application
