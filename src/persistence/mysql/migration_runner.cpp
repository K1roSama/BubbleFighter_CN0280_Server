#include "persistence/mysql/migration_runner.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
constexpr std::string_view kMigrationLock =
    "CONCAT('bf.r42.migration.', LEFT(SHA2(DATABASE(),256),40))";
constexpr std::size_t kMaxScriptSize = 128U * 1024U * 1024U;

struct Step {
  std::string kind;
  std::string name;
  std::string statement;
  std::string expected;
};

struct Migration {
  std::uint64_t version;
  std::vector<Step> steps;
  std::string seed;
  std::string digest;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("Missing migration input: " +
                             path.filename().string());
  }
  const auto end = stream.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxScriptSize) {
    throw std::runtime_error("Migration input exceeds the size limit");
  }
  std::string content(static_cast<std::size_t>(end), '\0');
  stream.seekg(0);
  stream.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) {
    throw std::runtime_error("Cannot read migration input");
  }
  return content;
}

std::string Sha256(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
  if (!SHA256(reinterpret_cast<const unsigned char*>(value.data()),
              value.size(), hash.data())) {
    throw std::runtime_error("Unable to calculate migration digest");
  }
  constexpr char k_hex[] = "0123456789abcdef";
  std::string digest;
  for (auto byte : hash) {
    digest.push_back(k_hex[byte >> 4U]);
    digest.push_back(k_hex[byte & 15U]);
  }
  return digest;
}

std::string_view TrimStatement(std::string_view value) {
  const auto first = value.find_first_not_of(" \n\r\t");
  const auto end = value.find_last_not_of(" \n\r\t;");
  if (first == std::string_view::npos || end == std::string_view::npos) {
    throw std::runtime_error("Empty migration step");
  }
  return value.substr(first, end - first + 1);
}

std::vector<Step> ParseSteps(const std::string& schema,
                             const nlohmann::json& contract) {
  const std::regex marker(
      R"((?:^|\n)-- bf-step (table|view|function|trigger) ([A-Za-z0-9_]+)\r?\n)");
  std::vector<Step> steps;
  for (auto it = std::sregex_iterator(schema.begin(), schema.end(), marker);
       it != std::sregex_iterator{}; ++it) {
    const auto index = steps.size();
    const auto next = std::next(it);
    const auto first = static_cast<std::size_t>(it->position() + it->length());
    const auto end = next == std::sregex_iterator{}
                         ? schema.size()
                         : static_cast<std::size_t>(next->position());
    const auto& spec = contract.at(index);
    Step step{it->str(1), it->str(2),
              std::string(TrimStatement(
                  std::string_view(schema).substr(first, end - first))),
              spec.at("sha256").get<std::string>()};
    const bool removal =
        step.expected.empty() &&
        (step.kind == "table" || step.kind == "function") &&
        step.statement == "DROP " +
                              std::string(step.kind == "table" ? "TABLE" : "FUNCTION") +
                              " IF EXISTS `" + step.name + "`";
    if (step.kind != spec.at("kind").get<std::string>() ||
        step.name != spec.at("name").get<std::string>() ||
        (step.expected.size() != 64 && !removal)) {
      throw std::runtime_error("Migration step and schema contract differ");
    }
    steps.push_back(std::move(step));
  }
  if (steps.empty() || steps.size() != contract.size()) {
    throw std::runtime_error("Incomplete MySQL schema contract");
  }
  return steps;
}

