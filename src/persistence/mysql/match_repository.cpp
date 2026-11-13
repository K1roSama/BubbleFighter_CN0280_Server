#include "persistence/mysql/match_repository.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "persistence/mysql/room_lease_repository.h"
#include "persistence/mysql/sql_connection.h"
#include "persistence/mysql/user_host_prize_repository.h"

namespace bf::infrastructure::mysql {
namespace {

using Json = nlohmann::json;

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

application::ServiceError StorageError(
    std::string_view operation,
    const bf::infrastructure::mysql::SqlError& error) {
  spdlog::error("{} SQL 失败，sqlstate={}，detail={}", operation,
                error.Sqlstate(), error.what());
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 拒绝操作");
}

application::ServiceError ConnectionInterrupted(std::string_view operation) {
  return MakeError(application::ErrorCode::kStorageUnavailable,
                   std::string{operation} + "时 MySQL 连接中断", true);
}

application::ServiceError InvalidDatabaseState(std::string_view detail) {
  return MakeError(
      application::ErrorCode::kInternal,
      std::string{"MySQL 对局数据不满足当前契约："} + std::string{detail});
}

std::int64_t UnixMilliseconds(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch())
      .count();
}

int Hexadecimal(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::optional<domain::MatchId> MatchIdFromUuid(std::string_view value) {
  if (value.size() != 36U || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return std::nullopt;
  }
  domain::MatchId::ByteArray bytes{};
  std::size_t source = 0U;
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (source == 8U || source == 13U || source == 18U || source == 23U) {
      ++source;
    }
    const auto high = Hexadecimal(value[source]);
    const auto low = Hexadecimal(value[source + 1U]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    source += 2U;
  }
  return domain::MatchId::FromBytes(bytes);
}

std::string ScoreModelText(domain::MatchScoreModel value) {
  switch (value) {
    case domain::MatchScoreModel::kTeamPangs:
      return "team_pangs";
    case domain::MatchScoreModel::kIndividualPangs:
      return "individual_pangs";
    case domain::MatchScoreModel::kTeamObjectives:
      return "team_objectives";
    case domain::MatchScoreModel::kIndividualObjectives:
      return "individual_objectives";
    case domain::MatchScoreModel::kTeamElimination:
      return "team_elimination";
    case domain::MatchScoreModel::kIndividualElimination:
      return "individual_elimination";
    case domain::MatchScoreModel::kTeamRounds:
      return "team_rounds";
    case domain::MatchScoreModel::kCooperative:
      return "cooperative";
    case domain::MatchScoreModel::kInfection:
      return "infection";
  }
  return "unsupported";
}

std::string CompletionText(application::MatchCompletionKind value) {
  switch (value) {
    case application::MatchCompletionKind::kCompleted:
      return "completed";
    case application::MatchCompletionKind::kInterrupted:
      return "interrupted";
    case application::MatchCompletionKind::kAbandoned:
      return "abandoned";
  }
  return "unsupported";
}

std::optional<application::MatchCompletionKind> CompletionFromText(
    std::string_view value) {
  if (value == "completed") {
    return application::MatchCompletionKind::kCompleted;
  }
  if (value == "interrupted") {
    return application::MatchCompletionKind::kInterrupted;
  }
  if (value == "abandoned") {
    return application::MatchCompletionKind::kAbandoned;
  }
  return std::nullopt;
}

std::string OutcomeText(domain::MatchOutcome value) {
  switch (value) {
    case domain::MatchOutcome::kWin:
      return "win";
    case domain::MatchOutcome::kDraw:
      return "draw";
    case domain::MatchOutcome::kLoss:
      return "loss";
  }
  return "unsupported";
}

std::optional<domain::MatchOutcome> OutcomeFromText(std::string_view value) {
  if (value == "win") {
    return domain::MatchOutcome::kWin;
  }
  if (value == "draw") {
    return domain::MatchOutcome::kDraw;
  }
  if (value == "loss") {
    return domain::MatchOutcome::kLoss;
  }
  return std::nullopt;
}

application::MatchSettlementCommand NormalizedCommand(
    const application::MatchSettlementCommand& command) {
  auto result = command;
  std::ranges::sort(result.participants, {},
                    &application::MatchSettlementParticipant::player_id);
  return result;
}

std::optional<std::array<std::uint32_t, 16U>> PveUnitsFromJson(
    const Json& value) {
  std::array<std::uint32_t, 16U> result{};
  const auto found = value.find("pve_units");
  if (found == value.end()) return result;
  if (!found->is_array() || found->size() != result.size()) return std::nullopt;
  for (std::size_t index = 0U; index < result.size(); ++index) {
    const auto& count = (*found)[index];
    if (!count.is_number_integer() ||
        (!count.is_number_unsigned() && count.get<std::int64_t>() < 0) ||
        count.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
      return std::nullopt;
    result[index] = count.get<std::uint32_t>();
  }
  return result;
}

bool HasPveUnits(const domain::MatchPerformance& performance) {
  return std::ranges::any_of(performance.pve_units,
                             [](auto count) { return count != 0U; });
}

Json TerminalSnapshot(const application::MatchSettlementCommand& input) {
  const auto command = NormalizedCommand(input);
  Json participants = Json::array();
  for (const auto& participant : command.participants) {
    participants.push_back(
        {{"player_id", participant.player_id.Value()},
         {"seat_index", participant.seat_index},
         {"team_code", participant.team_code},
         {"bubbles", participant.performance.bubbles},
         {"pangs", participant.performance.pangs},
         {"deaths", participant.performance.deaths},
         {"helps", participant.performance.helps},
         {"objective_points", participant.performance.objective_points},
         {"outcome", OutcomeText(participant.outcome)},
         {"rank", participant.rank},
         {"departed", participant.departed}});
    if (participant.performance.loot_lucci != 0U)
      participants.back()["loot_lucci"] = participant.performance.loot_lucci;
    if (HasPveUnits(participant.performance))
      participants.back()["pve_units"] = participant.performance.pve_units;
  }
  Json snapshot{{"schema_version", 1},
                {"match_id", command.match_id.ToUuidString()},
                {"mode_id", command.mode_id},
                {"elapsed_seconds", command.elapsed_seconds},
                {"completion_kind", CompletionText(command.completion_kind)},
                {"participants", std::move(participants)}};
  if (command.room_owner)
    snapshot["room_owner"] = {
        {"instance_id", command.room_owner->instance_id.ToUuidString()},
        {"owner_epoch", command.room_owner->owner_epoch}};
  return snapshot;
}

application::ServiceResult<application::MatchSettlementCommand>
CommandFromSnapshot(std::string_view serialized) {
  try {
    const auto document = Json::parse(serialized);
    if (!document.is_object() ||
        document.at("schema_version").get<int>() != 1 ||
        !document.at("participants").is_array()) {
      return InvalidDatabaseState("终局快照版本或参与者结构非法");
    }
    const auto match_id =
        MatchIdFromUuid(document.at("match_id").get<std::string>());
    const auto completion =
        CompletionFromText(document.at("completion_kind").get<std::string>());
    if (!match_id.has_value() || !completion.has_value()) {
      return InvalidDatabaseState("终局快照包含无效对局标识或终局类型");
    }
    application::MatchSettlementCommand command{
        .match_id = *match_id,
        .mode_id = document.at("mode_id").get<std::uint32_t>(),
        .elapsed_seconds = document.at("elapsed_seconds").get<std::uint32_t>(),
        .completion_kind = *completion};
    if (document.contains("room_owner")) {
      const auto& owner = document.at("room_owner");
      const auto instance = domain::MatchId::FromUuidString(
          owner.at("instance_id").get<std::string>());
      if (!instance)
        return InvalidDatabaseState("invalid terminal room instance");
      command.room_owner = application::RoomOwnership{
          *instance, owner.at("owner_epoch").get<std::uint64_t>()};
    }
    for (const auto& value : document.at("participants")) {
      const auto player_id = value.at("player_id").get<std::uint64_t>();
      const auto outcome =
          OutcomeFromText(value.at("outcome").get<std::string>());
      if (player_id == 0U || !outcome.has_value()) {
        return InvalidDatabaseState("终局快照包含无效玩家或胜负");
      }
      const auto units = PveUnitsFromJson(value);
      if (!units) return InvalidDatabaseState("终局快照的PVE类别次数结构非法");
      command.participants.push_back(
          {.player_id = domain::PlayerId{player_id},
           .seat_index = value.at("seat_index").get<std::uint8_t>(),
           .team_code = value.at("team_code").get<std::uint8_t>(),
           .performance = {.bubbles = value.at("bubbles").get<std::uint32_t>(),
                           .pangs = value.at("pangs").get<std::uint32_t>(),
                           .deaths = value.at("deaths").get<std::uint32_t>(),
                           .helps = value.at("helps").get<std::uint32_t>(),
                           .loot_lucci = value.value("loot_lucci", 0U),
                           .objective_points = value.at("objective_points")
                                                   .get<std::uint32_t>(),
                           .pve_units = *units},
           .outcome = *outcome,
           .rank = value.at("rank").get<std::uint32_t>(),
           .departed = value.at("departed").get<bool>()});
    }
    if (const auto error =
            application::ValidateMatchSettlementCommand(command)) {
      return InvalidDatabaseState(error->safe_message);
    }
    return NormalizedCommand(command);
  } catch (const Json::exception&) {
    return InvalidDatabaseState("终局快照不是完整的版本 1 JSON");
  }
}

domain::RewardRateLine RateLineFromJson(const Json& value) {
  return {.finish = value.at("finish").get<std::int32_t>(),
          .win = value.at("win").get<std::int32_t>(),
          .draw = value.at("draw").get<std::int32_t>(),
          .loss = value.at("loss").get<std::int32_t>(),
          .bubble = value.at("bubble").get<std::int32_t>(),
          .pang = value.at("pang").get<std::int32_t>(),
          .help = value.at("help").get<std::int32_t>(),
          .first = value.at("first").get<std::int32_t>(),
          .second = value.at("second").get<std::int32_t>(),
          .third = value.at("third").get<std::int32_t>()};
}

domain::RewardRateOverride RateOverrideFromJson(const Json& value) {
  domain::RewardRateOverride result;
  const auto assign = [&value](std::string_view key,
                               std::optional<std::int32_t>& target) {
    const auto iterator = value.find(key);
    if (iterator != value.end()) {
      target = iterator->get<std::int32_t>();
    }
  };
  assign("finish", result.finish);
  assign("win", result.win);
  assign("draw", result.draw);
  assign("loss", result.loss);
  assign("bubble", result.bubble);
  assign("pang", result.pang);
  assign("help", result.help);
  assign("first", result.first);
  assign("second", result.second);
  assign("third", result.third);
  return result;
}

application::ServiceResult<domain::MatchRewardPolicy> RewardPolicyFromPayload(
    std::string_view serialized) {
  try {
    const auto document = Json::parse(serialized);
    const auto& policy = document.at("policy");
    const auto& defaults = policy.at("defaults");
    domain::MatchRewardPolicy result{
        .version = policy.at("version").get<std::uint32_t>(),
        .minimum_seconds = policy.at("minimum_seconds").get<std::uint32_t>(),
        .minimum_players = policy.at("minimum_players").get<std::uint32_t>(),
        .level_step = policy.at("level_step").get<std::uint32_t>(),
        .maximum_level = policy.at("maximum_level").get<std::uint32_t>(),
        .experience = RateLineFromJson(defaults.at("xp")),
        .lucci = RateLineFromJson(defaults.at("lucci")),
        .rating = RateLineFromJson(defaults.at("rating"))};
    const auto& modes = policy.at("modes");
    if (!modes.is_object()) {
      return InvalidDatabaseState("奖励规则 modes 不是对象");
    }
    for (auto iterator = modes.begin(); iterator != modes.end(); ++iterator) {
      const auto mode_value = std::stoull(iterator.key());
      if (mode_value == 0U ||
          mode_value > std::numeric_limits<std::uint32_t>::max()) {
        return InvalidDatabaseState("奖励规则模式编号越界");
      }
      domain::ModeRewardOverride override;
      const auto& value = iterator.value();
      if (value.contains("xp")) {
        override.experience = RateOverrideFromJson(value.at("xp"));
      }
      if (value.contains("lucci")) {
        override.lucci = RateOverrideFromJson(value.at("lucci"));
      }
      if (value.contains("rating")) {
        override.rating = RateOverrideFromJson(value.at("rating"));
      }
      result.modes.push_back({.mode_id = static_cast<std::uint32_t>(mode_value),
                              .override = override});
    }
    std::ranges::sort(result.modes, {}, &domain::ModeRewardRule::mode_id);
    if (domain::ValidateMatchRewardPolicy(result).has_value()) {
      return InvalidDatabaseState("已发布奖励规则违反领域约束");
    }
    return result;
  } catch (const std::exception&) {
    return InvalidDatabaseState("已发布奖励规则不是完整的受支持 JSON");
  }
}

Json BalancesJson(const domain::CurrentRewardBalances& value) {
  return {{"experience", value.experience},
          {"lucci", value.lucci},
          {"rating", value.rating}};
}

application::ServiceResult<domain::CurrentRewardBalances> BalancesFromJson(
    const Json& value) {
  try {
    return domain::CurrentRewardBalances{
        .experience = value.at("experience").get<std::int64_t>(),
        .lucci = value.at("lucci").get<std::int64_t>(),
        .rating = value.at("rating").get<std::int64_t>()};
  } catch (const Json::exception&) {
    return InvalidDatabaseState("结算结果余额字段不完整");
  }
}

Json ResultSnapshot(const application::MatchSettlementResult& result) {
  Json players = Json::array();
  for (const auto& player : result.players) {
    players.push_back(
        {{"player_id", player.player_id.Value()},
         {"performance",
          {{"bubbles", player.performance.bubbles},
           {"pangs", player.performance.pangs},
           {"deaths", player.performance.deaths},
           {"helps", player.performance.helps},
           {"objective_points", player.performance.objective_points},
           {"loot_lucci", player.performance.loot_lucci}}},
         {"effective_outcome", OutcomeText(player.effective_outcome)},
         {"rank", player.rank},
         {"reward",
          {{"before", BalancesJson(player.reward.before)},
           {"after", BalancesJson(player.reward.after)},
           {"applied_award",
            {{"eligible", player.reward.applied_award.eligible},
             {"experience", player.reward.applied_award.experience},
             {"lucci", player.reward.applied_award.lucci},
             {"rating", player.reward.applied_award.rating},
             {"effective_outcome",
              OutcomeText(player.reward.applied_award.effective_outcome)}}},
           {"progression",
            {{"experience", player.reward.progression.experience},
             {"level", player.reward.progression.level},
             {"level_base", player.reward.progression.level_base},
             {"level_next", player.reward.progression.level_next}}}}}});
    if (HasPveUnits(player.performance))
      players.back()["performance"]["pve_units"] = player.performance.pve_units;
  }
  return {{"schema_version", 1},
          {"match_id", result.match_id.ToUuidString()},
          {"mode_id", result.mode_id},
          {"elapsed_seconds", result.elapsed_seconds},
          {"completion_kind", CompletionText(result.completion_kind)},
          {"players", std::move(players)}};
}

application::ServiceResult<application::MatchSettlementResult>
ResultFromSnapshot(std::string_view serialized) {
  try {
    const auto document = Json::parse(serialized);
    if (document.at("schema_version").get<int>() != 1 ||
        !document.at("players").is_array()) {
      return InvalidDatabaseState("结算结果版本或玩家结构非法");
    }
    const auto match_id =
        MatchIdFromUuid(document.at("match_id").get<std::string>());
    const auto completion =
        CompletionFromText(document.at("completion_kind").get<std::string>());
    if (!match_id.has_value() || !completion.has_value()) {
      return InvalidDatabaseState("结算结果包含无效对局标识或终局类型");
    }
    application::MatchSettlementResult result{
        .match_id = *match_id,
        .mode_id = document.at("mode_id").get<std::uint32_t>(),
        .elapsed_seconds = document.at("elapsed_seconds").get<std::uint32_t>(),
        .completion_kind = *completion};
    for (const auto& value : document.at("players")) {
      const auto player_id = value.at("player_id").get<std::uint64_t>();
      const auto outcome =
          OutcomeFromText(value.at("effective_outcome").get<std::string>());
      const auto award_outcome = OutcomeFromText(value.at("reward")
                                                     .at("applied_award")
                                                     .at("effective_outcome")
                                                     .get<std::string>());
      auto before = BalancesFromJson(value.at("reward").at("before"));
      auto after = BalancesFromJson(value.at("reward").at("after"));
      const auto* before_value =
          std::get_if<domain::CurrentRewardBalances>(&before);
      const auto* after_value =
          std::get_if<domain::CurrentRewardBalances>(&after);
      if (player_id == 0U || !outcome.has_value() ||
          !award_outcome.has_value() || before_value == nullptr ||
          after_value == nullptr) {
        return InvalidDatabaseState("结算结果包含无效玩家、胜负或余额");
      }
      const auto& performance = value.at("performance");
      const auto units = PveUnitsFromJson(performance);
      if (!units) return InvalidDatabaseState("结算结果的PVE类别次数结构非法");
      const auto total =
          std::accumulate(units->begin(), units->end(), std::uint64_t{0U});
      if ((result.mode_id != 16U && total != 0U) ||
          total > performance.at("pangs").get<std::uint32_t>())
        return InvalidDatabaseState(
            "结算结果的PVE类别次数不属于该模式或超过击爆数");
      const auto& award = value.at("reward").at("applied_award");
      const auto& progression = value.at("reward").at("progression");
      result.players.push_back(
          {.player_id = domain::PlayerId{player_id},
           .performance =
               {.bubbles = performance.at("bubbles").get<std::uint32_t>(),
                .pangs = performance.at("pangs").get<std::uint32_t>(),
                .deaths = performance.at("deaths").get<std::uint32_t>(),
                .helps = performance.at("helps").get<std::uint32_t>(),
                .loot_lucci = performance.value("loot_lucci", 0U),
                .objective_points =
                    performance.at("objective_points").get<std::uint32_t>(),
                .pve_units = *units},
           .effective_outcome = *outcome,
           .rank = value.at("rank").get<std::uint32_t>(),
           .reward = {
               .before = *before_value,
               .after = *after_value,
               .applied_award = {.eligible = award.at("eligible").get<bool>(),
                                 .experience =
                                     award.at("experience").get<std::int64_t>(),
                                 .lucci = award.at("lucci").get<std::int64_t>(),
                                 .rating =
                                     award.at("rating").get<std::int64_t>(),
                                 .effective_outcome = *award_outcome},
               .progression = {
                   .experience =
                       progression.at("experience").get<std::int64_t>(),
                   .level = progression.at("level").get<std::uint32_t>(),
                   .level_base =
                       progression.at("level_base").get<std::int64_t>(),
                   .level_next =
                       progression.at("level_next").get<std::int64_t>()}}});
    }
    return result;
  } catch (const Json::exception&) {
    return InvalidDatabaseState("结算结果不是完整的版本 1 JSON");
  }
}

class MySqlMatchStartTransaction final
    : public application::MatchStartTransaction {
 public:
  explicit MySqlMatchStartTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::MatchStartClaimState> Persist(
      const application::MatchStartCommand& command) override {
    if (completed_ || persisted_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "对局开局事务已使用");
    }
    const auto match_id = command.match_id.ToUuidString();
    const auto started_at = UnixMilliseconds(command.started_at);
    auto rules =
        Json{{"schema_version", 1},
             {"reward_rules_version_id", command.reward_rules_version_id},
             {"score_model", ScoreModelText(command.score_model)}};
    if (command.source_channel != 0U)
      rules["source_channel"] = command.source_channel;
    if (command.room_owner)
      rules["room_owner"] = {
          {"instance_id", command.room_owner->instance_id.ToUuidString()},
          {"owner_epoch", command.room_owner->owner_epoch}};
    if (command.user_host_prize) {
      const auto& prize = *command.user_host_prize;
      rules["user_host_prize"] = {
          {"sponsor_player_id", prize.sponsor_player_id.Value()},
          {"amount", prize.amount},
          {"minimum_amount", prize.minimum_amount},
          {"maximum_amount", prize.maximum_amount},
          {"policy_version", prize.policy_version}};
    }
    try {
      if (command.room_owner) {
        if (!LockRoomOwnership(transaction_, *command.room_owner,
                               command.source_room_id))
          return MakeError(application::ErrorCode::kConflict,
                           "房间实例所有权已失效");
      }
      // Serialize start/cancel even when COMMIT's reply is lost. SELECT FOR
      // UPDATE alone cannot see an uncommitted insertion and would permit a
      // late charge.
      transaction_.Exec(
          "SELECT LockKey FROM TransactionLocks WHERE LockKey=CONCAT('match_',LEFT(SHA2($1,256),1)) "
          "FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_, match_id});
      const auto configurations = transaction_.Exec(
          "SELECT `ConfigurationVersionId`,`ConfigurationKind`,`ReleaseStatus` FROM "
          "`ConfigurationVersions` WHERE `ConfigurationVersionId` IN($1,$2) ORDER BY "
          "`ConfigurationVersionId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, command.configuration_version_id,
              command.reward_rules_version_id});
      bool configuration_available = false;
      bool reward_available = false;
      for (const auto& row : configurations) {
        const auto identifier = row[0].As<std::int64_t>();
        const auto status = row[2].As<std::string_view>();
        if (identifier == command.configuration_version_id &&
            status == "published") {
          configuration_available = true;
        }
        if (identifier == command.reward_rules_version_id &&
            row[1].As<std::string_view>() == "reward_rules" &&
            status == "published") {
          reward_available = true;
        }
      }
      if (!configuration_available || !reward_available) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "对局引用的玩法配置或奖励规则尚未发布");
      }
      const auto inserted = transaction_.Exec(
          "INSERT INTO "
          "`Matches`(`MatchId`,`SourceRoomId`,`ModeId`,`MatchStatus`,`ConfigurationVersionId`,`RulesSnapshot`,`StartedAt`,`RoomInstanceId`,`RoomOwnerEpoch`)"
          " VALUES($1,$2,$3,'playing',$4,$5,FROM_UNIXTIME($6/1000.0),$7,$8) ON CONFLICT(`MatchId`) DO"
          " NOTHING RETURNING `MatchId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, match_id, command.source_room_id.Value(),
              command.mode_id, command.configuration_version_id, rules.dump(),
              started_at,
              command.room_owner ? std::optional{command.room_owner->instance_id
                                                     .ToUuidString()}
                                 : std::nullopt,
              command.room_owner
                  ? std::optional{command.room_owner->owner_epoch}
                  : std::nullopt});
      if (!inserted.empty()) {
        for (const auto& participant : command.participants) {
          transaction_.Exec(
              "INSERT INTO `MatchParticipants`(`MatchId`,`PlayerId`,`SeatIndex`,`TeamCode`,`JoinedAt`) "
              "VALUES($1,$2,$3,$4,FROM_UNIXTIME($5/1000.0))",
              bf::infrastructure::mysql::Parameters{
                  transaction_, match_id, participant.player_id.Value(),
                  static_cast<std::uint16_t>(participant.seat_index),
                  static_cast<std::uint16_t>(participant.team_code),
                  started_at});
        }
        transaction_.Exec(
            "INSERT INTO "
            "`MatchCoupleBenefits`(`MatchId`,`PlayerId`,`PartnerPlayerId`,`CoupleId`,`RingItemId`) "
            "SELECT p.`MatchId`,p.`PlayerId`,other.`PlayerId`,c.`CoupleId`,c.`RingItemId` FROM "
            "`MatchParticipants` p JOIN `CoupleMembers` self ON self.`PlayerId`=p.`PlayerId` JOIN "
            "`Couples` c ON c.`CoupleId`=self.`CoupleId` JOIN `CoupleMembers` other ON "
            "other.`CoupleId`=c.`CoupleId` AND other.`PlayerId`<>p.`PlayerId` JOIN `MatchParticipants` "
            "peer ON peer.`MatchId`=p.`MatchId` AND peer.`PlayerId`=other.`PlayerId` WHERE "
            "p.`MatchId`=$1 AND c.`RingItemId` IN(100985,101396) AND EXISTS(SELECT 1 FROM `PlayerItems`"
            " i WHERE i.`PlayerId`=p.`PlayerId` AND i.`ItemId`=c.`RingItemId` AND (i.`ExpiresAt` IS "
            "NULL OR i.`ExpiresAt`>CURRENT_TIMESTAMP(6))) AND EXISTS(SELECT 1 FROM `PlayerItems` i "
            "WHERE i.`PlayerId`=other.`PlayerId` AND i.`ItemId`=c.`RingItemId` AND (i.`ExpiresAt` IS "
            "NULL OR i.`ExpiresAt`>CURRENT_TIMESTAMP(6)))",
            bf::infrastructure::mysql::Parameters{transaction_, match_id});
        if (command.source_channel == 80U &&
            (command.score_model == domain::MatchScoreModel::kTeamPangs ||
             command.score_model == domain::MatchScoreModel::kTeamObjectives ||
             command.score_model == domain::MatchScoreModel::kTeamElimination ||
             command.score_model == domain::MatchScoreModel::kTeamRounds)) {
          // 一个语句快照冻结两队归属；以后退队、换队不会改写历史战绩。
          transaction_.Exec(
              "INSERT INTO `ClanMatchSides`(`MatchId`,`TeamCode`,`ClanId`) WITH sides AS (SELECT "
              "p.`TeamCode`,min(c.`ClanId`) `ClanId`,count(*) members FROM `MatchParticipants` p LEFT "
              "JOIN `ClanMemberships` cm USING(`PlayerId`) LEFT JOIN `Clans` c ON c.`ClanId`=cm.`ClanId` "
              "AND c.`ClanId`<>99 AND c.`ClanStatus`='active' WHERE p.`MatchId`=$1 GROUP BY p.`TeamCode` "
              "HAVING count(c.`ClanId`)=count(*) AND count(DISTINCT c.`ClanId`)=1), valid AS (SELECT 1 "
              "FROM sides HAVING count(*)=2 AND count(DISTINCT `ClanId`)=2 AND min(members)=max(members))"
              " SELECT $1,`TeamCode`,`ClanId` FROM sides WHERE EXISTS(SELECT 1 FROM valid)",
              bf::infrastructure::mysql::Parameters{transaction_, match_id});
        }
        if (const auto error = FundUserHostPrize(transaction_, command))
          return *error;
        persisted_ = true;
        return application::MatchStartClaimState::kCreated;
      }

      const auto existing = transaction_.Exec(
          "SELECT "
          "`SourceRoomId`,`ModeId`,`ConfigurationVersionId`,`RulesSnapshot`,round(UNIX_TIMESTAMP(`StartedAt`)*1000)"
          " FROM `Matches` WHERE `MatchId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{transaction_, match_id});
      if (existing.size() != 1U ||
          existing[0][0].As<std::uint64_t>() !=
              command.source_room_id.Value() ||
          existing[0][1].As<std::uint32_t>() != command.mode_id ||
          existing[0][2].As<std::int64_t>() !=
              command.configuration_version_id ||
          existing[0][4].As<std::int64_t>() != started_at ||
          Json::parse(existing[0][3].As<std::string>()) != rules) {
        return MakeError(application::ErrorCode::kConflict,
                         "对局标识已用于不同的开局快照");
      }
      const auto participants = transaction_.Exec(
          "SELECT `PlayerId`,`SeatIndex`,`TeamCode` FROM `MatchParticipants` WHERE `MatchId`=$1 ORDER"
          " BY `PlayerId`",
          bf::infrastructure::mysql::Parameters{transaction_, match_id});
      if (participants.size() != command.participants.size()) {
        return MakeError(application::ErrorCode::kConflict,
                         "既有对局参与者数量与重试命令不同");
      }
      for (std::size_t index = 0U; index < command.participants.size();
           ++index) {
        const auto& expected = command.participants[index];
        const auto row = participants[static_cast<
            bf::infrastructure::mysql::QueryResult::size_type>(index)];
        if (row[0].As<std::uint64_t>() != expected.player_id.Value() ||
            row[1].As<std::uint16_t>() != expected.seat_index ||
            row[2].As<std::uint16_t>() != expected.team_code) {
          return MakeError(application::ErrorCode::kConflict,
                           "既有对局参与者与重试命令不同");
        }
      }
      persisted_ = true;
      return application::MatchStartClaimState::kExistingSame;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("保存对局开局");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("保存对局开局", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "保存对局开局时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_ || !persisted_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "对局开局事务不能提交");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("提交对局开局");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("提交对局开局", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交对局开局时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  bool persisted_{false};
  bool completed_{false};
};

class MySqlMatchSettlementTransaction final
    : public application::MatchSettlementTransaction {
 public:
  explicit MySqlMatchSettlementTransaction(MySqlConnectionPool::Lease lease)
      : lease_(std::move(lease)), transaction_(lease_.Connection()) {}

  application::ServiceResult<application::MatchSettlementClaim> ClaimSettlement(
      const application::MatchSettlementCommand& input) override {
    if (completed_ || command_.has_value()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "对局结算事务已使用");
    }
    const auto command = NormalizedCommand(input);
    const auto match_id = command.match_id.ToUuidString();
    const auto terminal = TerminalSnapshot(command).dump();
    try {
      const auto rows = transaction_.Exec(
          "SELECT "
          "settlement.`SettlementStatus`,(settlement.`TerminalSnapshot`=CAST($2 AS JSON)),settlement.`RewardRulesVersionId`,settlement.`ResultSnapshot`,`Match`.`MatchStatus`"
          " FROM `MatchSettlements` AS settlement JOIN `Matches`  AS `Match` ON "
          "`Match`.`MatchId`=settlement.`MatchId` WHERE settlement.`MatchId`=$1 FOR UPDATE OF "
          "`Match`,settlement",
          bf::infrastructure::mysql::Parameters{transaction_, match_id,
                                                terminal});
      if (rows.empty()) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "对局终局尚未写入耐久日志");
      }
      if (!rows[0][1].As<bool>()) {
        return MakeError(application::ErrorCode::kConflict,
                         "对局终局与耐久日志内容冲突");
      }
      const auto rules_version = rows[0][2].As<std::int64_t>();
      const auto configuration = transaction_.Exec(
          "SELECT `Payload` FROM `ConfigurationVersions` WHERE `ConfigurationVersionId`=$1 AND "
          "`ConfigurationKind`='reward_rules' AND `ReleaseStatus` IN('published','retired')",
          bf::infrastructure::mysql::Parameters{transaction_, rules_version});
      if (configuration.size() != 1U) {
        return InvalidDatabaseState("结算引用的奖励规则版本不存在或不可使用");
      }
      auto policy_result =
          RewardPolicyFromPayload(configuration[0][0].As<std::string>());
      const auto* policy =
          std::get_if<domain::MatchRewardPolicy>(&policy_result);
      if (policy == nullptr) {
        return std::get<application::ServiceError>(std::move(policy_result));
      }
      std::vector<domain::PlayerId> bonuses;
      if (command.completion_kind ==
          application::MatchCompletionKind::kCompleted) {
        for (
            const auto& benefit : transaction_.Exec(
                "SELECT `PlayerId`,`PartnerPlayerId` FROM `MatchCoupleBenefits` WHERE `MatchId`=$1 ORDER BY"
                " `PlayerId`",
                bf::infrastructure::mysql::Parameters{transaction_,
                                                      match_id})) {
          const auto p = domain::PlayerId{benefit[0].As<std::uint64_t>()};
          const auto peer = domain::PlayerId{benefit[1].As<std::uint64_t>()};
          const auto active = [&](domain::PlayerId id) {
            return std::ranges::any_of(
                command.participants, [&](const auto& member) {
                  return member.player_id == id && !member.departed;
                });
          };
          if (active(p) && active(peer)) bonuses.push_back(p);
        }
      }
      const auto status = rows[0][0].As<std::string_view>();
      if (status == "succeeded") {
        if (rows[0][3].IsNull() ||
            rows[0][4].As<std::string_view>() != "settled") {
          return InvalidDatabaseState("成功结算缺少结果快照或对局终态");
        }
        auto replay_result = ResultFromSnapshot(rows[0][3].As<std::string>());
        const auto* replay =
            std::get_if<application::MatchSettlementResult>(&replay_result);
        if (replay == nullptr) {
          return std::get<application::ServiceError>(std::move(replay_result));
        }
        command_ = command;
        return application::MatchSettlementClaim{
            .state = application::MatchSettlementClaimState::kCompletedReplay,
            .reward_policy = *policy,
            .completed_result = *replay,
            .couple_bonus_players = std::move(bonuses)};
      }
      if (status == "failed_terminal") {
        return MakeError(application::ErrorCode::kInvalidState,
                         "对局结算已进入不可自动重试状态");
      }
      if (status != "pending" && status != "processing" &&
          status != "failed_retryable") {
        return InvalidDatabaseState("结算状态未知");
      }
      const auto changed = transaction_.Exec(
          "UPDATE `MatchSettlements` SET "
          "`SettlementStatus`='processing',`AttemptCount`=`AttemptCount`+1,`FailureCode`=NULL,`ProcessingStartedAt`=CURRENT_TIMESTAMP(6),`UpdatedAt`=CURRENT_TIMESTAMP(6)"
          " WHERE `MatchId`=$1 RETURNING `SettlementId`",
          bf::infrastructure::mysql::Parameters{transaction_, match_id});
      if (changed.size() != 1U) {
        return InvalidDatabaseState("无法占用唯一结算行");
      }
      command_ = command;
      rules_version_id_ = rules_version;
      return application::MatchSettlementClaim{
          .state = application::MatchSettlementClaimState::kNew,
          .reward_policy = *policy,
          .completed_result = std::nullopt,
          .couple_bonus_players = std::move(bonuses)};
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("占用对局结算");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("占用对局结算", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "占用对局结算时发生未分类错误");
    }
  }

