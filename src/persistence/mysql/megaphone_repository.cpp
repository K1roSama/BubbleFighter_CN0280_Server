#include <spdlog/spdlog.h>

#include <string>
#include <utility>

#include "features/chat/megaphone_event.h"
#include "features/chat/user_message.h"
#include "persistence/mysql/item_use_repository.h"
#include "persistence/mysql/sql_connection.h"
#include "protocol/codec/text_codec.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError Failed(application::ErrorCode code,
                                 std::string message) {
  return {.code = code, .safe_message = std::move(message)};
}

constexpr auto kColumns =
    "`BusinessId`,`PlayerId`,`ChannelId`,`RequestType`,`SelectionId`,`ItemId`,"
    "`SenderName`,`MessageText`,`PublishedAt` IS NOT NULL";

application::MegaphoneReceipt Receipt(
    const bf::infrastructure::mysql::Row& row) {
  const auto item = row[5].As<std::uint32_t>();
  const auto definition = domain::GetMegaphoneDefinition(item).value();
  return {.business_id = row[0].As<std::string>(),
          .player_id = domain::PlayerId{row[1].As<std::uint64_t>()},
          .channel = row[2].As<std::uint32_t>(),
          .type = definition.type,
          .item_id = item,
          .color = definition.color,
          .sender_name = protocol::Utf8ToUtf16(row[6].As<std::string>()),
          .text = protocol::Utf8ToUtf16(row[7].As<std::string>()),
          .published = row[8].As<bool>()};
}
}  // namespace

application::ServiceResult<application::MegaphoneReceipt>
MySqlItemUseRepository::Megaphone(const application::MegaphoneCommand& command,
                                  std::chrono::system_clock::time_point now) {
  if (!command.player_id.IsValid() ||
      (command.channel != 60U && command.channel != 80U) ||
      !domain::ValidMegaphoneSelection(command.type, command.selection) ||
      !std::holds_alternative<domain::UserMessage>(
          domain::UserMessage::Parse(command.text))) {
    return Failed(application::ErrorCode::kInvalidInput, "喇叭请求无效");
  }
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    if (tx.Exec("SELECT `PlayerChatDenied`($1)",
                bf::infrastructure::mysql::Parameters{
                    tx, command.player_id.Value()})[0][0]
            .As<bool>())
      return Failed(application::ErrorCode::kPermissionDenied,
                    "账号处于禁言或封禁期间，暂时无法使用喇叭");
    const auto player = command.player_id.Value();
    const auto operation = command.operation_id.ToUuidString();
    const auto text = protocol::Utf16ToUtf8(command.text);
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count();
    const auto profile = tx.Exec(
        "SELECT `Nickname` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{tx, player});
    if (profile.empty())
      return Failed(application::ErrorCode::kPermissionDenied, "角色尚未创建");
    const auto previous =
        tx.Exec(std::string{"SELECT "} + kColumns +
                    " FROM `MegaphoneMessages` WHERE `BusinessId`=$1",
                bf::infrastructure::mysql::Parameters{tx, operation});
    if (!previous.empty()) {
      const auto& row = previous[0];
      if (row[1].As<std::uint64_t>() != player ||
          row[2].As<std::uint32_t>() != command.channel ||
          row[3].As<std::uint32_t>() != command.type ||
          row[4].As<std::uint32_t>() != command.selection ||
          row[7].As<std::string>() != text) {
        return Failed(application::ErrorCode::kConflict,
                      "喇叭操作标识已被不同请求使用");
      }
      auto result = Receipt(row);
      tx.Commit();
      return result;
    }
    // C2S520
    // 无请求序号；十秒相同正文去重是本地策略，不能宣称原版具备端到端幂等键。
    const auto duplicate = tx.Exec(
        std::string{"SELECT "} + kColumns +
            " FROM `MegaphoneMessages` WHERE `PlayerId`=$1 AND `ChannelId`=$2 AND `RequestType`=$3 AND "
            "`SelectionId`=$4 AND `MessageText`=$5 AND "
            "`ConsumedAt`>FROM_UNIXTIME($6/1000000.0)-INTERVAL 10 SECOND ORDER BY `ConsumedAt` DESC "
            "LIMIT 1",
        bf::infrastructure::mysql::Parameters{tx, player, command.channel,
                                              command.type, command.selection,
                                              text, now_us});
    if (!duplicate.empty()) {
      auto result = Receipt(duplicate[0]);
      tx.Commit();
      return result;
    }
    const auto count = tx.Exec(
        "SELECT count(*) FROM `MegaphoneMessages` WHERE `PlayerId`=$1 AND "
        "`ConsumedAt`>FROM_UNIXTIME($2/1000000.0)-INTERVAL 2 SECOND",
        bf::infrastructure::mysql::Parameters{tx, player, now_us});
    if (count[0][0].As<std::uint32_t>() >= 5U)
      return Failed(application::ErrorCode::kConflict,
                    "喇叭发送过快，请稍后再试");
    const auto inventory = tx.Exec(
        "SELECT owned.`ItemInstanceId`,owned.`ItemId`,owned.`Quantity` FROM `PlayerItems` owned "
        "JOIN `ItemTemplates` `Kind` ON `Kind`.`ItemId`=owned.`ItemId` JOIN `ConfigurationVersions`"
        " version ON version.`ConfigurationVersionId`=`Kind`.`ConfigurationVersionId` WHERE "
        "owned.`PlayerId`=$1 AND owned.`Quantity`>0 AND `Kind`.`IsGrantable` AND "
        "JSON_UNQUOTE(JSON_EXTRACT(`Kind`.`ClientFields`,'$.use_type'))='4' AND "
        "version.`ReleaseStatus`='published' AND (owned.`ExpiresAt` IS NULL OR "
        "owned.`ExpiresAt`>FROM_UNIXTIME($3/1000000.0)) AND ((owned.`ItemId`=$2 AND $2>20000) OR "
        "($2=20000 AND owned.`ItemId`=100251) OR ($2=10000 AND owned.`ItemId` IN "
        "(100252,100547,100548,102173,190071,190074,190105))) ORDER BY (owned.`ItemId` IN "
        "(100251,100252)) DESC,(owned.`ExpiresAt` IS NULL), "
        "owned.`ExpiresAt`,owned.`ItemInstanceId` LIMIT 1 FOR UPDATE OF owned",
        bf::infrastructure::mysql::Parameters{tx, player, command.selection,
                                              now_us});
    if (inventory.empty())
      return Failed(application::ErrorCode::kConflict,
                    "没有可用的对应喇叭道具");
    const auto instance = inventory[0][0].As<std::uint64_t>();
    const auto item = inventory[0][1].As<std::uint32_t>();
    if (inventory[0][2].As<std::uint32_t>() == 1U) {
      tx.Exec("DELETE FROM `PlayerEquipment` WHERE `ItemInstanceId`=$1",
              bf::infrastructure::mysql::Parameters{tx, instance});
      tx.Exec("DELETE FROM `PlayerItems` WHERE `ItemInstanceId`=$1",
              bf::infrastructure::mysql::Parameters{tx, instance});
    } else {
      tx.Exec(
          "UPDATE `PlayerItems` SET `Quantity`=`Quantity`-1 WHERE `ItemInstanceId`=$1",
          bf::infrastructure::mysql::Parameters{tx, instance});
    }
    const auto inserted = tx.Exec(
        "INSERT INTO "
        "`MegaphoneMessages`(`BusinessId`,`PlayerId`,`ChannelId`,`RequestType`,`SelectionId`,`ItemId`,`SenderName`,`MessageText`,`ConsumedAt`)"
        " VALUES($1,$2,$3,$4,$5,$6,$7,$8,FROM_UNIXTIME($9/1000000.0)) RETURNING " +
            std::string{kColumns},
        bf::infrastructure::mysql::Parameters{
            tx, operation, player, command.channel, command.type,
            command.selection, item, profile[0][0].As<std::string>(), text,
            now_us});
    tx.Exec(
        "INSERT INTO "
        "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ReasonCode`,`BusinessId`)"
        " VALUES($1,$2,NULL,-1,'megaphone',$3)",
        bf::infrastructure::mysql::Parameters{tx, player, item, operation});
    auto result = Receipt(inserted[0]);
    tx.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    spdlog::error("喇叭事务SQL失败，sqlstate={}", error.Sqlstate());
    return Failed(application::ErrorCode::kStorageUnavailable,
                  "喇叭事务未能确认，请稍后重试");
  } catch (const std::exception&) {
    return Failed(application::ErrorCode::kStorageUnavailable,
                  "喇叭数据库暂不可用");
  }
}

