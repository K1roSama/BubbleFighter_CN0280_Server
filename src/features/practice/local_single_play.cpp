#include "features/practice/local_single_play.h"

#include <algorithm>
#include <limits>

#include "protocol/battle/match_transition_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/codec/text_codec.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::runtime {
namespace {
using application::ErrorCode;
using application::ServiceResult;

application::ServiceError Error(ErrorCode code, std::string text) {
  return {.code = code,
          .safe_message = std::move(text),
          .correlation_id = {},
          .is_retryable = false};
}

constexpr std::uint32_t kActor = 1'000'000U;
constexpr std::uint16_t kScene = 65'534U;
constexpr auto kLoadLimit = std::chrono::seconds{90};
constexpr auto kReportGrace =
    std::chrono::seconds{15};  // 本服传输容差，不改客户端目标计时。
constexpr std::string_view kProfileHash =
    "f24d6c78efd8ebe3adffb1687f9237948a1b67b612215be0207079c2d925413a";
constexpr SinglePlayMission kMissions[]{
#include "features/practice/generated/single_play_missions.inc"
};

protocol::SceneSessionSnapshot Scene(
    const application::PlayerMenuSnapshot& player,
    const SinglePlayMission& mission) {
  protocol::SceneConfiguration config;
  config.mode_id = 15U;
  config.family_key = mission.mission_key;
  config.map_key = mission.directory;
  config.display_name = "SinglePlay";
  config.field_316 = 0U;
  config.field_319 = 0U;
  config.field_320 = 0U;
  config.field_408 = 0U;
  return {.scene = {.scene_id = kScene,
                    .kind = 2U,
                    .name = "SinglePlay",
                    .occupants = 1U,
                    .observers = 0U,
                    .capacity = 1U,
                    .locked = 0U,
                    .field_40 = {},
                    .scene_type = 2U,
                    .configuration = std::move(config),
                    .key = player.player_id.Value()},
          .host_slot = 0U,
          .selected_slot = 0U,
          .slots = {{.slot = 0U,
                     .team = 2U,
                     .occupied = true,
                     .locked = false,
                     .ready = false,
                     .player = player}},
          .include_equipment_records = true};
}
}  // namespace

std::span<const SinglePlayMission> SinglePlayMissions() noexcept {
  return kMissions;
}

const SinglePlayMission* FindSinglePlayMission(std::string_view key) noexcept {
  for (const auto& row : kMissions)
    if (row.mission_key == key) return &row;
  return nullptr;
}

ServiceResult<LocalTutorialUpdate> LocalSinglePlayCoordinator::Begin(
    application::PlayerMenuSnapshot snapshot, const protocol::Packet& request,
    const domain::MatchId& id, Time now) {
  if (request.opcode != 107U)
    return Error(ErrorCode::kUnsupportedOpcode,
                 "SinglePlay requires native107");
  const auto entry = protocol::DecodeTutorialStartReport(request.body);
  const auto key = protocol::Utf16ToUtf8(entry.mission_key);
  const auto* mission = FindSinglePlayMission(key);
  if (!mission ||
      protocol::Utf16ToUtf8(entry.relation_key) != mission->directory ||
      entry.trailing_value != mission->difficulty || entry.kind != 0x10U)
    return Error(ErrorCode::kPermissionDenied,
                 "SinglePlay mission/Directory/Difficulty mismatch");
  if (!snapshot.player_id.IsValid() ||
      snapshot.player_id.Value() > std::numeric_limits<std::uint32_t>::max())
    return Error(ErrorCode::kInvalidInput,
                 "SinglePlay requires legacy authenticated player");
  const auto player = snapshot.player_id;
  if (Contains(player) && !Waiting(player))
    return Error(ErrorCode::kConflict,
                 "SinglePlay attempt is active or awaiting SQL");
  auto menu = protocol::EncodeMenuStateResponse(snapshot);
  auto room = protocol::EncodeSceneSessionResponse(Scene(snapshot, *mission));
  LocalTutorialUpdate update;
  update.deliveries = {
      {player, {112U, room}},
      {player,
       {510U, protocol::EncodeStartSceneResponse(
                  {.map_key = std::string{mission->directory},
                   .mode_key = std::string{mission->mission_key}})}}};
  update.phase = LocalTutorialPhase::kLoading;
  update.persistence = application::TutorialSessionStart{
      .session_id = id,
      .player_id = player,
      .profile_hash = std::string{kProfileHash},
      .mode_id = 15U,
      .mission_key = key,
      .directory = std::string{mission->directory},
      .difficulty = mission->difficulty,
      .stage_key = mission->stage_key};
  if (Waiting(player))
    waiting_rollback_.insert_or_assign(player, sessions_.at(player));
  sessions_.insert_or_assign(
      player, Session{.id = id,
                      .mission = mission,
                      .menu = std::move(menu),
                      .scene = std::move(room),
                      .loading_at = now,
                      .clears = std::move(snapshot.single_play_clears)});
  return update;
}

