#include "persistence/mysql/social_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "persistence/mysql/sql_connection.h"
#include "protocol/codec/text_codec.h"

namespace bf::infrastructure::mysql {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

application::ServiceError StorageError(std::string message,
                                       bool retryable = false) {
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::move(message), retryable);
}

std::optional<domain::PlayerId> PlayerIdFromDatabase(std::int64_t value) {
  if (value <= 0) {
    return std::nullopt;
  }
  return domain::PlayerId{static_cast<std::uint64_t>(value)};
}

std::optional<std::uint32_t> CountFromDatabase(std::int64_t value) {
  if (value < 0 || static_cast<std::uint64_t>(value) >
                       std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

application::ServiceResult<domain::UserMessage> UserMessageFromDatabase(
    std::string_view text) {
  try {
    // Reading historical messages must not corrupt an otherwise valid social
    // snapshot.
    auto parsed =
        domain::UserMessage::Parse(protocol::Utf8ToUtf16(text), false);
    if (const auto* value = std::get_if<domain::UserMessage>(&parsed)) {
      return *value;
    }
    return MakeError(application::ErrorCode::kInternal,
                     "MySQL 社交正文不满足当前 Unicode 契约");
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "MySQL 社交正文不是有效 UTF-8");
  }
}

std::string MessageUtf8(const domain::UserMessage& message) {
  return protocol::Utf16ToUtf8(message.Text());
}

class MySqlSocialTransaction final : public application::SocialTransaction {
 public:
  explicit MySqlSocialTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::SocialParticipants> LockParticipants(
      const application::SocialTargetLookup& lookup) override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState, "社交事务已结束");
    }
    try {
      const auto rows = transaction_.Exec(
          "SELECT profile.`PlayerId`,profile.`Nickname`,profile.`NicknameKey` FROM `PlayerProfiles` "
          "AS profile JOIN `Accounts` AS account ON account.`AccountId`=profile.`AccountId` WHERE "
          "profile.`PlayerId`=$1 OR ($3=0 AND profile.`NicknameKey`=$2) OR profile.`PlayerId`=$3 "
          "ORDER BY profile.`PlayerId` FOR UPDATE OF profile",
          bf::infrastructure::mysql::Parameters{
              transaction_, lookup.actor_player_id.Value(),
              lookup.target_account_name.Key(),
              lookup.target_account_name.GetPlayerId().Value()});
      std::optional<domain::PlayerId> actor;
      std::optional<domain::PlayerId> target;
      std::string target_account;
      std::string actor_nickname;
      for (const auto& row : rows) {
        const auto player = PlayerIdFromDatabase(row[0].As<std::int64_t>());
        if (!player.has_value()) {
          return MakeError(application::ErrorCode::kInternal,
                           "社交事务读取到无效玩家标识");
        }
        if (*player == lookup.actor_player_id) {
          actor = *player;
          actor_nickname = row[1].As<std::string>();
        }
        if (lookup.target_account_name.Matches(*player,
                                               row[1].As<std::string>())) {
          target = *player;
          target_account = row[1].As<std::string>();
        }
      }
      if (!actor.has_value()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "当前玩家资料不存在");
      }
      if (!target.has_value()) {
        return MakeError(application::ErrorCode::kInvalidInput,
                         "没有这个游戏昵称；请确认对方已经创建角色");
      }
      if (*actor == *target) {
        return MakeError(application::ErrorCode::kInvalidInput,
                         "不能对自己的账号执行社交操作");
      }
      return application::SocialParticipants{
          .actor_player_id = *actor,
          .target_player_id = *target,
          .target_account_name = std::move(target_account),
          .actor_nickname = std::move(actor_nickname)};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return StorageError("锁定社交双方时 MySQL 连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      spdlog::error("锁定社交双方 SQL 失败，sqlstate={}，detail={}",
                    error.Sqlstate(), error.what());
      return StorageError("MySQL 拒绝锁定社交双方");
    } catch (const std::exception&) {
      return StorageError("锁定社交双方时发生未分类错误");
    }
  }

  application::ServiceResult<application::SocialRelationshipSnapshot>
  InspectRelationship(
      const application::SocialParticipants& participants) override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState, "社交事务已结束");
    }
    try {
      const auto actor = participants.actor_player_id.Value();
      const auto target = participants.target_player_id.Value();
      const auto first = std::min(actor, target);
      const auto second = std::max(actor, target);
      const auto rows = transaction_.Exec(
          "SELECT EXISTS(SELECT 1 FROM `Friendships` WHERE `FirstPlayerId`=$3 AND "
          "`SecondPlayerId`=$4),EXISTS(SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND "
          "`BlockedPlayerId`=$2) OR (`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)),EXISTS(SELECT 1 "
          "FROM `PlayerBlocks` WHERE `OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2),EXISTS(SELECT 1 "
          "FROM `FriendRequests` WHERE `SenderPlayerId`=$2 AND `RecipientPlayerId`=$1),(SELECT "
          "count(*) FROM `Friendships` WHERE `FirstPlayerId`=$1 OR `SecondPlayerId`=$1),(SELECT "
          "count(*) FROM `Friendships` WHERE `FirstPlayerId`=$2 OR `SecondPlayerId`=$2),(SELECT "
          "count(*) FROM `FriendRequests` WHERE `SenderPlayerId`=$1 OR "
          "`RecipientPlayerId`=$2),(SELECT count(*) FROM `PlayerBlocks` WHERE `OwnerPlayerId`=$1)",
          bf::infrastructure::mysql::Parameters{transaction_, actor, target,
                                                first, second});
      const auto actor_friends =
          CountFromDatabase(rows[0][4].As<std::int64_t>());
      const auto target_friends =
          CountFromDatabase(rows[0][5].As<std::int64_t>());
      const auto pressure = CountFromDatabase(rows[0][6].As<std::int64_t>());
      const auto actor_blocks =
          CountFromDatabase(rows[0][7].As<std::int64_t>());
      if (!actor_friends.has_value() || !target_friends.has_value() ||
          !pressure.has_value() || !actor_blocks.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "社交关系计数超出应用范围");
      }
      return application::SocialRelationshipSnapshot{
          .are_friends = rows[0][0].As<bool>(),
          .blocked_either_direction = rows[0][1].As<bool>(),
          .actor_has_block = rows[0][2].As<bool>(),
          .incoming_request_exists = rows[0][3].As<bool>(),
          .actor_friend_count = *actor_friends,
          .target_friend_count = *target_friends,
          .pending_request_pressure = *pressure,
          .actor_block_count = *actor_blocks};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return StorageError("读取社交关系时 MySQL 连接中断", true);
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      spdlog::error("读取社交关系 SQL 失败，sqlstate={}，detail={}",
                    error.Sqlstate(), error.what());
      return StorageError("MySQL 拒绝读取社交关系");
    } catch (const std::exception&) {
      return StorageError("读取社交关系时发生未分类错误");
    }
  }

  application::ServiceResult<bool> InsertFriendRequestIfAbsent(
      const application::FriendRequestWrite& write) override {
    try {
      const auto message = write.request_message.has_value()
                               ? MessageUtf8(*write.request_message)
                               : std::string{};
      return !transaction_
                  .Exec(
                      "INSERT INTO `FriendRequests`(`SenderPlayerId`,`RecipientPlayerId`,`RequestMessage`) "
                      "VALUES($1,$2,$3) ON CONFLICT DO NOTHING RETURNING 1",
                      bf::infrastructure::mysql::Parameters{
                          transaction_,
                          write.participants.actor_player_id.Value(),
                          write.participants.target_player_id.Value(), message})
                  .empty();
    } catch (const std::exception&) {
      return StorageError("写入好友申请失败");
    }
  }

  application::ServiceResult<bool> InsertFriendshipIfAbsent(
      const application::SocialParticipants& participants) override {
    try {
      const auto first = std::min(participants.actor_player_id.Value(),
                                  participants.target_player_id.Value());
      const auto second = std::max(participants.actor_player_id.Value(),
                                   participants.target_player_id.Value());
      return !transaction_
                  .Exec(
                      "INSERT INTO `Friendships`(`FirstPlayerId`,`SecondPlayerId`) VALUES($1,$2) ON CONFLICT DO "
                      "NOTHING RETURNING 1",
                      bf::infrastructure::mysql::Parameters{transaction_, first,
                                                            second})
                  .empty();
    } catch (const std::exception&) {
      return StorageError("写入好友关系失败");
    }
  }

  application::ServiceResult<bool> DeleteFriendship(
      const application::SocialParticipants& participants) override {
    try {
      const auto first = std::min(participants.actor_player_id.Value(),
                                  participants.target_player_id.Value());
      const auto second = std::max(participants.actor_player_id.Value(),
                                   participants.target_player_id.Value());
      return transaction_
                 .Exec(
                     "DELETE FROM `Friendships` WHERE `FirstPlayerId`=$1 AND `SecondPlayerId`=$2",
                     bf::infrastructure::mysql::Parameters{transaction_, first,
                                                           second})
                 .AffectedRows() == 1U;
    } catch (const std::exception&) {
      return StorageError("删除好友关系失败");
    }
  }

  application::ServiceResult<bool> DeleteRequestsBetween(
      const application::SocialParticipants& participants) override {
    try {
      return transaction_
                 .Exec(
                     "DELETE FROM `FriendRequests` WHERE (`SenderPlayerId`=$1 AND `RecipientPlayerId`=$2) OR "
                     "(`SenderPlayerId`=$2 AND `RecipientPlayerId`=$1)",
                     bf::infrastructure::mysql::Parameters{
                         transaction_, participants.actor_player_id.Value(),
                         participants.target_player_id.Value()})
                 .AffectedRows() > 0U;
    } catch (const std::exception&) {
      return StorageError("删除双方好友申请失败");
    }
  }

  application::ServiceResult<bool> DeleteIncomingRequest(
      const application::SocialParticipants& participants) override {
    try {
      return transaction_
                 .Exec(
                     "DELETE FROM `FriendRequests` WHERE `SenderPlayerId`=$2 AND `RecipientPlayerId`=$1",
                     bf::infrastructure::mysql::Parameters{
                         transaction_, participants.actor_player_id.Value(),
                         participants.target_player_id.Value()})
                 .AffectedRows() == 1U;
    } catch (const std::exception&) {
      return StorageError("删除收到的好友申请失败");
    }
  }

  application::ServiceResult<bool> InsertBlockIfAbsent(
      const application::SocialParticipants& participants) override {
    try {
      return !transaction_
                  .Exec(
                      "INSERT INTO `PlayerBlocks`(`OwnerPlayerId`,`BlockedPlayerId`) VALUES($1,$2) ON CONFLICT DO"
                      " NOTHING RETURNING 1",
                      bf::infrastructure::mysql::Parameters{
                          transaction_, participants.actor_player_id.Value(),
                          participants.target_player_id.Value()})
                  .empty();
    } catch (const std::exception&) {
      return StorageError("写入屏蔽关系失败");
    }
  }

  application::ServiceResult<bool> DeleteOwnedBlock(
      const application::SocialParticipants& participants) override {
    try {
      return transaction_
                 .Exec(
                     "DELETE FROM `PlayerBlocks` WHERE `OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2",
                     bf::infrastructure::mysql::Parameters{
                         transaction_, participants.actor_player_id.Value(),
                         participants.target_player_id.Value()})
                 .AffectedRows() == 1U;
    } catch (const std::exception&) {
      return StorageError("删除屏蔽关系失败");
    }
  }

  application::ServiceResult<domain::MessageId> InsertPrivateMessage(
      const application::PrivateMessageWrite& write) override {
    try {
      if (transaction_
              .Exec("SELECT `PlayerChatDenied`($1)",
                    bf::infrastructure::mysql::Parameters{
                        transaction_,
                        write.participants.actor_player_id.Value()})[0][0]
              .As<bool>())
        return MakeError(application::ErrorCode::kPermissionDenied,
                         "账号处于禁言或封禁期间，暂时无法发送私聊");
      const auto rows = transaction_.Exec(
          "INSERT INTO `PrivateMessages`(`SenderPlayerId`,`RecipientPlayerId`,`MessageBody`) "
          "VALUES($1,$2,$3) RETURNING `MessageId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, write.participants.actor_player_id.Value(),
              write.participants.target_player_id.Value(),
              MessageUtf8(write.message)});
      const auto value = PlayerIdFromDatabase(rows[0][0].As<std::int64_t>());
      if (!value.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "私聊写入返回无效消息游标");
      }
      return domain::MessageId{value->Value()};
    } catch (const std::exception&) {
      return StorageError("写入私聊失败");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_) {
      return MakeError(application::ErrorCode::kInvalidState, "社交事务已结束");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      completed_ = true;
      return StorageError("提交社交事务时 MySQL 连接中断", true);
    } catch (const std::exception&) {
      completed_ = true;
      return StorageError("提交社交事务失败");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool completed_{false};
};

}  // namespace

