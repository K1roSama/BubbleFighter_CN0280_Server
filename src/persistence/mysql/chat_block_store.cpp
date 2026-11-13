#include "persistence/mysql/chat_block_store.h"

#include <limits>

#include "features/characters/nickname.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError Invalid(const char* text) {
  return {.code = application::ErrorCode::kInvalidInput, .safe_message = text};
}

application::ServiceError Unavailable() {
  return {.code = application::ErrorCode::kStorageUnavailable,
          .safe_message = "聊天屏蔽名单暂时不可用"};
}
}  // namespace

application::ServiceResult<std::vector<application::ChatBlockEntry>>
MySqlChatBlockStore::Load(domain::PlayerId owner) {
  if (!owner.IsValid()) return Invalid("当前玩家身份无效");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto rows = tx.Exec(
        "SELECT p.`PlayerId`,p.`Nickname` FROM `PlayerChatBlocks` b JOIN `PlayerProfiles` p ON "
        "p.`PlayerId`=b.`BlockedPlayerId` WHERE b.`OwnerPlayerId`=$1 ORDER BY "
        "b.`CreatedAt`,b.`BlockedPlayerId`",
        bf::infrastructure::mysql::Parameters{tx, owner.Value()});
    if (rows.size() > 50U) return Unavailable();
    std::vector<application::ChatBlockEntry> result;
    for (const auto& row : rows)
      result.push_back({row[0].As<std::uint32_t>(), row[1].As<std::string>()});
    tx.Commit();
    return result;
  } catch (const std::exception&) {
    return Unavailable();
  }
}

application::ServiceResult<application::ChatBlockEntry>
MySqlChatBlockStore::Change(domain::PlayerId owner,
                            const application::ChatBlockTarget& target,
                            bool blocked) {
  auto parsed = domain::Nickname::Parse(target.nickname);
  const auto* name = std::get_if<domain::Nickname>(&parsed);
  if (!owner.IsValid() || !name) return Invalid("玩家昵称无效");
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (tx.Exec(
              "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR UPDATE",
              bf::infrastructure::mysql::Parameters{tx, owner.Value()})
            .empty())
      return Invalid("玩家尚未完成建角");
    // An identity-bearing operation follows the stable ID across renames. The
    // nickname is never allowed to redirect that operation to a different
    // player.
    const auto rows =
        target.player_id
            ? tx.Exec(
                  "SELECT `PlayerId`,`Nickname` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND "
                  "`IsCharacterCreated`",
                  bf::infrastructure::mysql::Parameters{tx, target.player_id})
            : tx.Exec(
                  "SELECT `PlayerId`,`Nickname` FROM `PlayerProfiles` WHERE `NicknameKey`=$1 AND "
                  "`IsCharacterCreated`",
                  bf::infrastructure::mysql::Parameters{tx, name->Key()});
    if (rows.empty()) return Invalid("找不到该玩家");
    const auto id = rows[0][0].As<std::uint64_t>();
    if (id == owner.Value()) return Invalid("不能屏蔽自己");
    if (id > std::numeric_limits<std::uint32_t>::max())
      return Invalid("玩家身份超出客户端范围");
    const auto present = tx.Exec(
        "SELECT 1 FROM `PlayerChatBlocks` WHERE `OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2",
        bf::infrastructure::mysql::Parameters{tx, owner.Value(), id});
    if (blocked && present.empty()) {
      const auto count =
          tx.Exec(
                "SELECT count(*) FROM `PlayerChatBlocks` WHERE `OwnerPlayerId`=$1",
                bf::infrastructure::mysql::Parameters{tx, owner.Value()})[0][0]
              .As<unsigned>();
      if (count >= 50U) return Invalid("聊天屏蔽名单已满（最多50人）");
      tx.Exec(
          "INSERT INTO `PlayerChatBlocks`(`OwnerPlayerId`,`BlockedPlayerId`) VALUES($1,$2)",
          bf::infrastructure::mysql::Parameters{tx, owner.Value(), id});
    } else if (!blocked) {
      tx.Exec(
          "DELETE FROM `PlayerChatBlocks` WHERE `OwnerPlayerId`=$1 AND `BlockedPlayerId`=$2",
          bf::infrastructure::mysql::Parameters{tx, owner.Value(), id});
    }
    application::ChatBlockEntry result{static_cast<std::uint32_t>(id),
                                       rows[0][1].As<std::string>()};
    tx.Commit();
    return result;
  } catch (const std::exception&) {
    return Unavailable();
  }
}
}  // namespace bf::infrastructure::mysql