application::ServiceResult<std::vector<application::MegaphoneReceipt>>
MySqlItemUseRepository::PendingMegaphones() {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::ReadTransaction tx{lease.Connection()};
    const auto rows = tx.Exec(
        std::string{"SELECT "} + kColumns +
        " FROM `MegaphoneMessages` WHERE `PublishedAt` IS NULL ORDER BY `ConsumedAt`,`BusinessId` "
        "LIMIT 64");
    std::vector<application::MegaphoneReceipt> result;
    for (const auto& row : rows) result.push_back(Receipt(row));
    return result;
  } catch (const std::exception&) {
    return Failed(application::ErrorCode::kStorageUnavailable,
                  "喇叭待投递记录暂不可用");
  }
}

std::optional<application::ServiceError>
MySqlItemUseRepository::AcknowledgeMegaphone(std::string_view id) {
  auto acquired = pool_.Acquire();
  if (const auto* error = std::get_if<application::ServiceError>(&acquired))
    return *error;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto result = tx.Exec(
        "UPDATE `MegaphoneMessages` SET `PublishedAt`=COALESCE(`PublishedAt`,CURRENT_TIMESTAMP(6)) "
        "WHERE `BusinessId`=$1 RETURNING `BusinessId`",
        bf::infrastructure::mysql::Parameters{tx, id});
    if (result.empty())
      return Failed(application::ErrorCode::kConflict, "喇叭消费记录不存在");
    tx.Commit();
    return std::nullopt;
  } catch (const std::exception&) {
    return Failed(application::ErrorCode::kStorageUnavailable,
                  "喇叭投递回执暂不可用");
  }
}
}  // namespace bf::infrastructure::mysql