std::string ObjectDigest(Connection& connection, const Step& step) {
  try {
    const char tick = 96;
    const auto rows = connection.Execute("SHOW CREATE " + step.kind + " " +
                                         tick + step.name + tick);
    auto definition =
        rows.OneRow()[step.kind == "table" || step.kind == "view" ? 1U : 2U]
            .As<std::string>();
    const std::regex definer("DEFINER=" + std::string(1, tick) + "[^" + tick +
                             "]*" + tick + "@" + tick + "[^" + tick + "]*" +
                             tick + " ");
    definition = std::regex_replace(definition, definer, "");
    definition = std::regex_replace(definition,
                                    std::regex(" AUTO_INCREMENT=[0-9]+"), "");
    if (step.kind == "table") {
      // A restored column may spell out the character set implied by its
      // collation, depending on the session used to create the table.
      const std::regex explicit_charset(
          "\n(  " + std::string(1, tick) + "[A-Za-z0-9_]+" + tick +
          " [A-Za-z]+(?:\\([0-9]+\\))?)"
          " CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin");
      definition = std::regex_replace(definition, explicit_charset,
                                      "\n$1 COLLATE utf8mb4_0900_bin");
    }
    const auto database = tick + connection.Dbname() + tick + ".";
    for (std::size_t offset = 0;
         (offset = definition.find(database, offset)) != std::string::npos;) {
      definition.erase(offset, database.size());
    }
    definition.erase(std::remove(definition.begin(), definition.end(), '\r'),
                     definition.end());
    return Sha256(definition);
  } catch (const SqlError& error) {
    if (error.Code() == 1146U || error.Code() == 1305U ||
        error.Code() == 1360U) {
      return {};
    }
    throw;
  }
}

void CreateLedger(Connection& connection) {
  connection.ExecuteDdl(
      "CREATE TABLE IF NOT EXISTS SchemaMigrations ("
      "MigrationVersion BIGINT UNSIGNED NOT NULL PRIMARY KEY, "
      "ScriptDigest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
      "AppliedAt DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)) ENGINE=InnoDB");
  connection.ExecuteDdl(
      "CREATE TABLE IF NOT EXISTS SchemaMigrationSteps ("
      "StepNumber INT UNSIGNED NOT NULL PRIMARY KEY, "
      "StepDigest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
      "IsApplied BOOLEAN NOT NULL, "
      "CONSTRAINT CK_SchemaMigrationSteps_Applied CHECK (IsApplied IN (0,1))) ENGINE=InnoDB");
}

void CheckStep(Connection& connection, Autocommit& session, const Step& step,
               std::uint64_t number, std::string_view previous, bool finished) {
  const auto digest = Sha256(step.statement + "\n" + step.expected);
  const auto records = session.Exec(
      "SELECT StepDigest, IsApplied FROM SchemaMigrationSteps WHERE StepNumber=$1",
      Parameters{session, number});
  if (!records.empty() && records[0][0].As<std::string_view>() != digest) {
    throw std::runtime_error("MySQL migration step digest changed at " +
                             std::to_string(number));
  }
  if (!records.empty() && records[0][1].As<bool>()) {
    return;
  }
  if (finished) {
    throw std::runtime_error(
        "MySQL migration version is marked complete with missing DDL");
  }
  const auto observed = ObjectDigest(connection, step);
  if (records.empty()) {
    if (observed != previous &&
        !(step.expected.empty() && observed.empty())) {
      throw std::runtime_error(
          "Unexpected object before MySQL migration step " +
          std::to_string(number));
    }
    session.Exec(
        "INSERT INTO SchemaMigrationSteps (StepNumber,StepDigest,IsApplied) VALUES ($1,$2,0)",
        Parameters{session, number, digest});
  }
  if (observed == previous && observed != step.expected) {
    connection.ExecuteDdl(step.statement);
  } else if (observed != step.expected) {
    throw std::runtime_error(
        "Unexpected object after interrupted MySQL migration step " +
        std::to_string(number));
  }
  if (ObjectDigest(connection, step) != step.expected) {
    throw std::runtime_error("MySQL schema contract differs after step " +
                             std::to_string(number));
  }
  session.Exec(
      "UPDATE SchemaMigrationSteps SET IsApplied=1 WHERE StepNumber=$1",
      Parameters{session, number});
}

void ApplySeed(Connection& connection, std::string_view seed) {
  constexpr std::string_view k_separator = "-- bf-seed\n";
  auto offset = seed.find(k_separator);
  if (offset == std::string_view::npos) {
    throw std::runtime_error("MySQL seed has no statements");
  }
  while (offset != std::string_view::npos) {
    const auto start = offset + k_separator.size();
    offset = seed.find(k_separator, start);
    connection.ExecuteDdl(TrimStatement(seed.substr(start, offset - start)));
  }
}

using ObjectSteps =
    std::map<std::pair<std::string, std::string>, const Step*>;

void VerifyObjects(Connection& connection, const ObjectSteps& latest) {
  for (const auto& [key, step] : latest) {
    if (ObjectDigest(connection, *step) != step->expected) {
      throw std::runtime_error("MySQL schema drift in " + key.first + " " +
                               key.second);
    }
  }
}
}  // namespace

