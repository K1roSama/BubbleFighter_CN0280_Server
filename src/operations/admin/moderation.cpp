#include "operations/admin/moderation.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <variant>

#include "application/utf8.h"
#include "operations/utf8_path.h"
#include "persistence/mysql/connection_pool.h"
#include "persistence/mysql/sql_connection.h"

namespace {
bool ValidText(std::string_view text, std::size_t maximum) {
  const auto size = bf::domain::CountUtf8CodePoints(text);
  return size && *size > 0 && *size <= maximum &&
         std::ranges::none_of(
             text, [](unsigned char c) { return c < 32 || c == 127; });
}

int Invalid(const char* message) {
  std::cerr << message << '\n';
  return 2;
}
}  // namespace

bool IsModerationCommand(const char* command) {
  const std::string_view value{command};
  return value == "inspect-account" || value == "mute" || value == "unmute" ||
         value == "ban-account" || value == "unban-account";
}

void PrintModerationUsage() {
  std::cerr
      << "\nModeration commands (database credentials only from config/admin.conf):\n"
         "  inspect-account --account <login> | --account-id <id>\n"
         "  mute | ban-account --account <login> --minutes <0=permanent> --actor <operator> --reason <text>\n"
         "  unmute | unban-account --account <login> --actor <operator> --reason <text>\n"
         "  Every command accepts --config <database.conf>.\n";
}