  application::ServiceResult<std::vector<application::LockedRewardBalances>>
  LockRewardBalances(std::span<const domain::PlayerId> player_ids) override {
    if (!command_.has_value() || rules_version_id_ <= 0 ||
        !locked_players_.empty()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "结算尚未占用或余额已锁定");
    }
    std::vector<application::LockedRewardBalances> result;
    result.reserve(player_ids.size());
    try {
      std::optional<domain::PlayerId> previous;
      for (const auto player_id : player_ids) {
        if (!player_id.IsValid() ||
            (previous && previous->Value() >= player_id.Value()))
          return MakeError(application::ErrorCode::kInternal,
                           "结算玩家必须按严格升序锁定");
        previous = player_id;
      }
      std::vector<domain::PlayerId> lock_ids{player_ids.begin(),
                                             player_ids.end()};
      if (const auto sponsor =
              UserHostSponsor(transaction_, command_->match_id.ToUuidString()))
        lock_ids.push_back(*sponsor);
      std::ranges::sort(lock_ids);
      lock_ids.erase(std::unique(lock_ids.begin(), lock_ids.end()),
                     lock_ids.end());
      previous.reset();
      for (const auto player_id : lock_ids) {
        if (!player_id.IsValid() ||
            (previous.has_value() &&
             !(previous->Value() < player_id.Value()))) {
          return MakeError(application::ErrorCode::kInternal,
                           "结算玩家必须按严格升序锁定");
        }
        const auto profile = transaction_.Exec(
            "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
            bf::infrastructure::mysql::Parameters{transaction_,
                                                  player_id.Value()});
        const auto progression = transaction_.Exec(
            "SELECT `Experience`,`Rating` FROM `PlayerProgressions` WHERE `PlayerId`=$1 FOR UPDATE",
            bf::infrastructure::mysql::Parameters{transaction_,
                                                  player_id.Value()});
        const auto wallet = transaction_.Exec(
            "SELECT `Balance` FROM `PlayerWallets` WHERE `PlayerId`=$1 AND `CurrencyCode`='lucci' FOR "
            "UPDATE",
            bf::infrastructure::mysql::Parameters{transaction_,
                                                  player_id.Value()});
        if (profile.size() != 1U || progression.size() != 1U ||
            wallet.size() != 1U) {
          return MakeError(application::ErrorCode::kInvalidState,
                           "对局参与者缺少资料、进度或 Lucci 钱包");
        }
        if (std::ranges::binary_search(player_ids, player_id)) {
          result.push_back(
              {.player_id = player_id,
               .balances = {.experience = progression[0][0].As<std::int64_t>(),
                            .lucci = wallet[0][0].As<std::int64_t>(),
                            .rating = progression[0][1].As<std::int64_t>()}});
          locked_players_.insert(player_id);
        }
        previous = player_id;
      }
      return result;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("锁定结算余额");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("锁定结算余额", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "锁定结算余额时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> ApplyPlayerSettlement(
      const application::MatchSettlementPlayerWrite& write) override {
    if (!command_.has_value() || !(write.match_id == command_->match_id) ||
        !locked_players_.contains(write.result.player_id) ||
        applied_players_.contains(write.result.player_id)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "玩家结算写入顺序或对局身份非法");
    }
    const auto participant =
        std::ranges::find(command_->participants, write.result.player_id,
                          &application::MatchSettlementParticipant::player_id);
    if (participant == command_->participants.end()) {
      return MakeError(application::ErrorCode::kInternal,
                       "结算结果玩家不在终局快照中");
    }
    const auto match_id = write.match_id.ToUuidString();
    const auto& reward = write.result.reward;
    auto statistics =
        Json{{"schema_version", 1},
             {"bubbles", write.result.performance.bubbles},
             {"pangs", write.result.performance.pangs},
             {"deaths", write.result.performance.deaths},
             {"helps", write.result.performance.helps},
             {"objective_points", write.result.performance.objective_points},
             {"loot_lucci", write.result.performance.loot_lucci},
             {"effective_outcome", OutcomeText(write.result.effective_outcome)},
             {"rank", write.result.rank},
             {"departed", participant->departed}};
    if (HasPveUnits(write.result.performance))
      statistics["pve_units"] = write.result.performance.pve_units;
    try {
      const auto progression = transaction_.Exec(
          "UPDATE `PlayerProgressions` SET "
          "`Experience`=$2,`Rating`=$3,`RulesVersionId`=$4,`UpdatedAt`=CURRENT_TIMESTAMP(6) WHERE "
          "`PlayerId`=$1 AND `Experience`=$5 AND `Rating`=$6 RETURNING `PlayerId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, write.result.player_id.Value(),
              reward.after.experience, reward.after.rating, rules_version_id_,
              reward.before.experience, reward.before.rating});
      if (progression.size() != 1U) {
        return InvalidDatabaseState("锁定后的玩家进度发生不一致变化");
      }
      std::optional<std::int64_t> wallet_transaction_id;
      if (reward.applied_award.lucci != 0) {
        const auto wallet_change = transaction_.Exec(
            "UPDATE PlayerWallets SET Balance=$3,UpdatedAt=CURRENT_TIMESTAMP(6) "
            "WHERE PlayerId=$1 AND CurrencyCode='lucci' AND Balance=$2",
            Parameters{transaction_, write.result.player_id.Value(),
                       reward.before.lucci, reward.after.lucci});
        if (wallet_change.AffectedRows() != 1U) {
          return InvalidDatabaseState("锁定后的 Lucci 钱包发生不一致变化");
        }
        const auto wallet = transaction_.Exec(
            "INSERT INTO WalletTransactions (PlayerId,CurrencyCode,AmountDelta,BalanceBefore,"
            "BalanceAfter,ReasonCode,BusinessId) VALUES($1,'lucci',$4,$2,$3,'match_reward',$5) "
            "RETURNING WalletTransactionId",
            Parameters{transaction_, write.result.player_id.Value(),
                       reward.before.lucci, reward.after.lucci,
                       reward.applied_award.lucci, match_id});
        if (wallet.size() != 1U) {
          return InvalidDatabaseState("锁定后的 Lucci 钱包发生不一致变化");
        }
        wallet_transaction_id = wallet[0][0].As<std::int64_t>();
      } else if (reward.after.lucci != reward.before.lucci) {
        return InvalidDatabaseState("零 Lucci 奖励却改变了余额");
      }

      const auto win =
          write.result.effective_outcome == domain::MatchOutcome::kWin ? 1 : 0;
      const auto draw =
          write.result.effective_outcome == domain::MatchOutcome::kDraw ? 1 : 0;
      const auto loss =
          write.result.effective_outcome == domain::MatchOutcome::kLoss ? 1 : 0;
      transaction_.Exec(
          "INSERT INTO "
          "`PlayerModeStatistics`(`PlayerId`,`ModeId`,`Matches`,`Wins`,`Losses`,`Draws`,`Bubbles`,`Pangs`,`Deaths`,`Helps`,`PlayedSeconds`,`ObjectivePoints`)"
          " VALUES($1,$2,1,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT(`PlayerId`,`ModeId`) DO UPDATE "
          "SET "
          "`Matches`=`PlayerModeStatistics`.`Matches`+1,`Wins`=`PlayerModeStatistics`.`Wins`+VALUES(`Wins`),`Losses`=`PlayerModeStatistics`.`Losses`+VALUES(`Losses`),`Draws`=`PlayerModeStatistics`.`Draws`+VALUES(`Draws`),`Bubbles`=`PlayerModeStatistics`.`Bubbles`+VALUES(`Bubbles`),`Pangs`=`PlayerModeStatistics`.`Pangs`+VALUES(`Pangs`),`Deaths`=`PlayerModeStatistics`.`Deaths`+VALUES(`Deaths`),`Helps`=`PlayerModeStatistics`.`Helps`+VALUES(`Helps`),`PlayedSeconds`=`PlayerModeStatistics`.`PlayedSeconds`+VALUES(`PlayedSeconds`),`ObjectivePoints`=`PlayerModeStatistics`.`ObjectivePoints`+VALUES(`ObjectivePoints`)",
          bf::infrastructure::mysql::Parameters{
              transaction_, write.result.player_id.Value(), write.mode_id, win,
              loss, draw, write.result.performance.bubbles,
              write.result.performance.pangs, write.result.performance.deaths,
              write.result.performance.helps, write.elapsed_seconds,
              write.result.performance.objective_points});

      const auto participant_changed = transaction_.Exec(
          "UPDATE `MatchParticipants` SET `ParticipantStatus`=$3,`Statistics`=$4,`LeftAt`=(SELECT "
          "`EndedAt` FROM `Matches` WHERE `MatchId`=$1) WHERE `MatchId`=$1 AND `PlayerId`=$2 "
          "RETURNING `PlayerId`",
          bf::infrastructure::mysql::Parameters{
              transaction_, match_id, write.result.player_id.Value(),
              participant->departed ? "disconnected" : "completed",
              statistics.dump()});
      if (participant_changed.size() != 1U) {
        return InvalidDatabaseState("终局玩家不属于开局参与者集合");
      }

      if (reward.applied_award.lucci > 0) {
        const auto grant = transaction_.Exec(
            "INSERT INTO "
            "`MatchRewardGrants`(`RewardGrantId`,`SettlementId`,`MatchId`,`PlayerId`,`RewardKey`,`CurrencyCode`,`Amount`,`GrantStatus`,`WalletTransactionId`,`GrantedAt`)"
            " "
            "VALUES(BIN_TO_UUID(UNHEX(MD5(CONCAT($1,':',$2,':lucci')))),$1,$1,$2,'lucci','lucci',$3,'granted',$4,CURRENT_TIMESTAMP(6))"
            " ON CONFLICT(`MatchId`,`PlayerId`,`RewardKey`) DO NOTHING RETURNING `RewardGrantId`",
            bf::infrastructure::mysql::Parameters{
                transaction_, match_id, write.result.player_id.Value(),
                reward.applied_award.lucci, *wallet_transaction_id});
        if (grant.size() != 1U) {
          return InvalidDatabaseState("同一玩家已存在冲突的对局奖励项");
        }
      }
      applied_players_.insert(write.result.player_id);
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("写入玩家结算");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("写入玩家结算", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "写入玩家结算时发生未分类错误");
    }
  }

  std::optional<application::ServiceError> CompleteSettlement(
      const application::MatchSettlementResult& result) override {
    if (!command_.has_value() || !(result.match_id == command_->match_id) ||
        result.players.size() != applied_players_.size() ||
        applied_players_.size() != command_->participants.size()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "结算结果尚未完整写入");
    }
    try {
      if (const auto error = SettleUserHostPrize(transaction_, *command_))
        return error;
      const auto match_id = result.match_id.ToUuidString();
      for (const auto& player : result.players) {
        if (!player.reward.applied_award.eligible) continue;
        const auto peer = transaction_.Exec(
            "SELECT `PartnerPlayerId` FROM `MatchCoupleBenefits` WHERE `MatchId`=$1 AND `PlayerId`=$2 "
            "AND `RingItemId`=101396",
            bf::infrastructure::mysql::Parameters{transaction_, match_id,
                                                  player.player_id.Value()});
        if (peer.empty() ||
            !std::ranges::any_of(result.players, [&](const auto& other) {
              return other.player_id.Value() ==
                         peer[0][0].As<std::uint64_t>() &&
                     other.reward.applied_award.eligible;
            }))
          continue;
        const auto claimed = transaction_.Exec(
            "INSERT INTO `CoupleDailyGrants`(`PlayerId`,`RewardDay`,`MatchId`,`CoupleId`) SELECT "
            "b.`PlayerId`,DATE((CONVERT_TZ(m.`StartedAt`,'+00:00','+08:00'))),b.`MatchId`,b.`CoupleId` "
            "FROM `MatchCoupleBenefits` b JOIN `Matches` m USING(`MatchId`) JOIN `CoupleMembers` cm ON "
            "cm.`PlayerId`=b.`PlayerId` AND cm.`CoupleId`=b.`CoupleId` JOIN `Couples` c ON "
            "c.`CoupleId`=b.`CoupleId` AND c.`RingItemId`=101396 WHERE b.`MatchId`=$1 AND "
            "b.`PlayerId`=$2 ON CONFLICT DO NOTHING",
            bf::infrastructure::mysql::Parameters{transaction_, match_id,
                                                  player.player_id.Value()});
        if (claimed.AffectedRows() == 0) continue;
        // Build752 Stock 1017262/3/4: ten bombs; each LOVE glasses variant
        // seven days. Both variants are granted: the original server's
        // variant-allocation table is unavailable.
        for (const auto item : {101383U, 101394U, 205056U}) {
          const bool consumable = item == 205056U;
          const auto old = transaction_.Exec(
              "SELECT `Quantity`,`ExpiresAt` FROM `PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 FOR "
              "UPDATE",
              bf::infrastructure::mysql::Parameters{
                  transaction_, player.player_id.Value(), item});
          const auto before = old.empty() ? 0 : old[0][0].As<std::int64_t>();
          const auto catalog = transaction_.Exec(
              "SELECT `MaxQuantity` FROM `ItemTemplates` WHERE `ItemId`=$1",
              bf::infrastructure::mysql::Parameters{transaction_, item});
          if (catalog.empty())
            return InvalidDatabaseState("情侣每日赠品模板未导入");
          const auto limit =
              std::min<std::int64_t>(domain::kNativeSignedDwordMaximum,
                                     catalog[0][0].As<std::int64_t>());
          const auto after =
              consumable ? std::max(before, std::min(limit, before + 10))
                         : std::max<std::int64_t>(before, 1);
          const auto expires_before =
              old.empty() || old[0][1].IsNull()
                  ? std::optional<std::string>{}
                  : std::optional{old[0][1].As<std::string>()};
          transaction_.Exec(
              "INSERT INTO "
              "`PlayerItems`(`PlayerId`,`ItemId`,`Quantity`,`AcquiredAt`,`ExpiresAt`,`SourceCode`) "
              "VALUES($1,$2,$3,CURRENT_TIMESTAMP(6),CASE WHEN $4 THEN NULL ELSE "
              "CURRENT_TIMESTAMP(6)+INTERVAL 7 DAY END,'couple_daily') ON CONFLICT(`PlayerId`,`ItemId`) "
              "DO UPDATE SET `Quantity`=VALUES(`Quantity`),`ExpiresAt`=CASE WHEN "
              "`PlayerItems`.`ExpiresAt` IS NULL THEN NULL WHEN $4 THEN NULL ELSE "
              "GREATEST(`PlayerItems`.`ExpiresAt`,VALUES(`ExpiresAt`)) END",
              bf::infrastructure::mysql::Parameters{transaction_,
                                                    player.player_id.Value(),
                                                    item, after, consumable});
          transaction_.Exec(
              "INSERT INTO "
              "`InventoryTransactions`(`PlayerId`,`ItemId`,`ItemInstanceId`,`QuantityDelta`,`ExpiresBefore`,`ExpiresAfter`,`ReasonCode`,`BusinessId`)"
              " SELECT `PlayerId`,`ItemId`,`ItemInstanceId`,$3,$5,`ExpiresAt`,'couple_daily',$4 FROM "
              "`PlayerItems` WHERE `PlayerId`=$1 AND `ItemId`=$2 AND ($3<>0 OR NOT (`ExpiresAt` <=> $5))",
              bf::infrastructure::mysql::Parameters{
                  transaction_, player.player_id.Value(), item, after - before,
                  match_id, expires_before});
        }
      }
      const auto completed = transaction_.Exec(
          "UPDATE `MatchSettlements` SET "
          "`SettlementStatus`='succeeded',`ResultSnapshot`=$2,`SettledAt`=CURRENT_TIMESTAMP(6),`ProcessingStartedAt`=NULL,`FailureCode`=NULL,`UpdatedAt`=CURRENT_TIMESTAMP(6)"
          " WHERE `MatchId`=$1 AND `SettlementStatus`='processing' RETURNING `SettlementId`",
          bf::infrastructure::mysql::Parameters{transaction_, match_id,
                                                ResultSnapshot(result).dump()});
      const auto match = transaction_.Exec(
          "UPDATE `Matches` SET `MatchStatus`='settled' WHERE `MatchId`=$1 AND "
          "`MatchStatus`='settlement_pending' RETURNING `MatchId`",
          bf::infrastructure::mysql::Parameters{transaction_, match_id});
      if (completed.size() != 1U || match.size() != 1U) {
        return InvalidDatabaseState("无法原子完成结算和对局终态");
      }
      settlement_completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("完成对局结算");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("完成对局结算", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "完成对局结算时发生未分类错误");
    }
  }

  application::ServiceResult<std::vector<application::MatchClanContribution>>
  ClanContributions() override {
    if (!command_)
      return MakeError(application::ErrorCode::kInvalidState, "尚未占用结算");
    try {
      const auto rows = transaction_.Exec(
          "SELECT "
          "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.player_id'))),cs.`ClanId`,coalesce(g.`Points`,0) "
          "FROM `MatchSettlements` s JOIN `ClanMatchSides` cs USING(`MatchId`) CROSS JOIN "
          "JSON_TABLE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.participants'), '$[*]' COLUMNS (Element "
          "JSON PATH '$')) p LEFT JOIN `ClanActivityGrants` g ON g.`SourceKind`='match' AND "
          "g.`SourceId`=s.`MatchId` AND "
          "g.`PlayerId`=(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.player_id'))) AND "
          "g.`ClanId`=cs.`ClanId` WHERE s.`MatchId`=$1 AND s.`SettlementStatus`='succeeded' AND "
          "JSON_UNQUOTE(JSON_EXTRACT(s.`TerminalSnapshot`,'$.completion_kind'))='completed' AND "
          "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.team_code')))=cs.`TeamCode` AND NOT "
          "coalesce((JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.departed'))),false) AND (g.`GrantId` IS "
          "NOT NULL OR (JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.outcome'))='loss' AND (SELECT count(*)"
          " FROM `MatchParticipants` mp WHERE mp.`MatchId`=s.`MatchId` AND "
          "mp.`TeamCode`=cs.`TeamCode`) IN(2,4))) ORDER BY "
          "(JSON_UNQUOTE(JSON_EXTRACT(p.Element,'$.player_id')))",
          bf::infrastructure::mysql::Parameters{
              transaction_, command_->match_id.ToUuidString()});
      std::vector<application::MatchClanContribution> result;
      for (const auto& row : rows)
        result.push_back({domain::PlayerId{row[0].As<std::uint64_t>()},
                          row[1].As<std::uint32_t>(),
                          row[2].As<std::uint32_t>()});
      return result;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("读取已提交战队贡献");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("读取已提交战队贡献", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取已提交战队贡献失败");
    }
  }

  application::ServiceResult<std::optional<application::UserHostPrizeReceipt>>
  UserHostPrize() override {
    if (!command_ || (rules_version_id_ != 0 && !settlement_completed_))
      return MakeError(application::ErrorCode::kInvalidState,
                       "主办奖金回执必须在结算成功后读取");
    try {
      return ReadUserHostPrizeReceipt(transaction_,
                                      command_->match_id.ToUuidString());
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("读取主办奖金回执");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("读取主办奖金回执", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "主办奖金回执或资产流水非法");
    }
  }

  application::ServiceResult<std::vector<domain::PlayerId>> CoupleDailyGifts()
      override {
    if (!command_)
      return MakeError(application::ErrorCode::kInvalidState, "尚未占用结算");
    try {
      std::vector<domain::PlayerId> result;
      for (
          const auto& row : transaction_.Exec(
              "SELECT `PlayerId` FROM `CoupleDailyGrants` WHERE `MatchId`=$1 ORDER BY `PlayerId`",
              bf::infrastructure::mysql::Parameters{
                  transaction_, command_->match_id.ToUuidString()}))
        result.push_back(domain::PlayerId{row[0].As<std::uint64_t>()});
      return result;
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("读取情侣赠品回执", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "读取情侣赠品回执失败");
    }
  }

  std::optional<application::ServiceError> Commit() override {
    if (completed_ || !command_.has_value()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "对局结算事务不能提交");
    }
    const bool replay = rules_version_id_ == 0;
    if (!replay && !settlement_completed_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "新对局结算尚未完成");
    }
    try {
      transaction_.Commit();
      completed_ = true;
      return std::nullopt;
    } catch (const bf::infrastructure::mysql::BrokenConnection&) {
      return ConnectionInterrupted("提交对局结算");
    } catch (const bf::infrastructure::mysql::SqlError& error) {
      return StorageError("提交对局结算", error);
    } catch (const std::exception&) {
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "提交对局结算时发生未分类错误");
    }
  }

 private:
  MySqlConnectionPool::Lease lease_;
  bf::infrastructure::mysql::WriteTransaction transaction_;
  std::optional<application::MatchSettlementCommand> command_;
  std::int64_t rules_version_id_{0};
  std::set<domain::PlayerId> locked_players_;
  std::set<domain::PlayerId> applied_players_;
  bool settlement_completed_{false};
  bool completed_{false};
};

}  // namespace