ServiceResult<LocalTutorialUpdate> LocalSinglePlayCoordinator::Handle(
    domain::PlayerId player, const protocol::Packet& packet, Time now) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end())
    return Error(ErrorCode::kInvalidState, "no SinglePlay scene");
  auto& session = found->second;
  if (packet.opcode == 32840U) {
    const auto probe = protocol::DecodeMatchLatencyProbe(packet.body);
    LocalTutorialUpdate reply;
    if (probe.slot == 0U)
      reply.deliveries.push_back(
          {player, {32840U, protocol::EncodeMatchLatencyProbe(probe)}});
    return reply;
  }
  if (packet.opcode == 105U || packet.opcode == 404U) {
    if (packet.opcode == 105U)
      static_cast<void>(protocol::DecodeReturnToMenuRequest(packet.body));
    else
      static_cast<void>(protocol::DecodeLeaveRoomRequest(packet.body));
    if (session.stage == Stage::kWaiting) {
      LocalTutorialUpdate update;
      update.phase = LocalTutorialPhase::kMenu;
      update.release_owner = session.id;
      update.deliveries.push_back({player, {112U, session.menu}});
      sessions_.erase(found);
      return update;
    }
    if (session.stage == Stage::kFinishing) return LocalTutorialUpdate{};
    return Finish(player, session, false, "client_leave", now, false, true);
  }
  if (packet.opcode == 725U) {
    const bool clear =
        protocol::DecodeSpecialModeFinishClaim(packet.body).value == 1U;
    if (session.stage == Stage::kWaiting) {
      if (session.terminal && std::get<application::TutorialSessionFinish>(
                                  *session.terminal->persistence)
                                      .reported_clear == clear)
        return LocalTutorialUpdate{};
      return Error(ErrorCode::kConflict,
                   "SinglePlay reported terminal is immutable");
    }
    if (session.stage == Stage::kFinishing) {
      if (session.terminal && std::get<application::TutorialSessionFinish>(
                                  *session.terminal->persistence)
                                      .reported_clear == clear)
        return *session.terminal;
      return Error(ErrorCode::kConflict,
                   "SinglePlay finish replay changed outcome");
    }
    if (!session.committed || session.stage != Stage::kPlaying ||
        !session.playing_at || now < *session.playing_at)
      return Error(ErrorCode::kInvalidState,
                   "SinglePlay report requires committed Playing");
    if (now >= *session.playing_at +
                   std::chrono::milliseconds{session.mission->duration_ms} +
                   kReportGrace)
      return Finish(player, session, false, "report_timeout", now, true, true);
    return Finish(player, session, clear,
                  clear ? "client_reported_clear" : "client_reported_failure",
                  now, true, true);
  }
  if (packet.opcode == 709U || packet.opcode == 702U || packet.opcode == 701U) {
    if (packet.opcode == 709U) {
      if (protocol::DecodePlayerObjectIdentityRequest(packet.body).object_id !=
          kActor)
        return Error(ErrorCode::kPermissionDenied,
                     "SinglePlay local actor must be seat zero");
    } else if (packet.opcode == 702U)
      static_cast<void>(protocol::DecodeMapAssetsRequest(packet.body));
    else
      static_cast<void>(protocol::DecodeLoadingCompleteRequest(packet.body));
    if (!session.committed)
      return Error(ErrorCode::kInvalidState, "SinglePlay begin awaits SQL");
    if (session.stage != Stage::kLoading) return LocalTutorialUpdate{};
    if (now >= session.loading_at + kLoadLimit)
      return Finish(player, session, false, "loading_timeout", now, true, true);
    if (now < session.loading_at)
      return Error(ErrorCode::kInvalidState, "SinglePlay clock rollback");
    if (packet.opcode == 709U) session.object = true;
    if (packet.opcode == 702U) session.assets = true;
    if (packet.opcode == 701U) session.loaded = true;
    if (!session.object || !session.assets || !session.loaded)
      return LocalTutorialUpdate{};
    session.stage = Stage::kPlaying;
    session.playing_at = now;
    LocalTutorialUpdate update;
    update.phase = LocalTutorialPhase::kPlaying;
    update.deliveries = {
        {player, {701U, protocol::EncodeActivateSlotResponse(0U)}},
        {player, {702U, protocol::EncodeLoadingOptionsResponse({})}},
        {player, {703U, protocol::EncodeCountdownResponse(0U)}},
        {player,
         {704U, protocol::EncodeMatchBeginResponse(
                    {.duration_ms = session.mission->duration_ms,
                     .scene_id = kScene})}}};
    return update;
  }
  if (packet.opcode == 925U) {
    protocol::ValidatePveEvent(packet.opcode, packet.body);
    return LocalTutorialUpdate{};
  }
  if (packet.opcode >= 0x8000U) {
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
    return LocalTutorialUpdate{};  // 原single-local动作没有网络NPC/伤害/纪录权限。
  }
  return Error(ErrorCode::kUnsupportedOpcode,
               "unsupported SinglePlay operation");
}

