#include "persistence/mysql/level_icon_store.h"

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
application::ServiceResult<bool> MySqlLevelIconStore::Save(
    domain::PlayerId player, const application::LevelIconSelection& selection) {
  if (!player.IsValid() || selection.display_type > 3U) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "等级图标选择无效"};
  }
  if (selection.display_type == 1U || selection.display_type == 2U) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidState,
        .safe_message = "天梯和竞技阶级显示尚未开放"};
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto owner = transaction.Exec(
        "SELECT `LevelIconType` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` "
        "FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (owner.empty()) {
      return application::ServiceError{
          .code = application::ErrorCode::kInvalidState,
          .safe_message = "玩家尚未完成建角"};
    }
    if (selection.display_type == 3U) {
      const auto clan = transaction.Exec(
          "SELECT member.`ClanId`,clan.`MarkCode` FROM `ClanMemberships` member JOIN `Clans` clan "
          "USING(`ClanId`) WHERE member.`PlayerId`=$1 FOR SHARE OF member,clan",
          bf::infrastructure::mysql::Parameters{transaction, player.Value()});
      if (clan.empty() || clan[0][0].As<std::uint64_t>() != selection.clan_id ||
          clan[0][1].As<std::uint32_t>() != selection.clan_mark) {
        return application::ServiceError{
            .code = application::ErrorCode::kPermissionDenied,
            .safe_message = "家族信息已变更，请刷新后选择家族徽章"};
      }
    }
    const bool changed =
        owner[0][0].As<std::uint32_t>() != selection.display_type;
    if (changed) {
      transaction.Exec(
          "UPDATE `PlayerProfiles` SET `LevelIconType`=$2 WHERE `PlayerId`=$1",
          bf::infrastructure::mysql::Parameters{transaction, player.Value(),
                                                selection.display_type});
    }
    transaction.Commit();
    return changed;
  } catch (const std::exception&) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "保存等级图标失败"};
  }
}
}  // namespace bf::infrastructure::mysql