MySqlMatchStartUnitOfWork::MySqlMatchStartUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<std::unique_ptr<application::MatchStartTransaction>>
MySqlMatchStartUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::MatchStartTransaction>{
        std::make_unique<MySqlMatchStartTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始对局开局事务", true);
  }
}

MySqlMatchLifecycleRepairRepository::MySqlMatchLifecycleRepairRepository(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

std::optional<application::ServiceError>
MySqlMatchLifecycleRepairRepository::Interrupt(
    const domain::MatchId& match_id,
    std::chrono::system_clock::time_point interrupted_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  const auto match_uuid = match_id.ToUuidString();
  const auto ended_at = UnixMilliseconds(interrupted_at);
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    transaction.Exec(
        "SELECT LockKey FROM TransactionLocks WHERE LockKey=CONCAT('match_',LEFT(SHA2($1,256),1)) "
        "FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid});
    const auto locked = transaction.Exec(
        "SELECT `MatchStatus`,`RoomInstanceId` FROM `Matches` WHERE `MatchId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid});
    if (locked.empty()) {
      // Under the same start/cancel lock, absence proves no committed funding.
      // This is a failed/uncertain start compensation, not a match success.
      transaction.Commit();
      return std::nullopt;
    }
    if (!locked[0][1].IsNull())
      return MakeError(application::ErrorCode::kConflict,
                       "owned DS matches require scoped recovery");
    const auto status = locked[0][0].As<std::string_view>();
    if (status != "loading" && status != "playing" && status != "interrupted") {
      return MakeError(application::ErrorCode::kConflict,
                       "开局补偿不能覆盖已经进入其他终态的对局");
    }
    transaction.Exec(
        "UPDATE `MatchParticipants` SET "
        "`ParticipantStatus`='disconnected',`LeftAt`=GREATEST(`JoinedAt`,FROM_UNIXTIME($2/1000.0)) "
        "WHERE `MatchId`=$1 AND `ParticipantStatus`='joined'",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                              ended_at});
    transaction.Exec(
        "UPDATE `Matches` SET "
        "`MatchStatus`='interrupted',`EndedAt`=GREATEST(`StartedAt`,FROM_UNIXTIME($2/1000.0)) WHERE"
        " `MatchId`=$1 AND `MatchStatus` IN('loading','playing')",
        bf::infrastructure::mysql::Parameters{transaction, match_uuid,
                                              ended_at});
    RefundUserHostPrize(transaction, match_uuid);
    transaction.Commit();
    return std::nullopt;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return ConnectionInterrupted("补偿失败开局");
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return StorageError("补偿失败开局", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "补偿失败开局时发生未分类错误", true);
  }
}

application::ServiceResult<application::MatchLifecycleRecoveryReport>
MySqlMatchLifecycleRepairRepository::RecoverAbandoned(
    std::chrono::system_clock::time_point interrupted_at) {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  const auto ended_at = UnixMilliseconds(interrupted_at);
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const auto abandoned = transaction.Exec(
        "SELECT `MatchId` FROM `Matches` WHERE `MatchStatus` IN('loading','playing') AND "
        "`RoomInstanceId` IS NULL ORDER BY `MatchId` FOR UPDATE");
    const auto participants = transaction.Exec(
        "UPDATE `MatchParticipants` AS participant, `Matches` AS match_row SET "
        "participant.`ParticipantStatus`='disconnected', "
        "participant.`LeftAt`=GREATEST(participant.`JoinedAt`,FROM_UNIXTIME($1/1000.0)) WHERE "
        "participant.`MatchId`=match_row.`MatchId` AND match_row.`MatchStatus` "
        "IN('loading','playing') AND match_row.`RoomInstanceId` IS NULL AND "
        "participant.`ParticipantStatus`='joined'",
        bf::infrastructure::mysql::Parameters{transaction, ended_at});
    const auto matches = transaction.Exec(
        "UPDATE `Matches` SET "
        "`MatchStatus`='interrupted',`EndedAt`=GREATEST(`StartedAt`,FROM_UNIXTIME($1/1000.0)) WHERE"
        " `MatchStatus` IN('loading','playing') AND `RoomInstanceId` IS NULL",
        bf::infrastructure::mysql::Parameters{transaction, ended_at});
    if (matches.AffectedRows() != abandoned.size()) {
      return InvalidDatabaseState(
          "启动恢复锁定的活动对局数量在同一事务内发生变化");
    }
    // 启动恢复可能涉及多个主办者，先统一按玩家编号锁定，避免跨局退款逆序。
    const auto sponsors = transaction.Exec(
        "SELECT DISTINCT prize.`SponsorPlayerId` FROM `UserHostPrizes` prize JOIN `Matches`  AS "
        "`Match` USING(`MatchId`) WHERE prize.`PrizeStatus`='funded' AND "
        "`Match`.`MatchStatus`='interrupted' ORDER BY prize.`SponsorPlayerId`");
    for (const auto& sponsor : sponsors)
      transaction.Exec(
          "SELECT `PlayerId` FROM `PlayerProfiles` WHERE `PlayerId`=$1 FOR UPDATE",
          bf::infrastructure::mysql::Parameters{
              transaction, sponsor[0].As<std::uint64_t>()});
    for (const auto& match : abandoned)
      RefundUserHostPrize(transaction, match[0].As<std::string>());
    transaction.Commit();
    return application::MatchLifecycleRecoveryReport{
        .match_count = static_cast<std::size_t>(matches.AffectedRows()),
        .participant_count =
            static_cast<std::size_t>(participants.AffectedRows())};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return ConnectionInterrupted("恢复遗留活动对局");
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return StorageError("恢复遗留活动对局", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "恢复遗留活动对局时发生未分类错误", true);
  }
}