MigrationReport RunMigrations(const ConnectionSettings& settings,
                              const std::filesystem::path& directory) {
  constexpr std::array<std::string_view, 2U> scripts{
      "0001_schema", "0002_remove_retired_services"};
  std::vector<Migration> migrations;
  std::size_t step_count = 0;
  for (const auto script : scripts) {
    const auto schema = ReadFile(directory / (std::string(script) + ".sql"));
    const auto contract_bytes =
        ReadFile(directory / (std::string(script) + "_contract.json"));
    auto seed = migrations.empty() ? ReadFile(directory / "0001_seed.sql")
                                    : std::string{};
    auto steps = ParseSteps(schema, nlohmann::json::parse(contract_bytes));
    step_count += steps.size();
    const auto digest =
        Sha256(Sha256(schema) + Sha256(seed) + Sha256(contract_bytes));
    migrations.push_back({static_cast<std::uint64_t>(migrations.size() + 1U),
                          std::move(steps), std::move(seed), digest});
  }
  if (migrations.size() != kSchemaVersion) {
    throw std::runtime_error("MySQL migration inputs do not match this server");
  }
  Connection connection(settings);
  Autocommit session(connection);
  const auto lock =
      session.Exec("SELECT GET_LOCK(" + std::string(kMigrationLock) + ",5)");
  if (lock[0][0].IsNull() || !lock[0][0].As<bool>()) {
    throw std::runtime_error("Cannot obtain MySQL migration lock");
  }
  const auto world = session.Exec(
      "SELECT GET_LOCK(CONCAT('bf.r42.world.',LEFT(SHA2(DATABASE(),256),40)),0)");
  if (world[0][0].IsNull() || !world[0][0].As<bool>()) {
    throw std::runtime_error("A game server currently owns this database");
  }
  CreateLedger(connection);
  const auto history = session.Exec(
      "SELECT MigrationVersion, ScriptDigest FROM SchemaMigrations ORDER BY MigrationVersion");
  if (history.size() > migrations.size()) {
    throw std::runtime_error("MySQL schema version is newer than this server");
  }
  for (std::size_t index = 0; index < history.size(); ++index) {
    if (history[index][0].As<std::uint64_t>() != migrations[index].version ||
        history[index][1].As<std::string_view>() != migrations[index].digest) {
      throw std::runtime_error(
          "MySQL schema version or migration digest mismatch");
    }
  }
  const auto previous_version =
      history.empty() ? 0U : history[history.size() - 1U][0].As<std::uint64_t>();
  const auto step_max =
      session
          .Exec("SELECT COALESCE(MAX(StepNumber),0) FROM SchemaMigrationSteps")
          .OneField()
          .As<std::uint64_t>();
  if (step_max > step_count) {
    throw std::runtime_error("MySQL migration steps are newer than the input");
  }
  ObjectSteps latest;
  std::uint64_t number = 0;
  std::size_t applied_count = 0;
  for (const auto& migration : migrations) {
    const bool installed = migration.version <= previous_version;
    for (const auto& step : migration.steps) {
      const auto key = std::pair{step.kind, step.name};
      const auto found = latest.find(key);
      if (step.expected.empty() && found == latest.end()) {
        throw std::runtime_error("Cannot remove an unknown MySQL schema object");
      }
      const auto previous = found == latest.end() ? std::string_view{}
                                                  : found->second->expected;
      CheckStep(connection, session, step, ++number, previous, installed);
      latest[key] = &step;
    }
    if (!installed) {
      VerifyObjects(connection, latest);
      WriteTransaction transaction(connection);
      if (!migration.seed.empty()) {
        ApplySeed(connection, migration.seed);
      }
      transaction.Exec(
          "INSERT INTO SchemaMigrations (MigrationVersion,ScriptDigest) VALUES ($1,$2)",
          Parameters{transaction, migration.version, migration.digest});
      transaction.Commit();
      ++applied_count;
    }
  }
  VerifyObjects(connection, latest);
  return {.previous_version = previous_version,
          .current_version = kSchemaVersion,
          .applied_count = applied_count};
}

}  // namespace bf::infrastructure::mysql