LocalTutorialUpdate LocalSinglePlayCoordinator::Finish(
    domain::PlayerId player, Session& session, bool clear, std::string reason,
    Time now, bool stay_in_room, bool deliver) {
  if (session.terminal) return {};
  std::optional<std::uint32_t> elapsed;
  if (session.playing_at && now >= *session.playing_at) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - *session.playing_at)
                        .count();
    elapsed = static_cast<std::uint32_t>(
        std::min<std::int64_t>(ms, std::numeric_limits<std::uint32_t>::max()));
  }
  LocalTutorialUpdate update;
  update.persistence =
      application::TutorialSessionFinish{.session_id = session.id,
                                         .player_id = player,
                                         .reported_clear = clear,
                                         .reason = std::move(reason),
                                         .mode_id = 15U,
                                         .elapsed_ms = elapsed};
  update.phase = stay_in_room ? LocalTutorialPhase::kSingleWaiting
                              : LocalTutorialPhase::kMenu;
  if (deliver) {
    if (clear && elapsed) {
      const auto key = session.mission->stage_key;
      const auto seconds = *elapsed / 1000U;
      auto found = std::ranges::find(
          session.clears, key, &application::SinglePlayClearView::stage_key);
      if (found == session.clears.end()) {
        session.clears.push_back(
            {.stage_key = key, .observed_seconds = seconds});
        std::ranges::sort(session.clears, {},
                          &application::SinglePlayClearView::stage_key);
      } else {
        found->observed_seconds = std::min(found->observed_seconds, seconds);
      }
      update.deliveries.push_back(
          {player,
           {924U, protocol::EncodeSinglePlayClearsResponse(session.clears)}});
    }
    update.deliveries.push_back(
        {player, {736U, {}}});  // 原B47720会请求Room stage4。
    if (!stay_in_room)
      update.deliveries.push_back({player, {112U, session.menu}});
  }
  session.stage = Stage::kFinishing;
  session.leave_after_finish = !stay_in_room;
  session.terminal = update;
  return update;
}