MySqlMatchSettlementJournal::MySqlMatchSettlementJournal(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<application::MatchSettlementStageReceipt>
MySqlMatchSettlementJournal::Stage(
    const application::MatchSettlementCommand& input) {
  if (const auto error = application::ValidateMatchSettlementCommand(input)) {
    return *error;
  }
  const auto command = NormalizedCommand(input);
  const auto snapshot = TerminalSnapshot(command);
  const auto match_id = command.match_id.ToUuidString();
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::WriteTransaction transaction{lease.Connection()};
    const bool durable =
        !transaction
             .Exec("SELECT 1 FROM `MatchSettlements` WHERE `MatchId`=$1",
                   bf::infrastructure::mysql::Parameters{transaction, match_id})
             .empty();
    if (!LockMatchOwnership(transaction, command.match_id, command.room_owner,
                            !durable))
      return MakeError(application::ErrorCode::kConflict,
                       "terminal room ownership expired or mismatched");
    const auto match = transaction.Exec(
        "SELECT "
        "`ModeId`,`MatchStatus`,(JSON_UNQUOTE(JSON_EXTRACT(`RulesSnapshot`,'$.reward_rules_version_id')))"
        " FROM `Matches` WHERE `MatchId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, match_id});
    if (match.size() != 1U) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "终局所属对局尚未持久化");
    }
    if (match[0][0].As<std::uint32_t>() != command.mode_id) {
      return MakeError(application::ErrorCode::kConflict,
                       "终局模式与开局快照不一致");
    }
    const auto participants = transaction.Exec(
        "SELECT `PlayerId`,`SeatIndex`,`TeamCode` FROM `MatchParticipants` WHERE `MatchId`=$1 ORDER"
        " BY `PlayerId`",
        bf::infrastructure::mysql::Parameters{transaction, match_id});
    if (participants.size() != command.participants.size()) {
      return MakeError(application::ErrorCode::kConflict,
                       "终局参与者数量与开局快照不一致");
    }
    for (std::size_t index = 0U; index < command.participants.size(); ++index) {
      const auto& expected = command.participants[index];
      const auto row = participants[static_cast<
          bf::infrastructure::mysql::QueryResult::size_type>(index)];
      if (row[0].As<std::uint64_t>() != expected.player_id.Value() ||
          row[1].As<std::uint16_t>() != expected.seat_index ||
          row[2].As<std::uint16_t>() != expected.team_code) {
        return MakeError(application::ErrorCode::kConflict,
                         "终局玩家、席位或队伍与开局快照不一致");
      }
    }
    if (match[0][1].As<std::string_view>() == "interrupted")
      return MakeError(application::ErrorCode::kConflict,
                       "已补偿中断的对局不能接受迟到终局");
    const auto reward_rules_version = match[0][2].As<std::int64_t>();
    const auto inserted = transaction.Exec(
        "INSERT INTO "
        "`MatchSettlements`(`SettlementId`,`MatchId`,`TerminalSnapshot`,`RewardRulesVersionId`) "
        "VALUES($1,$1,$2,$3) ON CONFLICT(`MatchId`) DO NOTHING RETURNING `SettlementId`",
        bf::infrastructure::mysql::Parameters{
            transaction, match_id, snapshot.dump(), reward_rules_version});
    const auto settlement = transaction.Exec(
        "SELECT (`TerminalSnapshot`=CAST($2 AS JSON)),`RewardRulesVersionId` FROM `MatchSettlements` WHERE "
        "`MatchId`=$1 FOR UPDATE",
        bf::infrastructure::mysql::Parameters{transaction, match_id,
                                              snapshot.dump()});
    if (settlement.size() != 1U || !settlement[0][0].As<bool>() ||
        settlement[0][1].As<std::int64_t>() != reward_rules_version) {
      return MakeError(application::ErrorCode::kConflict,
                       "对局已保存不同的不可变终局快照");
    }
    if (match[0][1].As<std::string_view>() != "settled") {
      const auto updated = transaction.Exec(
          "UPDATE `Matches` SET `MatchStatus`='settlement_pending',`EndedAt`=`StartedAt`+INTERVAL $2 "
          "SECOND WHERE `MatchId`=$1 RETURNING `MatchId`",
          bf::infrastructure::mysql::Parameters{transaction, match_id,
                                                command.elapsed_seconds});
      if (updated.size() != 1U) {
        return InvalidDatabaseState("无法把对局推进到待结算状态");
      }
    }
    transaction.Commit();
    return application::MatchSettlementStageReceipt{
        .match_id = command.match_id,
        .state =
            inserted.empty()
                ? application::MatchSettlementStageState::kExistingIdentical
                : application::MatchSettlementStageState::kCreated};
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return ConnectionInterrupted("保存对局终局日志");
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return StorageError("保存对局终局日志", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "保存对局终局日志时发生未分类错误");
  }
}

