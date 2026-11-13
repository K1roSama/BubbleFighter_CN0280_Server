#include "network/room_value_codec.h"

#include <chrono>
#include <set>
#include <stdexcept>

namespace nlohmann {
template <typename T>
struct adl_serializer<std::optional<T>> {
  static void to_json(json& out, const std::optional<T>& value) {
    if (value)
      out = *value;
    else
      out = nullptr;
  }

  static void from_json(const json& in, std::optional<T>& value) {
    if (in.is_null())
      value.reset();
    else
      value = in.get<T>();
  }
};

template <>
struct adl_serializer<std::chrono::system_clock::time_point> {
  static void to_json(json& out,
                      const std::chrono::system_clock::time_point& value) {
    out = std::chrono::duration_cast<std::chrono::milliseconds>(
              value.time_since_epoch())
              .count();
  }

  static void from_json(const json& in,
                        std::chrono::system_clock::time_point& value) {
    const auto milliseconds = in.get<std::int64_t>();
    // Bound before converting milliseconds to the platform clock's finer
    // duration.
    if (milliseconds < 0 || milliseconds > 4'102'444'800'000LL)
      throw std::invalid_argument{
          "internal UTC timestamp outside supported range"};
    value = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{milliseconds}};
  }
};
}  // namespace nlohmann

namespace bf::domain {
template <typename Tag>
void to_json(nlohmann::json& out, const Identifier<Tag>& id) {
  out = id.Value();
}

template <typename Tag>
void from_json(const nlohmann::json& in, Identifier<Tag>& id) {
  if (!in.is_number_unsigned())
    throw std::invalid_argument{"internal identifier must be unsigned"};
  id = Identifier<Tag>{in.get<std::uint64_t>()};
}

void to_json(nlohmann::json& out, const MatchId& id) { out = id.Bytes(); }
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NativeItemAttribute, source_item_id,
                                   definition_id, values, category, label)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchPerformance, bubbles, pangs, deaths,
                                   helps, loot_lucci, objective_points,
                                   pve_units)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CurrentRewardBalances, experience, lucci,
                                   rating)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchRewardAward, eligible, experience,
                                   lucci, rating, effective_outcome)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RewardProgressSnapshot, experience, level,
                                   level_base, level_next)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AppliedMatchReward, before, after,
                                   applied_award, progression)
}  // namespace bf::domain

namespace bf::application {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlayerProgressView, level,
                                                experience, level_base,
                                                level_next)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlayerWalletView, lucci,
                                                bubble_cash, treasure_bubble)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PetGrowthView, level, experience, fed_until)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlayerItemView, item_instance_id, item_id,
                                   quantity, acquired_at, expires_at, slot_code,
                                   equipped, pet_growth, attributes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WeaponPresetView, enabled, primary,
                                   secondary)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SinglePlayClearView, stage_key,
                                   observed_seconds)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlayerModeStatisticsView, mode_id, matches,
                                   wins, losses, draws, bubbles, pangs, helps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlayerMedalProgressView, medal_key, count)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CoupleProfileView, partner_id,
                                   partner_nickname, ring_item_id, since)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    PlayerMenuSnapshot, player_id, nickname, initial_fighter, progress, wallets,
    items, observed_at, clan_mark, color_code, active_weapon_preset,
    weapon_presets, login_name, clan_name, rating, level_icon_type, clan_id,
    mode_statistics, couple, medal_progress, expired_items, single_play_clears)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchSettlementPlayerResult, player_id,
                                   performance, effective_outcome, rank, reward)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MatchClanContribution, player_id, clan_id,
                                   points)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UserHostPrizeClaimReceipt, player_id, amount,
                                   credited_amount, ticket_due, ticket_credited)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UserHostPrizeWalletReceipt, player_id,
                                   wallets)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UserHostPrizeReceipt, sponsor_player_id,
                                   amount, fee_amount, policy_version, refunded,
                                   claims, wallets)

void to_json(nlohmann::json& out, const MatchSettlementParticipant& value) {
  out = {{"player_id", value.player_id}, {"seat_index", value.seat_index},
         {"team_code", value.team_code}, {"performance", value.performance},
         {"outcome", value.outcome},     {"rank", value.rank},
         {"departed", value.departed}};
}

void to_json(nlohmann::json& out, const MatchSettlementCommand& value) {
  out = {{"match_id", value.match_id},
         {"mode_id", value.mode_id},
         {"elapsed_seconds", value.elapsed_seconds},
         {"completion_kind", value.completion_kind},
         {"participants", value.participants}};
  if (value.room_owner)
    out["room_owner"] = {{"instance_id", value.room_owner->instance_id.Bytes()},
                         {"owner_epoch", value.room_owner->owner_epoch}};
}
}  // namespace bf::application

