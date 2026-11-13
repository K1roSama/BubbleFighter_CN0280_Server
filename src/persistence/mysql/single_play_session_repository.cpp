#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <string_view>

#include "persistence/mysql/sql_connection.h"
#include "persistence/mysql/tutorial_session_repository.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError SingleError(application::ErrorCode code,
                                      std::string text, bool retry = false) {
  return {.code = code,
          .safe_message = std::move(text),
          .correlation_id = {},
          .is_retryable = retry};
}

bool HashValid(std::string_view text) {
  return text.size() == 64U && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
}  // namespace

application::ServiceResult<bool>
MySqlTutorialSessionRepository::BeginSinglePlay(
    const application::TutorialSessionStart& c) {
  if (!c.player_id.IsValid() || !HashValid(c.profile_hash) ||
      c.difficulty < 1U || c.difficulty > 3U)
    return SingleError(application::ErrorCode::kInvalidInput,
                       "SinglePlay identity/profile invalid");
  bool commit_requested = false;
  auto acquired = pool_.Acquire();
  if (const auto* e = std::get_if<application::ServiceError>(&acquired))
    return *e;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto owner = tx.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value()});
    if (owner.empty())
      return SingleError(application::ErrorCode::kConflict,
                         "SinglePlay authenticated profile missing");
    const auto mission = tx.Exec(
        "SELECT `StageKey` FROM `SinglePlayMissions` WHERE `MissionKey`=$1 AND `Directory`=$2 AND "
        "`Difficulty`=$3 AND `StageKey`=$4",
        bf::infrastructure::mysql::Parameters{tx, c.mission_key, c.directory,
                                              c.difficulty, c.stage_key});
    if (mission.empty())
      return SingleError(application::ErrorCode::kInvalidInput,
                         "SinglePlay exact mission mapping invalid");
    const auto row = tx.Exec(
        "SELECT `PlayerId`,`ProfileHash`,`MissionKey`,`Directory`,`Difficulty`,`StageKey` FROM "
        "`SinglePlayAttempts` WHERE `AttemptId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.session_id.ToUuidString()});
    if (!row.empty()) {
      const auto r = row[0];
      if (r[0].As<std::uint64_t>() != c.player_id.Value() ||
          r[1].As<std::string>() != c.profile_hash ||
          r[2].As<std::string>() != c.mission_key ||
          r[3].As<std::string>() != c.directory ||
          r[4].As<std::uint32_t>() != c.difficulty ||
          r[5].As<std::uint32_t>() != c.stage_key)
        return SingleError(
            application::ErrorCode::kConflict,
            "SinglePlay UUID cannot migrate owner/mission/profile");
      tx.Commit();
      return false;
    }
    static_cast<void>(tx.Exec(
        "UPDATE `SinglePlayAttempts` SET "
        "`SessionStatus`='interrupted',`TerminalReason`='superseded_session',`FinishedAt`=SYSDATE(6)"
        " WHERE `PlayerId`=$1 AND `SessionStatus`='started'",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value()}));
    static_cast<void>(tx.Exec(
        "INSERT INTO "
        "`SinglePlayAttempts`(`AttemptId`,`PlayerId`,`ProfileHash`,`MissionKey`,`Directory`,`Difficulty`,`StageKey`)"
        " VALUES($1,$2,$3,$4,$5,$6,$7)",
        bf::infrastructure::mysql::Parameters{
            tx, c.session_id.ToUuidString(), c.player_id.Value(),
            c.profile_hash, c.mission_key, c.directory, c.difficulty,
            c.stage_key}));
    commit_requested = true;
    tx.Commit();
    return true;
  } catch (const std::exception& e) {
    spdlog::warn("SinglePlay begin unconfirmed: {}", e.what());
    auto failure =
        SingleError(application::ErrorCode::kStorageUnavailable,
                    "SinglePlay begin transaction unconfirmed", true);
    if (dynamic_cast<const bf::infrastructure::mysql::InDoubtError*>(&e) ||
        (commit_requested &&
         dynamic_cast<const bf::infrastructure::mysql::BrokenConnection*>(&e)))
      failure.correlation_id = "tutorial_begin_uncertain";
    return failure;
  }
}

application::ServiceResult<bool>
MySqlTutorialSessionRepository::FinishSinglePlay(
    const application::TutorialSessionFinish& c) {
  const bool report = c.reason == "client_reported_clear" ||
                      c.reason == "client_reported_failure";
  if (!c.player_id.IsValid() ||
      c.reported_clear != (c.reason == "client_reported_clear") ||
      (report && !c.elapsed_ms) ||
      (!report && c.reason != "disconnect" && c.reason != "client_leave" &&
       c.reason != "loading_timeout" && c.reason != "report_timeout"))
    return SingleError(application::ErrorCode::kInvalidInput,
                       "SinglePlay terminal fields invalid");
  auto acquired = pool_.Acquire();
  if (const auto* e = std::get_if<application::ServiceError>(&acquired))
    return *e;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto owner = tx.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.player_id.Value()});
    if (owner.empty())
      return SingleError(application::ErrorCode::kConflict,
                         "SinglePlay profile missing");
    const auto row = tx.Exec(
        "SELECT `PlayerId`,`SessionStatus`,`TerminalReason`,`ElapsedMs` FROM `SinglePlayAttempts` "
        "WHERE `AttemptId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, c.session_id.ToUuidString()});
    if (row.empty() || row[0][0].As<std::uint64_t>() != c.player_id.Value())
      return SingleError(application::ErrorCode::kConflict,
                         "SinglePlay attempt not owned");
    const std::string status = c.reported_clear ? "reported_clear"
                               : report         ? "reported_failure"
                                                : "interrupted";
    if (row[0][1].As<std::string>() != "started") {
      if (row[0][1].As<std::string>() != status ||
          row[0][2].As<std::string>() != c.reason ||
          row[0][3].As<std::optional<std::uint32_t>>() != c.elapsed_ms)
        return SingleError(
            application::ErrorCode::kConflict,
            "SinglePlay terminal replay changed outcome/reason/time");
      tx.Commit();
      return false;
    }
    static_cast<void>(tx.Exec(
        "UPDATE `SinglePlayAttempts` SET "
        "`SessionStatus`=$2,`TerminalReason`=$3,`ElapsedMs`=$4,`FinishedAt`=SYSDATE(6) WHERE "
        "`AttemptId`=$1 AND `SessionStatus`='started'",
        bf::infrastructure::mysql::Parameters{tx, c.session_id.ToUuidString(),
                                              status, c.reason, c.elapsed_ms}));
    tx.Commit();
    return true;
  } catch (const std::exception& e) {
    spdlog::warn("SinglePlay finish unconfirmed: {}", e.what());
    return SingleError(application::ErrorCode::kStorageUnavailable,
                       "SinglePlay terminal transaction unconfirmed", true);
  }
}
}  // namespace bf::infrastructure::mysql
