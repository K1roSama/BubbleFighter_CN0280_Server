#include "persistence/mysql/social_spaces_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

#include "features/chat/user_message.h"
#include "persistence/mysql/sql_connection.h"
#include "protocol/codec/text_codec.h"

namespace bf::infrastructure::mysql {
namespace {
using namespace application;
using Action = SocialSpaceAction;

ServiceError Denied(std::string message) {
  return {.code = ErrorCode::kPermissionDenied,
          .safe_message = std::move(message)};
}

bool Friends(bf::infrastructure::mysql::WriteTransaction& tx, std::uint64_t a,
             std::uint64_t b) {
  return a != b &&
         tx.Exec(
               "SELECT EXISTS(SELECT 1 FROM `Friendships` WHERE `FirstPlayerId`=LEAST($1,$2) AND "
               "`SecondPlayerId`=GREATEST($1,$2)) AND NOT EXISTS(SELECT 1 FROM `PlayerBlocks` WHERE "
               "(`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR (`OwnerPlayerId`=$2 AND "
               "`BlockedPlayerId`=$1))",
               bf::infrastructure::mysql::Parameters{tx, a, b})
             .OneRow()[0]
             .As<bool>();
}

bool Capacity(bf::infrastructure::mysql::WriteTransaction& tx,
              std::uint64_t player) {
  return tx
      .Exec("SELECT count(*)<16 FROM `GroupChatMembers` WHERE `PlayerId`=$1",
            bf::infrastructure::mysql::Parameters{tx, player})
      .OneRow()[0]
      .As<bool>();
}

SocialSpacesSnapshot Snapshot(bf::infrastructure::mysql::WriteTransaction& tx,
                              domain::PlayerId player) {
  SocialSpacesSnapshot result{.player = player};
  for (
      const auto& row : tx.Exec(
          "SELECT `CategoryId`,`Name` FROM `FriendCategories` WHERE `OwnerPlayerId`=$1 ORDER BY "
          "`CategoryId`",
          bf::infrastructure::mysql::Parameters{tx, player.Value()})) {
    FriendCategory category{.id = row[0].As<std::uint32_t>(),
                            .name = row[1].As<std::string>()};
    for (
        const auto& member : tx.Exec(
            "SELECT `FriendPlayerId` FROM `FriendCategoryMembers` WHERE `OwnerPlayerId`=$1 AND "
            "`CategoryId`=$2 ORDER BY `FriendPlayerId`",
            bf::infrastructure::mysql::Parameters{tx, player.Value(),
                                                  category.id}))
      category.members.emplace_back(member[0].As<std::uint64_t>());
    result.categories.push_back(std::move(category));
  }
  for (
      const auto& row : tx.Exec(
          "SELECT `ChatId` FROM `GroupChatMembers` WHERE `PlayerId`=$1 ORDER BY `ChatId`",
          bf::infrastructure::mysql::Parameters{tx, player.Value()})) {
    GroupChat chat{.id = row[0].As<std::uint32_t>()};
    for (
        const auto& member : tx.Exec(
            "SELECT m.`PlayerId`,p.`Nickname` FROM `GroupChatMembers` m JOIN `PlayerProfiles` p "
            "USING(`PlayerId`) WHERE `ChatId`=$1 ORDER BY m.`PlayerId`",
            bf::infrastructure::mysql::Parameters{tx, chat.id}))
      chat.members.push_back(
          {.player = domain::PlayerId{member[0].As<std::uint64_t>()},
           .nickname = member[1].As<std::string>()});
    result.chats.push_back(std::move(chat));
  }
  for (
      const auto& row : tx.Exec(
          "SELECT i.`ChatId`,i.`InviterPlayerId`,p.`Nickname` FROM `GroupChatInvitations` i JOIN "
          "`PlayerProfiles` p ON p.`PlayerId`=i.`InviterPlayerId` WHERE i.`PlayerId`=$1 AND "
          "i.`ExpiresAt`>CURRENT_TIMESTAMP(6) AND EXISTS(SELECT 1 FROM `GroupChatMembers` m WHERE "
          "m.`ChatId`=i.`ChatId` AND m.`PlayerId`=i.`InviterPlayerId`) ORDER BY i.`ChatId` LIMIT 32",
          bf::infrastructure::mysql::Parameters{tx, player.Value()}))
    result.invitations.push_back(
        {.chat = row[0].As<std::uint32_t>(),
         .inviter = {.player = domain::PlayerId{row[1].As<std::uint64_t>()},
                     .nickname = row[2].As<std::string>()}});
  return result;
}
}  // namespace

application::ServiceResult<application::SocialSpacesChange>
MySqlSocialSpacesRepository::Execute(const application::SocialSpaceCommand& c) {
  const auto a = c.actor.Value();
  const auto t = c.target;
  auto id = c.space;
  if (!a || a >= 0x80000000ULL) return Denied("玩家编号不支持多人通讯");
  const bool text_action = c.action == Action::kCategoryCreate ||
                           c.action == Action::kCategoryRename ||
                           c.action == Action::kChatMessage;
  if (text_action) {
    const auto text = domain::UserMessage::Parse(c.text);
    if (const auto* error =
            std::get_if<domain::UserMessageValidationError>(&text))
      return Denied(
          std::string{domain::UserMessageValidationErrorMessage(*error)});
    if (c.action != Action::kChatMessage && c.text.size() > 31U)
      return Denied("分组名称最多31个字符");
  } else if (!c.text.empty())
    return Denied("此操作不能携带文本");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<ServiceError>(&acquired)) return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    std::set<std::uint64_t> affected{a};
    // Serialize each chat before taking profile locks. Category/friend
    // operations take profiles only, in the same ascending order as friendship
    // transactions.
    if (static_cast<unsigned>(c.action) >= 11U) {
      if (tx.Exec(
                "SELECT `ChatId` FROM `GroupChats` WHERE `ChatId`=$1 FOR UPDATE",
                bf::infrastructure::mysql::Parameters{tx, id})
              .empty())
        return Denied("群聊不存在或已结束");
    }
    const auto locked = tx.Exec(
        "SELECT `PlayerId`,`Nickname` FROM `PlayerProfiles` WHERE `PlayerId` IN ($1,$2,$3) AND "
        "`IsCharacterCreated` ORDER BY `PlayerId` FOR UPDATE",
        bf::infrastructure::mysql::Parameters{
            tx, a, t, c.action == Action::kChatCreate ? id : 0U});
    std::string actor_name;
    for (const auto& row : locked)
      if (row[0].As<std::uint64_t>() == a)
        actor_name = row[1].As<std::string>();
    if (actor_name.empty()) return Denied("玩家资料不存在");
    if (c.action != Action::kSnapshot &&
        tx.Exec("SELECT `PlayerChatDenied`($1)",
                bf::infrastructure::mysql::Parameters{tx, a})
            .OneRow()[0]
            .As<bool>() &&
        (text_action || c.action == Action::kChatCreate ||
         c.action == Action::kChatInvite))
      return Denied("禁言或封禁期间不能执行通讯操作");
    if (static_cast<unsigned>(c.action) <= 5U) {
      if (c.action != Action::kSnapshot &&
          c.action != Action::kCategoryCreate &&
          tx.Exec(
                "SELECT `CategoryId` FROM `FriendCategories` WHERE `OwnerPlayerId`=$1 AND `CategoryId`=$2",
                bf::infrastructure::mysql::Parameters{tx, a, id})
              .empty())
        return Denied("分组不属于当前玩家");
      if (c.action == Action::kCategoryCreate) {
        if (id || t) return Denied("新增分组参数无效");
        if (tx.Exec(
                  "SELECT count(*)>=20 FROM `FriendCategories` WHERE `OwnerPlayerId`=$1",
                  bf::infrastructure::mysql::Parameters{tx, a})
                .OneRow()[0]
                .As<bool>())
          return Denied("最多创建20个好友分组");
        tx.Exec(
            "INSERT INTO `FriendCategories`(`OwnerPlayerId`,`Name`) VALUES($1,$2)",
            bf::infrastructure::mysql::Parameters{
                tx, a, protocol::Utf16ToUtf8(c.text)});
      } else if (c.action == Action::kCategoryRename) {
        tx.Exec(
            "UPDATE `FriendCategories` SET `Name`=$3 WHERE `OwnerPlayerId`=$1 AND `CategoryId`=$2",
            bf::infrastructure::mysql::Parameters{
                tx, a, id, protocol::Utf16ToUtf8(c.text)});
      } else if (c.action == Action::kCategoryDelete) {
        tx.Exec(
            "DELETE FROM `FriendCategoryMembers` WHERE `OwnerPlayerId`=$1 AND `CategoryId`=$2",
            bf::infrastructure::mysql::Parameters{tx, a, id});
        tx.Exec(
            "DELETE FROM `FriendCategories` WHERE `OwnerPlayerId`=$1 AND `CategoryId`=$2",
            bf::infrastructure::mysql::Parameters{tx, a, id});
      } else if (c.action == Action::kCategoryMove ||
                 c.action == Action::kCategoryRemove) {
        if (!Friends(tx, a, t)) return Denied("只能移动当前未屏蔽的好友");
        if (c.action == Action::kCategoryMove)
          tx.Exec(
              "INSERT INTO `FriendCategoryMembers`(`OwnerPlayerId`,`FriendPlayerId`,`CategoryId`) "
              "VALUES($1,$2,$3) ON CONFLICT(`OwnerPlayerId`,`FriendPlayerId`) DO UPDATE SET "
              "`CategoryId`=VALUES(`CategoryId`)",
              bf::infrastructure::mysql::Parameters{tx, a, t, id});
        else
          tx.Exec(
              "DELETE FROM `FriendCategoryMembers` WHERE `OwnerPlayerId`=$1 AND `FriendPlayerId`=$2 AND "
              "`CategoryId`=$3",
              bf::infrastructure::mysql::Parameters{tx, a, t, id});
      }
    } else if (c.action == Action::kChatCreate) {
      // Two explicit invitations turn an existing private conversation into a
      // group. Only the creator joins automatically; recipients must consent.
      if (!id || !t || id == t || !Friends(tx, a, id) || !Friends(tx, a, t))
        return Denied("请选择两名不同且未屏蔽的好友");
      if (!Capacity(tx, a)) return Denied("最多加入16个群聊");
      const auto first = id;
      id = tx.Exec("INSERT INTO `GroupChats` () VALUES () RETURNING `ChatId`")
               .OneRow()[0]
               .As<std::uint32_t>();
      tx.Exec(
          "INSERT INTO `GroupChatMembers`(`ChatId`,`PlayerId`) VALUES($1,$2)",
          bf::infrastructure::mysql::Parameters{tx, id, a});
      for (auto peer : {first, t}) {
        if (!Capacity(tx, peer)) return Denied("受邀好友群聊数量已达上限");
        tx.Exec(
            "INSERT INTO `GroupChatInvitations`(`ChatId`,`PlayerId`,`InviterPlayerId`) VALUES($1,$2,$3)",
            bf::infrastructure::mysql::Parameters{tx, id, peer, a});
        affected.insert(peer);
      }
    } else {
      const auto members = tx.Exec(
          "SELECT `PlayerId` FROM `GroupChatMembers` WHERE `ChatId`=$1 ORDER BY `PlayerId`",
          bf::infrastructure::mysql::Parameters{tx, id});
      const bool member = std::ranges::any_of(members, [&](const auto& row) {
        return row[0].template As<std::uint64_t>() == a;
      });
      if (c.action != Action::kChatAccept && c.action != Action::kChatDecline &&
          !member)
        return Denied("你不是此群聊的成员");
      if (c.action == Action::kChatAccept || c.action == Action::kChatDecline) {
        const auto invitation = tx.Exec(
            "SELECT `InviterPlayerId` FROM `GroupChatInvitations` WHERE `ChatId`=$1 AND `PlayerId`=$2 "
            "AND `ExpiresAt`>CURRENT_TIMESTAMP(6)",
            bf::infrastructure::mysql::Parameters{tx, id, a});
        if (invitation.empty()) return Denied("群聊邀请不存在或已过期");
        const auto inviter = invitation[0][0].As<std::uint64_t>();
        if (c.action == Action::kChatAccept) {
          if (!Friends(tx, a, inviter) ||
              !std::ranges::any_of(members, [&](const auto& row) {
                return row[0].template As<std::uint64_t>() == inviter;
              }))
            return Denied("邀请人已退出或好友关系已变化");
          if (member || members.size() >= 20U || !Capacity(tx, a))
            return Denied("已在群内或群聊人数达到上限");
          for (const auto& row : members) {
            const auto peer = row[0].As<std::uint64_t>();
            if (!tx.Exec(
                       "SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2) OR "
                       "(`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1)",
                       bf::infrastructure::mysql::Parameters{tx, a, peer})
                     .empty())
              return Denied("群成员之间存在屏蔽关系");
          }
          tx.Exec(
              "INSERT INTO `GroupChatMembers`(`ChatId`,`PlayerId`) VALUES($1,$2)",
              bf::infrastructure::mysql::Parameters{tx, id, a});
        }
        tx.Exec(
            "DELETE FROM `GroupChatInvitations` WHERE `ChatId`=$1 AND `PlayerId`=$2",
            bf::infrastructure::mysql::Parameters{tx, id, a});
      } else if (c.action == Action::kChatInvite) {
        if (!Friends(tx, a, t) || !Capacity(tx, t))
          return Denied("只能邀请未屏蔽且群聊数量未达上限的好友");
        if (members.size() >= 20U ||
            std::ranges::any_of(members, [&](const auto& row) {
              return row[0].template As<std::uint64_t>() == t;
            }))
          return Denied("好友已在群内或群聊已满");
        if (tx.Exec(
                  "SELECT count(*)>=20 FROM `GroupChatInvitations` WHERE `ChatId`=$1 AND "
                  "`ExpiresAt`>CURRENT_TIMESTAMP(6)",
                  bf::infrastructure::mysql::Parameters{tx, id})
                .OneRow()[0]
                .As<bool>())
          return Denied("此群聊待处理邀请过多");
        tx.Exec(
            "INSERT INTO `GroupChatInvitations`(`ChatId`,`PlayerId`,`InviterPlayerId`) VALUES($1,$2,$3)"
            " ON CONFLICT(`ChatId`,`PlayerId`) DO UPDATE SET "
            "InviterPlayerId=IF(GroupChatInvitations.ExpiresAt<=CURRENT_TIMESTAMP(6),VALUES(InviterPlayerId),GroupChatInvitations.InviterPlayerId),ExpiresAt=IF(GroupChatInvitations.ExpiresAt<=CURRENT_TIMESTAMP(6),VALUES(ExpiresAt),GroupChatInvitations.ExpiresAt)",
            bf::infrastructure::mysql::Parameters{tx, id, t, a});
        affected.insert(t);
      } else if (c.action == Action::kChatLeave) {
        for (
            const auto& row : tx.Exec(
                "SELECT `PlayerId` FROM `GroupChatInvitations` WHERE `ChatId`=$1 AND `InviterPlayerId`=$2",
                bf::infrastructure::mysql::Parameters{tx, id, a}))
          affected.insert(row[0].As<std::uint64_t>());
        tx.Exec(
            "DELETE FROM `GroupChatInvitations` WHERE `ChatId`=$1 AND (`PlayerId`=$2 OR "
            "`InviterPlayerId`=$2)",
            bf::infrastructure::mysql::Parameters{tx, id, a});
        tx.Exec(
            "DELETE FROM `GroupChatMembers` WHERE `ChatId`=$1 AND `PlayerId`=$2",
            bf::infrastructure::mysql::Parameters{tx, id, a});
      } else if (c.action != Action::kChatMessage &&
                 c.action != Action::kChatHistory)
        return Denied("未知通讯操作");
      for (const auto& row : members)
        affected.insert(row[0].As<std::uint64_t>());
    }
    SocialSpacesChange result;
    if (c.action == Action::kChatMessage) {
      const auto sequence =
          tx.Exec(
                "INSERT INTO `GroupChatMessages`(`ChatId`,`SenderPlayerId`,`MessageText`) VALUES($1,$2,$3) "
                "RETURNING `MessageId`",
                bf::infrastructure::mysql::Parameters{
                    tx, id, a, protocol::Utf16ToUtf8(c.text)})
              .OneRow()[0]
              .As<std::uint64_t>();
      GroupChatMessage message{
          .sequence = sequence,
          .chat = id,
          .sender = {.player = c.actor, .nickname = actor_name},
          .text = protocol::Utf16ToUtf8(c.text)};
      for (auto peer : affected)
        if (tx.Exec(
                  "SELECT NOT EXISTS(SELECT 1 FROM `PlayerBlocks` WHERE (`OwnerPlayerId`=$1 AND "
                  "`BlockedPlayerId`=$2) OR (`OwnerPlayerId`=$2 AND `BlockedPlayerId`=$1))",
                  bf::infrastructure::mysql::Parameters{tx, a, peer})
                .OneRow()[0]
                .As<bool>())
          message.recipients.emplace_back(peer);
      result.messages.push_back(std::move(message));
    } else if (c.action == Action::kChatHistory) {
      // A new or rejoining member cannot read messages predating their join.
      const auto rows = tx.Exec(
          "SELECT h.`MessageId`,h.`SenderPlayerId`,p.`Nickname`,h.`MessageText` FROM "
          "`GroupChatMessages` h JOIN `PlayerProfiles` p ON p.`PlayerId`=h.`SenderPlayerId` JOIN "
          "`GroupChatMembers` m ON m.`ChatId`=h.`ChatId` AND m.`PlayerId`=$2 WHERE h.`ChatId`=$1 AND "
          "h.`SentAt`>=m.`JoinedAt` AND NOT EXISTS(SELECT 1 FROM `PlayerBlocks` b WHERE "
          "(b.`OwnerPlayerId`=$2 AND b.`BlockedPlayerId`=h.`SenderPlayerId`) OR "
          "(b.`OwnerPlayerId`=h.`SenderPlayerId` AND b.`BlockedPlayerId`=$2)) ORDER BY h.`MessageId` "
          "DESC LIMIT 50",
          bf::infrastructure::mysql::Parameters{tx, id, a});
      for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        result.messages.push_back(
            {.sequence = (*it)[0].As<std::uint64_t>(),
             .chat = id,
             .sender = {.player =
                            domain::PlayerId{(*it)[1].As<std::uint64_t>()},
                        .nickname = (*it)[2].As<std::string>()},
             .text = (*it)[3].As<std::string>(),
             .recipients = {c.actor}});
    } else
      for (auto player : affected)
        result.snapshots.push_back(Snapshot(tx, domain::PlayerId{player}));
    tx.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::UniqueViolation&) {
    return Denied("分组名称已存在或请求已经处理");
  } catch (const std::exception&) {
    spdlog::error("social_spaces transaction failed");
    return ServiceError{.code = ErrorCode::kStorageUnavailable,
                        .safe_message = "通讯数据暂时无法保存，请稍后重试"};
  }
}
}  // namespace bf::infrastructure::mysql