namespace bf::runtime {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NativeTeamSpawnRange, first, last)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NativeTeamSpawnPose, point, position,
                                   direction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    CombatParticipant, player_id, actor_object_id, team_code, weapon_templates,
    ordinary_respawn_allowed, seat_index, lucci_pouch, chocolate_box, fighter,
    equipment_items, ordinary_spawn_point, ordinary_spawn_range,
    ordinary_spawn_poses)
}  // namespace bf::runtime

namespace bf::infrastructure::network {
using Json = nlohmann::json;

namespace {
domain::MatchId ReadMatchId(const Json& value) {
  if (!value.is_array() || value.size() != 16U)
    throw std::invalid_argument{"invalid match id"};
  domain::MatchId::ByteArray bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto byte = value.at(index).get<unsigned>();
    if (byte > 255U) throw std::invalid_argument{"invalid match id byte"};
    bytes[index] = static_cast<std::uint8_t>(byte);
  }
  const auto id = domain::MatchId::FromBytes(bytes);
  if (!id) throw std::invalid_argument{"zero match id"};
  return *id;
}
}  // namespace

Json ParseRoomPayload(std::span<const std::uint8_t> payload) {
  if (payload.size() > protocol::internal::kMaximumRoomPayloadBytes)
    throw std::invalid_argument{"room payload exceeds limit"};
  std::vector<std::set<std::string>> keys;
  auto result = Json::parse(
      payload.begin(), payload.end(),
      [&keys](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 32)
          throw std::invalid_argument{"room JSON nesting exceeds limit"};
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !keys.back().insert(parsed.get<std::string>()).second)
          throw std::invalid_argument{"duplicate room JSON key"};
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
  if (!result.is_object())
    throw std::invalid_argument{"room payload must be an object"};
  return result;
}

application::PlayerMenuSnapshot ReadRoomPlayer(const Json& value) {
  auto result = value.get<application::PlayerMenuSnapshot>();
  if (!result.player_id.IsValid() || result.items.size() > 4096U ||
      result.expired_items.size() > 4096U || result.nickname.size() > 256U)
    throw std::invalid_argument{"invalid room player snapshot"};
  return result;
}

Json WriteRoomPlayer(const application::PlayerMenuSnapshot& value) {
  return value;
}

runtime::RoomMatchInstallation ReadRoomInstallation(const Json& value) {
  runtime::RoomMatchInstallation result{
      .match_id = ReadMatchId(value.at("match_id")),
      .score_model = value.at("score_model").get<domain::MatchScoreModel>(),
      .started_at = std::chrono::steady_clock::now(),
      .participants = value.at("participants")
                          .get<std::vector<runtime::CombatParticipant>>()};
  if (result.participants.empty() || result.participants.size() > 16U)
    throw std::invalid_argument{"invalid combat participant count"};
  return result;
}

Json WriteRoomInstallation(const runtime::RoomMatchInstallation& value) {
  if (!value.match_id)
    throw std::invalid_argument{"dedicated installation requires match id"};
  return {{"match_id", *value.match_id},
          {"score_model", value.score_model},
          {"participants", value.participants}};
}

application::MatchSettlementResult ReadRoomSettlement(const Json& value) {
  return {
      .match_id = ReadMatchId(value.at("match_id")),
      .mode_id = value.at("mode_id").get<std::uint32_t>(),
      .elapsed_seconds = value.at("elapsed_seconds").get<std::uint32_t>(),
      .completion_kind =
          value.at("completion_kind").get<application::MatchCompletionKind>(),
      .players =
          value.at("players")
              .get<std::vector<application::MatchSettlementPlayerResult>>(),
      .clan_contributions =
          value.at("clan_contributions")
              .get<std::vector<application::MatchClanContribution>>(),
      .user_host_prize =
          value.at("user_host_prize")
              .get<std::optional<application::UserHostPrizeReceipt>>(),
      .couple_daily_gift_players = value.at("couple_daily_gift_players")
                                       .get<std::vector<domain::PlayerId>>()};
}

Json WriteRoomSettlement(const application::MatchSettlementResult& value) {
  return {{"match_id", value.match_id},
          {"mode_id", value.mode_id},
          {"elapsed_seconds", value.elapsed_seconds},
          {"completion_kind", value.completion_kind},
          {"players", value.players},
          {"clan_contributions", value.clan_contributions},
          {"user_host_prize", value.user_host_prize},
          {"couple_daily_gift_players", value.couple_daily_gift_players}};
}

