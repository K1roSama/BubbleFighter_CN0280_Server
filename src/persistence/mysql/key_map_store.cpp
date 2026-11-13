#include "persistence/mysql/key_map_store.h"

#include <optional>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
application::ServiceResult<std::vector<application::KeyBinding>>
MySqlKeyMapStore::Load(domain::PlayerId player) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto rows = transaction.Exec(
        "SELECT `ActionCode`,`KeyCode` FROM `PlayerKeyBindings` WHERE `PlayerId`=$1 ORDER BY "
        "`ActionCode`",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    std::vector<application::KeyBinding> result;
    for (const auto& row : rows)
      result.push_back(
          {.action = row[0].As<std::string>(),
           .key = row[1].IsNull() ? 0U : row[1].As<std::uint32_t>()});
    if (!application::ValidKeyMap({.bindings = result})) {
      return application::ServiceError{
          .code = application::ErrorCode::kInternal,
          .safe_message = "按键配置包含无效数据"};
    }
    transaction.Commit();
    return result;
  } catch (const std::exception&) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "读取按键设置失败"};
  }
}

application::ServiceResult<bool> MySqlKeyMapStore::Save(
    domain::PlayerId player, const application::KeyMapSettings& settings) {
  if (!player.IsValid() || !application::ValidKeyMap(settings)) {
    return application::ServiceError{
        .code = application::ErrorCode::kInvalidInput,
        .safe_message = "按键设置无效"};
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto owner = transaction.Exec(
        "SELECT 1 FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    if (owner.empty())
      return application::ServiceError{
          .code = application::ErrorCode::kInvalidState,
          .safe_message = "玩家尚未完成建角"};
    transaction.Exec(
        "DELETE FROM `PlayerKeyBindings` WHERE `PlayerId`=$1",
        bf::infrastructure::mysql::Parameters{transaction, player.Value()});
    for (const auto& binding : settings.bindings) {
      transaction.Exec(
          "INSERT INTO `PlayerKeyBindings`(`PlayerId`,`ActionCode`,`KeyCode`) VALUES($1,$2,$3)",
          bf::infrastructure::mysql::Parameters{
              transaction, player.Value(), binding.action,
              binding.key == 0U ? std::optional<std::uint32_t>{}
                                : std::optional<std::uint32_t>{binding.key}});
    }
    transaction.Commit();
    return true;
  } catch (const std::exception&) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "保存按键设置失败"};
  }
}
}  // namespace bf::infrastructure::mysql