int RunModerationCommand(int argc, char** argv) {
  const std::string action{argv[1]};
  const bool inspect = action.starts_with("inspect-");
  const bool revoke = action.starts_with("un");
  std::map<std::string, std::string> args;
  const std::set<std::string> allowed{"--account", "--account-id", "--minutes",
                                      "--actor",   "--reason",     "--config"};
  for (int i = 2; i < argc; ++i) {
    const std::string key{argv[i]};
    if (!allowed.contains(key) || i + 1 == argc || args.contains(key))
      return Invalid("Unknown, duplicate or incomplete argument.");
    args.emplace(key, argv[++i]);
  }
  if (args.contains("--account") == args.contains("--account-id")) {
    return Invalid("Provide exactly one --account or --account-id.");
  }
  std::uint64_t account_id = 0, minutes = 0;
  const auto parse_uint = [](const std::string& text, std::uint64_t& result) {
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    return !text.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
  };
  if (args.contains("--account-id") &&
      (!parse_uint(args["--account-id"], account_id) || account_id == 0 ||
       account_id > 9223372036854775807ULL))
    return Invalid("Invalid account id.");
  if (args.contains("--account") && !ValidText(args["--account"], 64))
    return Invalid("Invalid account name.");
  if (inspect && (args.contains("--minutes") || args.contains("--reason") ||
                  args.contains("--actor")))
    return Invalid("Inspection does not accept mutation parameters.");
  if (!inspect &&
      (!ValidText(args["--actor"], 64) || !ValidText(args["--reason"], 256)))
    return Invalid(
        "An operator and a reason are required (maximum 64 and 256 characters).");
  if (!inspect && !revoke &&
      (!args.contains("--minutes") || !parse_uint(args["--minutes"], minutes) ||
       minutes > 5256000))
    return Invalid(
        "--minutes is required: 0 for permanent, otherwise 1..5256000.");
  if (revoke && args.contains("--minutes"))
    return Invalid("Revocation does not accept --minutes.");
  const auto config_path =
      args.contains("--config") ? args["--config"] : "config/admin.conf";
  const auto settings = bf::infrastructure::mysql::LoadConnectionSettings(
      bf::infrastructure::Utf8Path(config_path));
  auto pool_result = bf::infrastructure::mysql::MySqlConnectionPool::Create(
      {.connection = settings,
       .size = 1U,
       .acquire_timeout = std::chrono::seconds{2},
       .query_timeout_ms = 30000U});
  auto* pool = std::get_if<
      std::unique_ptr<bf::infrastructure::mysql::MySqlConnectionPool>>(
      &pool_result);
  if (!pool || !*pool)
    return Invalid("Cannot open administrator database connection.");
  auto acquired = (*pool)->Acquire();
  auto* lease =
      std::get_if<bf::infrastructure::mysql::MySqlConnectionPool::Lease>(
          &acquired);
  if (!lease)
    return Invalid("Cannot acquire administrator database connection.");
  bf::infrastructure::mysql::WriteTransaction tx{lease->Connection()};
  if (!inspect)
    tx.Exec(
        "SELECT LockKey FROM TransactionLocks WHERE LockKey='moderation' FOR UPDATE");
  nlohmann::json report{{"action", action}};
  const auto rows = tx.Exec(
      "SELECT `AccountId`,`LoginName`,`AccountStatus` FROM `Accounts` WHERE "
      "($1>0 AND `AccountId`=$1) OR ($1=0 AND `LoginKey`=lower($2))",
      bf::infrastructure::mysql::Parameters{tx, account_id, args["--account"]});
  if (rows.size() != 1) return Invalid("Account was not found.");
  account_id = rows[0][0].As<std::uint64_t>();
  report["account_id"] = account_id;
  report["account"] = rows[0][1].As<std::string>();
  report["status"] = rows[0][2].As<std::string>();
  const std::string kind =
      action == "mute" || action == "unmute" ? "mute" : "account_ban";
  if (!inspect) {
    const auto revoked = tx.Exec(
        "UPDATE `ModerationSanctions` SET "
        "`RevokedAt`=CURRENT_TIMESTAMP(6),`RevokedBy`=$3,`RevocationReason`=$4 WHERE `Kind`=$1 AND "
        "`AccountId`=$2 AND `RevokedAt` IS NULL",
        bf::infrastructure::mysql::Parameters{
            tx, kind, account_id, args["--actor"], args["--reason"]});
    report["revoked_records"] = revoked.AffectedRows();
    if (!revoke) {
      const auto created = tx.Exec(
          "INSERT INTO "
          "`ModerationSanctions`(`Kind`,`AccountId`,`ExpiresAt`,`Actor`,`Reason`) "
          "VALUES($1,$2,CASE WHEN $3=0 THEN NULL ELSE "
          "CURRENT_TIMESTAMP(6)+INTERVAL $3 MINUTE END,$4,$5) RETURNING `SanctionId`,`ExpiresAt`",
          bf::infrastructure::mysql::Parameters{tx, kind, account_id, minutes,
                                                args["--actor"],
                                                args["--reason"]});
      report["sanction_id"] = created[0][0].As<std::uint64_t>();
      report["expires_at"] =
          created[0][1].IsNull()
              ? nlohmann::json{}
              : nlohmann::json(created[0][1].As<std::string>());
      if (kind != "mute") {
        tx.Exec("DELETE FROM `LoginTickets` WHERE `AccountId`=$1",
                bf::infrastructure::mysql::Parameters{tx, account_id});
      } else {
        tx.Exec(
            "INSERT INTO `ChatPenaltyHistory`(`PlayerId`,`Reason`,`RecordedBy`) SELECT `PlayerId`,$2,$3"
            " FROM `PlayerProfiles` WHERE `AccountId`=$1",
            bf::infrastructure::mysql::Parameters{
                tx, account_id, args["--reason"], args["--actor"]});
      }
    }
    tx.Exec(
        "INSERT INTO "
        "`ModerationAudit`(`Action`,`AccountId`,`Actor`,`Reason`,`Details`) "
        "VALUES($1,$2,$3,$4,$5)",
        bf::infrastructure::mysql::Parameters{tx, action, account_id,
                                              args["--actor"], args["--reason"],
                                              report.dump()});
  } else {
    const auto sanctions = tx.Exec(
        "SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('sanction_id',s.SanctionId, 'kind',s.Kind, "
        "'account_id',s.AccountId, 'starts_at',s.StartsAt, "
        "'expires_at',s.ExpiresAt, 'revoked_at',s.RevokedAt, 'actor',s.Actor, "
        "'reason',s.Reason,'active',IF(s.active,JSON_EXTRACT('true','$'),JSON_EXTRACT('false','$')))),'[]')"
        " FROM (SELECT *,`RevokedAt` IS NULL AND `StartsAt`<=CURRENT_TIMESTAMP(6) AND (`ExpiresAt` "
        "IS NULL OR `ExpiresAt`>CURRENT_TIMESTAMP(6)) AS active FROM `ModerationSanctions` WHERE "
        "`AccountId`=$1 AND `Kind` IN ('account_ban','mute') ORDER BY `SanctionId` "
        "DESC LIMIT 1000) s",
        bf::infrastructure::mysql::Parameters{tx, account_id});
    report["sanctions"] =
        nlohmann::json::parse(sanctions[0][0].As<std::string>());
  }
  tx.Commit();
  std::cout << report.dump(2) << '\n';
  return 0;
}
