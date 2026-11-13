#include "features/battle/durable_match_settlement_flow.h"

#include <set>
#include <string>
#include <utility>
#include <variant>

namespace bf::runtime {
namespace {

constexpr std::size_t kMaximumRecoveryBatch = 4'096U;

application::ServiceError ContractError(std::string detail) {
  return application::ServiceError{
      .code = application::ErrorCode::kInternal,
      .safe_message = std::move(detail),
      .correlation_id = {},
      .is_retryable = false,
  };
}

application::ServiceError InvalidLimitError() {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = "结算恢复批次上限必须为 1～4096",
      .correlation_id = {},
      .is_retryable = false,
  };
}

bool ValidStageState(application::MatchSettlementStageState state) noexcept {
  switch (state) {
    case application::MatchSettlementStageState::kCreated:
    case application::MatchSettlementStageState::kExistingIdentical:
      return true;
  }
  return false;
}

}  // namespace

DurableMatchSettlementFlow::DurableMatchSettlementFlow(
    application::MatchSettlementJournal& journal,
    MatchSettlementSubmissionSink& dispatcher,
    application::EventLogger& logger) noexcept
    : journal_(journal), dispatcher_(dispatcher), logger_(logger) {}

application::ServiceResult<DurableMatchSettlementSubmission>
DurableMatchSettlementFlow::Submit(
    const application::MatchSettlementCommand& command) {
  if (const auto error = application::ValidateMatchSettlementCommand(command)) {
    return *error;
  }
  auto stage_result = journal_.Stage(command);
  if (const auto* error =
          std::get_if<application::ServiceError>(&stage_result)) {
    return *error;
  }
  auto stage = std::get<application::MatchSettlementStageReceipt>(
      std::move(stage_result));
  if (stage.match_id != command.match_id || !ValidStageState(stage.state)) {
    return ContractError("结算耐久日志返回了不匹配的保存回执");
  }

  if (auto scheduling_error = dispatcher_.Submit(command)) {
    logger_.Log(application::LogLevel::kWarning,
                "durable match settlement remains pending for recovery");
    return DurableMatchSettlementSubmission{
        .stage = stage,
        .disposition = DurableMatchSettlementDisposition::kPersistedForRecovery,
        .scheduling_error = std::move(scheduling_error),
    };
  }
  return DurableMatchSettlementSubmission{
      .stage = stage,
      .disposition = DurableMatchSettlementDisposition::kScheduled,
      .scheduling_error = std::nullopt,
  };
}

application::ServiceResult<MatchSettlementRecoveryReport>
DurableMatchSettlementFlow::Recover(std::size_t limit) {
  if (limit == 0U || limit > kMaximumRecoveryBatch) {
    return InvalidLimitError();
  }
  auto load_result = journal_.LoadRecoverable(limit);
  if (const auto* error =
          std::get_if<application::ServiceError>(&load_result)) {
    return *error;
  }
  auto commands = std::get<std::vector<application::MatchSettlementCommand>>(
      std::move(load_result));
  if (commands.size() > limit) {
    return ContractError("结算耐久日志返回数量超过请求上限");
  }

  std::set<domain::MatchId::ByteArray> match_ids;
  for (const auto& command : commands) {
    if (application::ValidateMatchSettlementCommand(command).has_value()) {
      return ContractError("结算耐久日志返回非法终局快照");
    }
    if (!match_ids.insert(command.match_id.Bytes()).second) {
      return ContractError("结算耐久日志在同一批次返回重复 match_id");
    }
  }

  MatchSettlementRecoveryReport report{.loaded = commands.size()};
  for (std::size_t index = 0U; index < commands.size(); ++index) {
    auto error = dispatcher_.Submit(commands[index]);
    if (error.has_value()) {
      report.deferred = commands.size() - index;
      report.scheduling_error = std::move(error);
      logger_.Log(application::LogLevel::kWarning,
                  "match settlement recovery paused at dispatcher boundary");
      break;
    }
    ++report.scheduled;
  }
  return report;
}

}  // namespace bf::runtime