MySqlSocialRepository::MySqlSocialRepository(MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::unique_ptr<application::SocialTransaction>>
MySqlSocialRepository::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    return std::unique_ptr<application::SocialTransaction>{
        std::make_unique<MySqlSocialTransaction>(
            std::get<MySqlConnectionPool::Lease>(std::move(lease_result)))};
  } catch (const std::exception&) {
    return StorageError("开始社交事务失败");
  }
}

application::ServiceResult<std::optional<application::SocialOverviewSnapshot>>
MySqlSocialRepository::LoadOverview(
    const application::SocialOverviewRequest& request) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto owner =
        transaction.Exec("SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1",
                         bf::infrastructure::mysql::Parameters{
                             transaction, request.player_id.Value()});
    if (owner.empty()) {
      transaction.Commit();
      return std::optional<application::SocialOverviewSnapshot>{};
    }

    std::map<domain::PlayerId, application::SocialRosterRecord> roster;
    const auto friendship_rows = transaction.Exec(
        "SELECT CASE WHEN friendship.`FirstPlayerId`=$1 THEN friendship.`SecondPlayerId` ELSE "
        "friendship.`FirstPlayerId` END AS peer_id,profile.`Nickname` FROM `Friendships` AS "
        "friendship JOIN `PlayerProfiles` AS profile ON profile.`PlayerId`=CASE WHEN "
        "friendship.`FirstPlayerId`=$1 THEN friendship.`SecondPlayerId` ELSE "
        "friendship.`FirstPlayerId` END JOIN `Accounts` AS account ON "
        "account.`AccountId`=profile.`AccountId` WHERE friendship.`FirstPlayerId`=$1 OR "
        "friendship.`SecondPlayerId`=$1 ORDER BY peer_id",
        bf::infrastructure::mysql::Parameters{transaction,
                                              request.player_id.Value()});
    for (const auto& row : friendship_rows) {
      const auto peer = PlayerIdFromDatabase(row[0].As<std::int64_t>());
      if (!peer.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "好友名单包含无效玩家标识");
      }
      roster.emplace(*peer,
                     application::SocialRosterRecord{
                         .player_id = *peer,
                         .account_name = row[1].As<std::string>(),
                         .state = application::SocialRosterState::kFriend});
    }

    const auto request_rows = transaction.Exec(
        "SELECT CASE WHEN `SenderPlayerId`=$1 THEN `RecipientPlayerId` ELSE `SenderPlayerId` END AS"
        " peer_id,`SenderPlayerId`=$1 AS outgoing,profile.`Nickname` FROM `FriendRequests` AS "
        "request JOIN `PlayerProfiles` AS profile ON profile.`PlayerId`=CASE WHEN "
        "request.`SenderPlayerId`=$1 THEN request.`RecipientPlayerId` ELSE request.`SenderPlayerId`"
        " END JOIN `Accounts` AS account ON account.`AccountId`=profile.`AccountId` WHERE "
        "`SenderPlayerId`=$1 OR `RecipientPlayerId`=$1 ORDER BY peer_id",
        bf::infrastructure::mysql::Parameters{transaction,
                                              request.player_id.Value()});
    for (const auto& row : request_rows) {
      const auto peer = PlayerIdFromDatabase(row[0].As<std::int64_t>());
      if (!peer.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "好友申请包含无效玩家标识");
      }
      // Requests are directed: A->B and B->A may both be pending. The wire
      // roster has one state per peer. Prefer an actionable incoming request,
      // independent of SQL row order, and keep established friendships above
      // stale requests. Loading existing records must not lock either user out.
      const auto existing = roster.find(*peer);
      if (existing != roster.end() &&
          (existing->second.state == application::SocialRosterState::kFriend ||
           existing->second.state ==
               application::SocialRosterState::kIncomingRequest)) {
        continue;
      }
      roster.insert_or_assign(
          *peer,
          application::SocialRosterRecord{
              .player_id = *peer,
              .account_name = row[2].As<std::string>(),
              .state = row[1].As<bool>()
                           ? application::SocialRosterState::kOutgoingRequest
                           : application::SocialRosterState::kIncomingRequest});
    }

    const auto block_rows = transaction.Exec(
        "SELECT block.`BlockedPlayerId`,profile.`Nickname` FROM `PlayerBlocks` AS block JOIN "
        "`PlayerProfiles` AS profile ON profile.`PlayerId`=block.`BlockedPlayerId` JOIN `Accounts` "
        "AS account ON account.`AccountId`=profile.`AccountId` WHERE block.`OwnerPlayerId`=$1 ORDER"
        " BY block.`BlockedPlayerId`",
        bf::infrastructure::mysql::Parameters{transaction,
                                              request.player_id.Value()});
    for (const auto& row : block_rows) {
      const auto peer = PlayerIdFromDatabase(row[0].As<std::int64_t>());
      if (!peer.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "屏蔽名单包含无效玩家标识");
      }
      // Blocking is reversible and does not delete an existing friendship.
      // It also takes precedence over any legacy pending request for this peer.
      roster.insert_or_assign(
          *peer, application::SocialRosterRecord{
                     .player_id = *peer,
                     .account_name = row[1].As<std::string>(),
                     .state = application::SocialRosterState::kBlocked});
    }

    application::SocialOverviewSnapshot snapshot{.owner_player_id =
                                                     request.player_id};
    const auto invitation_rows = transaction.Exec(
        "SELECT request.`SenderPlayerId`,profile.`Nickname`,request.`RequestMessage` FROM "
        "`FriendRequests` AS request JOIN `PlayerProfiles` AS profile ON "
        "profile.`PlayerId`=request.`SenderPlayerId` JOIN `Accounts` AS account ON "
        "account.`AccountId`=profile.`AccountId` WHERE request.`RecipientPlayerId`=$1 ORDER BY "
        "request.`CreatedAt`,request.`SenderPlayerId`",
        bf::infrastructure::mysql::Parameters{transaction,
                                              request.player_id.Value()});
    snapshot.invitations.reserve(invitation_rows.size());
    for (const auto& row : invitation_rows) {
      const auto sender = PlayerIdFromDatabase(row[0].As<std::int64_t>());
      if (!sender.has_value()) {
        return MakeError(application::ErrorCode::kInternal,
                         "好友申请包含无效发起玩家");
      }
      const auto effective = roster.find(*sender);
      if (effective == roster.end() ||
          effective->second.state !=
              application::SocialRosterState::kIncomingRequest) {
        continue;  // No accept prompt for an established friend or blocked
                   // peer.
      }
      std::optional<domain::UserMessage> message;
      const auto text = row[2].As<std::string>();
      if (!text.empty()) {
        auto parsed = UserMessageFromDatabase(text);
        if (const auto* error =
                std::get_if<application::ServiceError>(&parsed)) {
          return *error;
        }
        message = std::get<domain::UserMessage>(std::move(parsed));
      }
      snapshot.invitations.push_back(
          {.sender_player_id = *sender,
           .sender_account_name = row[1].As<std::string>(),
           .request_message = std::move(message)});
    }

    snapshot.roster.reserve(roster.size() +
                            (request.search_account.has_value() ? 1U : 0U));
    for (auto& [player_id, record] : roster) {
      static_cast<void>(player_id);
      snapshot.roster.push_back(std::move(record));
    }

    if (request.search_account.has_value()) {
      const auto search_rows = transaction.Exec(
          "SELECT profile.`PlayerId`,profile.`Nickname` FROM `Accounts` AS account JOIN "
          "`PlayerProfiles` AS profile ON profile.`AccountId`=account.`AccountId` WHERE ($2=0 AND "
          "profile.`NicknameKey`=$1) OR profile.`PlayerId`=$2",
          bf::infrastructure::mysql::Parameters{
              transaction, request.search_account->Key(),
              request.search_account->GetPlayerId().Value()});
      if (!search_rows.empty()) {
        const auto peer =
            PlayerIdFromDatabase(search_rows[0][0].As<std::int64_t>());
        if (!peer.has_value()) {
          return MakeError(application::ErrorCode::kInternal,
                           "账号搜索返回无效玩家标识");
        }
        if (*peer != request.player_id && !roster.contains(*peer)) {
          snapshot.roster.push_back(
              {.player_id = *peer,
               .account_name = search_rows[0][1].As<std::string>(),
               .state = application::SocialRosterState::kSearchResult});
        }
      }
    }
    transaction.Commit();
    return std::optional<application::SocialOverviewSnapshot>{
        std::move(snapshot)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return StorageError("读取社交名单时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取社交名单 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return StorageError("MySQL 拒绝读取社交名单");
  } catch (const std::exception&) {
    return StorageError("读取社交名单时发生未分类错误");
  }
}

