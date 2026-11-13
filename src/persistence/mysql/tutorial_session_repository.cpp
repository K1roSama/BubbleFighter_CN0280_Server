#include "persistence/mysql/tutorial_session_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>

#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
namespace {
application::ServiceError Error(application::ErrorCode code, std::string text,
                                bool retry = false) {
  return {.code = code,
          .safe_message = std::move(text),
          .correlation_id = {},
          .is_retryable = retry};
}

bool ValidHash(std::string_view text) {
  return text.size() == 64U && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool ValidReason(std::string_view text) {
  return !text.empty() && text.size() <= 64U && text.front() >= 'a' &&
         text.front() <= 'z' && std::ranges::all_of(text, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}
}  // namespace

MySqlTutorialSessionRepository::MySqlTutorialSessionRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<bool> MySqlTutorialSessionRepository::Begin(
    const application::TutorialSessionStart& command) {
  if (command.mode_id == 15U) return BeginSinglePlay(command);
  if (command.mode_id != 103U || !command.mission_key.empty() ||
      !command.directory.empty() || command.difficulty != 0U ||
      command.stage_key != 0U)
    return Error(application::ErrorCode::kInvalidInput, "教程开始模式字段非法");
  if (!command.player_id.IsValid() || !ValidHash(command.profile_hash))
    return Error(application::ErrorCode::kInvalidInput,
                 "教程开始身份或证据profile摘要非法");
  bool commit_requested = false;
  auto acquired = pool_.Acquire();
  if (const auto* failure = std::get_if<application::ServiceError>(&acquired))
    return *failure;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto owner = tx.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 AND `IsCharacterCreated` FOR "
        "UPDATE",
        bf::infrastructure::mysql::Parameters{tx, command.player_id.Value()});
    if (owner.empty())
      return Error(application::ErrorCode::kConflict, "教程认证玩家资料不存在");
    const auto previous = tx.Exec(
        "SELECT `PlayerId`,`ProfileHash` FROM `HordeTutorSessions` WHERE `TutorialSessionId`=$1 FOR"
        " UPDATE",
        bf::infrastructure::mysql::Parameters{
            tx, command.session_id.ToUuidString()});
    if (!previous.empty()) {
      if (previous[0][0].As<std::uint64_t>() != command.player_id.Value() ||
          previous[0][1].As<std::string>() != command.profile_hash)
        return Error(application::ErrorCode::kConflict,
                     "教程UUID不能换玩家或原生profile");
      tx.Commit();
      return false;
    }
    // Only a newly authenticated107 which has no live world slot invokes begin.
    // After a crashed instance, a new UUID conservatively interrupts dangling
    // records rather than granting a win or blocking the player indefinitely.
    static_cast<void>(tx.Exec(
        "UPDATE `HordeTutorSessions` SET "
        "`SessionStatus`='interrupted',`FinishedAt`=SYSDATE(6),`TerminalReason`='superseded_session'"
        " WHERE `PlayerId`=$1 AND `SessionStatus`='started'",
        bf::infrastructure::mysql::Parameters{tx, command.player_id.Value()}));
    static_cast<void>(tx.Exec(
        "INSERT INTO `HordeTutorSessions`(`TutorialSessionId`,`PlayerId`,`ProfileHash`) "
        "VALUES($1,$2,$3)",
        bf::infrastructure::mysql::Parameters{
            tx, command.session_id.ToUuidString(), command.player_id.Value(),
            command.profile_hash}));
    commit_requested = true;
    tx.Commit();
    return true;
  } catch (const std::exception& failure) {
    spdlog::warn("tutorial begin transaction failed (no scene published): {}",
                 failure.what());
    auto failed = Error(application::ErrorCode::kStorageUnavailable,
                        "教程开始事务未确认", true);
    if (dynamic_cast<const bf::infrastructure::mysql::InDoubtError*>(
            &failure) ||
        (commit_requested &&
         dynamic_cast<const bf::infrastructure::mysql::BrokenConnection*>(
             &failure)))
      failed.correlation_id =
          "tutorial_begin_uncertain";  // Server-generated outcome ambiguity,
                                       // not client input.
    return failed;
  }
}

application::ServiceResult<bool> MySqlTutorialSessionRepository::Finish(
    const application::TutorialSessionFinish& command) {
  if (command.mode_id == 15U) return FinishSinglePlay(command);
  if (command.mode_id != 103U || command.elapsed_ms)
    return Error(application::ErrorCode::kInvalidInput, "教程终态模式字段非法");
  if (!command.player_id.IsValid() || !ValidReason(command.reason) ||
      command.reported_clear != (command.reason == "client_reported_clear"))
    return Error(application::ErrorCode::kInvalidInput,
                 "教程终态身份或报告原因非法");
  auto acquired = pool_.Acquire();
  if (const auto* failure = std::get_if<application::ServiceError>(&acquired))
    return *failure;
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(acquired));
    bf::infrastructure::mysql::WriteTransaction tx{lease.Connection()};
    const auto owner = tx.Exec(
        "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{tx, command.player_id.Value()});
    if (owner.empty())
      return Error(application::ErrorCode::kConflict, "教程认证玩家资料不存在");
    const auto row = tx.Exec(
        "SELECT `PlayerId`,`SessionStatus`,`TerminalReason` FROM `HordeTutorSessions` WHERE "
        "`TutorialSessionId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{
            tx, command.session_id.ToUuidString()});
    if (row.empty() ||
        row[0][0].As<std::uint64_t>() != command.player_id.Value())
      return Error(application::ErrorCode::kConflict,
                   "教程终态UUID不属于认证玩家");
    const std::string status =
        command.reported_clear ? "reported_clear" : "interrupted";
    const auto saved = row[0][1].As<std::string>();
    if (saved != "started") {
      if (saved == status && row[0][2].As<std::string>() == command.reason) {
        tx.Commit();
        return false;
      }
      return Error(application::ErrorCode::kConflict, "教程历史终态不能改写");
    }
    static_cast<void>(tx.Exec(
        "UPDATE `HordeTutorSessions` SET "
        "`SessionStatus`=$2,`TerminalReason`=$3,`FinishedAt`=SYSDATE(6) WHERE "
        "`TutorialSessionId`=$1 AND `SessionStatus`='started'",
        bf::infrastructure::mysql::Parameters{
            tx, command.session_id.ToUuidString(), status, command.reason}));
    tx.Commit();
    return true;
  } catch (const std::exception& failure) {
    spdlog::warn(
        "tutorial terminal transaction failed (no success published): {}",
        failure.what());
    return Error(application::ErrorCode::kStorageUnavailable,
                 "教程终态事务未确认", true);
  }
}
}  // namespace bf::infrastructure::mysql