application::ServiceResult<std::vector<application::MatchSettlementCommand>>
MySqlMatchSettlementJournal::LoadRecoverable(std::size_t limit) {
  if (limit == 0U || limit > 4'096U) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "结算恢复批量必须为 1 至 4096");
  }
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    bf::infrastructure::mysql::ReadTransaction transaction{lease.Connection()};
    const auto rows = transaction.Exec(
        "SELECT `TerminalSnapshot` FROM `MatchSettlements` WHERE `SettlementStatus` "
        "IN('pending','failed_retryable') OR (`SettlementStatus`='processing' AND "
        "`ProcessingStartedAt` < CURRENT_TIMESTAMP(6)-INTERVAL 5 MINUTE) ORDER BY "
        "`CreatedAt`,`MatchId` LIMIT $1",
        bf::infrastructure::mysql::Parameters{transaction, limit});
    std::vector<application::MatchSettlementCommand> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
      auto decoded = CommandFromSnapshot(row[0].As<std::string>());
      if (const auto* error =
              std::get_if<application::ServiceError>(&decoded)) {
        return *error;
      }
      result.push_back(
          std::get<application::MatchSettlementCommand>(std::move(decoded)));
    }
    transaction.Commit();
    return result;
  } catch (const bf::infrastructure::mysql::BrokenConnection&) {
    return ConnectionInterrupted("读取可恢复对局结算");
  } catch (const bf::infrastructure::mysql::SqlError& error) {
    return StorageError("读取可恢复对局结算", error);
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "读取可恢复对局结算时发生未分类错误");
  }
}

MySqlMatchSettlementUnitOfWork::MySqlMatchSettlementUnitOfWork(
    MySqlConnectionPool& pool) noexcept
    : pool_(pool) {}

application::ServiceResult<
    std::unique_ptr<application::MatchSettlementTransaction>>
MySqlMatchSettlementUnitOfWork::Begin() {
  auto lease_result = pool_.Acquire();
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  try {
    auto lease = std::get<MySqlConnectionPool::Lease>(std::move(lease_result));
    return std::unique_ptr<application::MatchSettlementTransaction>{
        std::make_unique<MySqlMatchSettlementTransaction>(std::move(lease))};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kStorageUnavailable,
                     "无法开始对局结算事务", true);
  }
}

}  // namespace bf::infrastructure::mysql