application::ServiceResult<
    std::optional<application::SocialConversationSnapshot>>
MySqlSocialRepository::LoadHistory(
    const application::SocialHistoryRequest& request) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::SnapshotTransaction transaction{
        lease.Connection()};
    const auto actor_rows = transaction.Exec(
        "SELECT profile.`Nickname` FROM `PlayerProfiles` AS profile JOIN `Accounts` AS account ON "
        "account.`AccountId`=profile.`AccountId` WHERE profile.`PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction,
                                              request.player_id.Value()});
    if (actor_rows.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "当前玩家资料不存在");
    }
    const auto peer_rows = transaction.Exec(
        "SELECT profile.`PlayerId`,profile.`Nickname` FROM `Accounts` AS account JOIN "
        "`PlayerProfiles` AS profile ON profile.`AccountId`=account.`AccountId` WHERE ($2=0 AND "
        "profile.`NicknameKey`=$1) OR profile.`PlayerId`=$2",
        bf::infrastructure::mysql::Parameters{
            transaction, request.target_account.Key(),
            request.target_account.GetPlayerId().Value()});
    if (peer_rows.empty()) {
      transaction.Commit();
      return std::optional<application::SocialConversationSnapshot>{};
    }
    const auto peer = PlayerIdFromDatabase(peer_rows[0][0].As<std::int64_t>());
    if (!peer.has_value() || *peer == request.player_id) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "私聊历史目标账号无效");
    }
    const auto first = std::min(request.player_id.Value(), peer->Value());
    const auto second = std::max(request.player_id.Value(), peer->Value());
    const auto relationship = transaction.Exec(
        "SELECT EXISTS(SELECT 1 FROM `Friendships` WHERE `FirstPlayerId`=$1 AND "
        "`SecondPlayerId`=$2) AND NOT EXISTS(SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1"
        " AND `BlockedPlayerId`=$2) OR (`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1))",
        bf::infrastructure::mysql::Parameters{transaction, first, second});
    const auto allowed = relationship[0][0].As<bool>();
    application::SocialConversationSnapshot snapshot{
        .actor_player_id = request.player_id,
        .actor_account_name = actor_rows[0][0].As<std::string>(),
        .peer_player_id = *peer,
        .peer_account_name = peer_rows[0][1].As<std::string>(),
        .relationship_allowed = allowed};
    if (allowed) {
      const auto messages = transaction.Exec(
          "SELECT `MessageId`,`SenderPlayerId`,`RecipientPlayerId`,`MessageBody` FROM (SELECT "
          "`MessageId`,`SenderPlayerId`,`RecipientPlayerId`,`MessageBody` FROM `PrivateMessages` "
          "WHERE (`SenderPlayerId`=$1 AND `RecipientPlayerId`=$2) OR (`SenderPlayerId`=$2 AND "
          "`RecipientPlayerId`=$1) ORDER BY `MessageId` DESC LIMIT $3) AS recent ORDER BY `MessageId`",
          bf::infrastructure::mysql::Parameters{transaction,
                                                request.player_id.Value(),
                                                peer->Value(), request.limit});
      snapshot.messages.reserve(messages.size());
      for (const auto& row : messages) {
        const auto message_id = PlayerIdFromDatabase(row[0].As<std::int64_t>());
        const auto sender = PlayerIdFromDatabase(row[1].As<std::int64_t>());
        const auto recipient = PlayerIdFromDatabase(row[2].As<std::int64_t>());
        auto message = UserMessageFromDatabase(row[3].As<std::string>());
        const auto* parsed_message = std::get_if<domain::UserMessage>(&message);
        if (!message_id.has_value() || !sender.has_value() ||
            !recipient.has_value() || parsed_message == nullptr) {
          if (const auto* error =
                  std::get_if<application::ServiceError>(&message)) {
            return *error;
          }
          return MakeError(application::ErrorCode::kInternal,
                           "私聊历史包含无效持久化字段");
        }
        snapshot.messages.push_back(
            {.message_id = domain::MessageId{message_id->Value()},
             .sender_player_id = *sender,
             .target_player_id = *recipient,
             .message = *parsed_message});
      }
    }
    transaction.Commit();
    return std::optional<application::SocialConversationSnapshot>{
        std::move(snapshot)};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return StorageError("读取私聊历史时 MySQL 连接中断", true);
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("读取私聊历史 SQL 失败，sqlstate={}，detail={}",
                  error.Sqlstate(), error.what());
    return StorageError("MySQL 拒绝读取私聊历史");
  } catch (const std::exception&) {
    return StorageError("读取私聊历史时发生未分类错误");
  }
}

}  // namespace bf::infrastructure::mysql