application::MatchSettlementCommand ReadRoomTerminal(const Json& value) {
  application::MatchSettlementCommand command{
      .match_id = ReadMatchId(value.at("match_id")),
      .mode_id = value.at("mode_id").get<std::uint32_t>(),
      .elapsed_seconds = value.at("elapsed_seconds").get<std::uint32_t>(),
      .completion_kind =
          value.at("completion_kind").get<application::MatchCompletionKind>()};
  const auto& participants = value.at("participants");
  if (!participants.is_array() || participants.size() > 16U)
    throw std::invalid_argument{"invalid terminal participant count"};
  for (const auto& row : participants)
    command.participants.push_back(
        {.player_id = row.at("player_id").get<domain::PlayerId>(),
         .seat_index = row.at("seat_index").get<std::uint8_t>(),
         .team_code = row.at("team_code").get<std::uint8_t>(),
         .performance = row.at("performance").get<domain::MatchPerformance>(),
         .outcome = row.at("outcome").get<domain::MatchOutcome>(),
         .rank = row.at("rank").get<std::uint32_t>(),
         .departed = row.at("departed").get<bool>()});
  if (value.contains("room_owner")) {
    const auto& owner = value.at("room_owner");
    command.room_owner = application::RoomOwnership{
        ReadMatchId(owner.at("instance_id")),
        owner.at("owner_epoch").get<std::uint64_t>()};
  }
  if (application::ValidateMatchSettlementCommand(command))
    throw std::invalid_argument{"invalid terminal business contract"};
  return command;
}

runtime::RoomCoordinatorResult ReadRoomResult(const Json& value) {
  runtime::RoomCoordinatorResult result;
  const auto event = value.value("event", 0U);
  if (event > static_cast<unsigned>(runtime::RoomEvent::kCombatTickDue))
    throw std::invalid_argument{"invalid room event"};
  result.event = static_cast<runtime::RoomEvent>(event);
  const auto& deliveries = value.at("deliveries");
  const auto& updates = value.at("session_updates");
  if (!deliveries.is_array() || deliveries.size() > 4096U ||
      !updates.is_array() || updates.size() > 36U)
    throw std::invalid_argument{"invalid room result batch"};
  for (const auto& row : deliveries) {
    const auto opcode = row.at("opcode").get<unsigned>();
    const auto& body = row.at("body");
    if (opcode > 65535U || !body.is_array() || body.size() > 65535U)
      throw std::invalid_argument{"invalid room delivery"};
    runtime::OutboundMessage message{.opcode =
                                         static_cast<std::uint16_t>(opcode)};
    for (const auto& byte : body) {
      const auto number = byte.get<unsigned>();
      if (number > 255U) throw std::invalid_argument{"invalid delivery byte"};
      message.body.push_back(static_cast<std::uint8_t>(number));
    }
    result.deliveries.push_back(
        {domain::PlayerId{row.at("player_id").get<std::uint64_t>()},
         std::move(message)});
  }
  for (const auto& row : updates) {
    const auto stage = row.at("stage").get<unsigned>();
    if (stage > static_cast<unsigned>(runtime::RoomSessionStage::kResults))
      throw std::invalid_argument{"invalid room stage"};
    result.session_updates.push_back(
        {domain::PlayerId{row.at("player_id").get<std::uint64_t>()},
         static_cast<runtime::RoomSessionStage>(stage)});
  }
  if (value.contains("settlement") && !value.at("settlement").is_null())
    result.settlement_command = ReadRoomTerminal(value.at("settlement"));
  return result;
}

Json WriteRoomResult(const runtime::RoomCoordinatorResult& result) {
  Json out{{"ok", true},
           {"event", result.event},
           {"deliveries", Json::array()},
           {"session_updates", Json::array()},
           {"settlement", nullptr}};
  for (const auto& delivery : result.deliveries)
    out["deliveries"].push_back({{"player_id", delivery.recipient.Value()},
                                 {"opcode", delivery.message.opcode},
                                 {"body", delivery.message.body}});
  for (const auto& update : result.session_updates)
    out["session_updates"].push_back(
        {{"player_id", update.player_id.Value()}, {"stage", update.stage}});
  if (result.settlement_command) out["settlement"] = *result.settlement_command;
  if (result.combat_reject_reason)
    out["combat_reject_reason"] = *result.combat_reject_reason;
  if (result.rule_rejection_detail)
    out["rule_rejection_detail"] = *result.rule_rejection_detail;
  return out;
}

Json WriteRoomError(const application::ServiceError& error) {
  return {{"ok", false},
          {"code", error.code},
          {"detail", error.safe_message},
          {"retryable", error.is_retryable}};
}
}  // namespace bf::infrastructure::network