LocalTutorialUpdate LocalSinglePlayCoordinator::Disconnect(
    domain::PlayerId player) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end()) return {};
  auto& s = found->second;
  if (s.stage == Stage::kWaiting) {
    LocalTutorialUpdate update;
    update.release_owner = s.id;
    sessions_.erase(found);
    return update;
  }
  if (s.terminal) {
    s.leave_after_finish = true;
    s.terminal->deliveries.clear();
    return {};
  }
  return Finish(player, s, false, "disconnect",
                s.playing_at.value_or(s.loading_at), false, false);
}

bool LocalSinglePlayCoordinator::Acknowledge(domain::PlayerId player,
                                             const domain::MatchId& id,
                                             bool begin) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end() || found->second.id != id) return false;
  auto& s = found->second;
  if (begin) {
    if (s.committed || s.stage != Stage::kLoading) return false;
    s.committed = true;
    waiting_rollback_.erase(player);
    return true;
  }
  if (s.stage != Stage::kFinishing || !s.terminal) return false;
  if (s.leave_after_finish)
    sessions_.erase(found);
  else
    s.stage = Stage::kWaiting;
  return true;
}

bool LocalSinglePlayCoordinator::BeginFailed(domain::PlayerId player,
                                             const domain::MatchId& id) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end() || found->second.id != id ||
      found->second.committed)
    return false;
  if (const auto previous = waiting_rollback_.find(player);
      previous != waiting_rollback_.end()) {
    found->second = std::move(previous->second);
    waiting_rollback_.erase(previous);
  } else
    sessions_.erase(found);
  return true;
}

bool LocalSinglePlayCoordinator::Contains(domain::PlayerId player) const {
  return sessions_.contains(player);
}

bool LocalSinglePlayCoordinator::Waiting(domain::PlayerId player) const {
  const auto i = sessions_.find(player);
  return i != sessions_.end() && i->second.stage == Stage::kWaiting;
}

std::optional<domain::MatchId> LocalSinglePlayCoordinator::SessionId(
    domain::PlayerId player) const {
  const auto i = sessions_.find(player);
  return i == sessions_.end() ? std::nullopt : std::optional{i->second.id};
}

std::optional<LocalSinglePlayCoordinator::Time>
LocalSinglePlayCoordinator::Deadline() const {
  std::optional<Time> first;
  for (const auto& [p, s] : sessions_) {
    static_cast<void>(p);
    if (!s.committed) continue;
    const auto due =
        s.stage == Stage::kLoading ? std::optional{s.loading_at + kLoadLimit}
        : s.stage == Stage::kPlaying
            ? std::optional{*s.playing_at +
                            std::chrono::milliseconds{s.mission->duration_ms} +
                            kReportGrace}
            : std::nullopt;
    if (due && (!first || *due < *first)) first = due;
  }
  return first;
}

std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>>
LocalSinglePlayCoordinator::Tick(Time now) {
  std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>> updates;
  for (auto& [p, s] : sessions_) {
    if (!s.committed) continue;
    if (s.stage == Stage::kLoading && now >= s.loading_at + kLoadLimit)
      updates.emplace_back(
          p, Finish(p, s, false, "loading_timeout", now, true, true));
    else if (s.stage == Stage::kPlaying &&
             now >= *s.playing_at +
                        std::chrono::milliseconds{s.mission->duration_ms} +
                        kReportGrace)
      updates.emplace_back(
          p, Finish(p, s, false, "report_timeout", now, true, true));
  }
  return updates;
}
}  // namespace bf::runtime
