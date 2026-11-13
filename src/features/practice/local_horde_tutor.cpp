/**
 * @file local_horde_tutor.cpp
 * @brief 唯一本地prologue的加载、单波CLIENT_REPORTED声明与SQL提交门控。
 */
#include "features/practice/local_horde_tutor.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include "features/battle/match_objectives.h"
#include "features/battle/pirate_map_agents.h"
#include "features/practice/horde_tutor_match.h"
#include "features/practice/horde_tutor_profile.h"
#include "protocol/battle/match_transition_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::runtime {
namespace {
using application::ErrorCode;
using application::ServiceError;
using application::ServiceResult;

// Immutable evidence profile selected for this implementation, not a
// client-supplied hash.
// docs/research/game_modes_followup_20261003/single_horde/horde_tutor_profile.json
constexpr std::string_view kProfileHash =
    "a8fcfd326f4d17b840345a4199a648843fd100905e7861d2d9f2c3c267acfb1e";
constexpr std::uint16_t kPrivateScene = 65'535U;
constexpr std::uint32_t kLocalObject = 1'000'000U;
constexpr auto kLoadingTimeout = std::chrono::seconds{90};
constexpr auto kMinimumClearElapsed =
    std::chrono::seconds{8};  // Native5s + client preclear3s.
constexpr auto kFinalRequestDelay = std::chrono::seconds{3};

ServiceError Error(ErrorCode code, std::string_view message) {
  return {.code = code,
          .safe_message = std::string{message},
          .correlation_id = {},
          .is_retryable = false};
}

std::vector<std::uint8_t> WaveBody() {
  protocol::PacketWriter writer;
  writer.WriteU32(1U);
  return writer.Release();
}

LocalHordeTutorCoordinator::Time ClearAckDue(
    LocalHordeTutorCoordinator::Time start) {
  std::uint32_t tail_ms = 0U;
  for (const auto& row : HordeTutorSpawns())
    tail_ms = std::max(
        tail_ms,
        row.start_ms);  // Every immutable group contains exactly one mob.
  // SERVER POLICY, not an original hard lower bound or enemy-death proof:
  // accept the one-shot native claim at5s+3s, then conservatively acknowledge
  // after the fixed19 plan's last start+500ms+3s (=11.5s). Native all-terminal
  // acceleration may legally send904 at8s, so retain it instead of losing it.
  const auto elapsed = std::max(
      std::chrono::milliseconds{kMinimumClearElapsed},
      std::chrono::milliseconds{tail_ms} + std::chrono::milliseconds{3500});
  return start + elapsed;
}

protocol::SceneSessionSnapshot LocalScene(
    const application::PlayerMenuSnapshot& snapshot) {
  protocol::SceneConfiguration configuration;
  configuration.mode_id = kHordeTutorMode;
  configuration.family_key = kHordeTutorMap;
  configuration.map_key = kHordeTutorMap;
  configuration.display_name = "HordeTutor";
  // The existing canonical menu codec's reserved-zero convention; no request
  // duration, score, flags or display text is promoted to a business rule.
  configuration.field_316 = 0U;
  configuration.field_319 = 0U;
  configuration.field_320 = 0U;
  configuration.field_408 = 0U;
  return {.scene = {.scene_id = kPrivateScene,
                    .kind = 2U,
                    .name = "HordeTutor",
                    .occupants = 1U,
                    .observers = 0U,
                    .capacity = 1U,
                    .locked = 0U,
                    .field_40 = {},
                    .scene_type = 2U,
                    .configuration = std::move(configuration),
                    .key = snapshot.player_id.Value()},
          .host_slot = 0U,
          .selected_slot = 0U,
          .slots = {{.slot = 0U,
                     .team = 2U,
                     .occupied = true,
                     .locked = false,
                     .ready = false,
                     .player = snapshot}},
          .include_equipment_records = true};
}

bool ExactPlan(const protocol::Packet& packet) {
  protocol::PacketReader reader{packet.body};
  const auto rows = HordeTutorSpawns();
  if (reader.ReadU32() != 1U || reader.ReadU16() != 1U ||
      reader.ReadU32() != 19U || reader.ReadU32() != 19U || rows.size() != 19U)
    return false;
  std::set<std::uint8_t> groups;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto start = reader.ReadU32();
    const auto macro = reader.ReadUtf16(256U);
    const auto spawn = reader.ReadUtf16(4096U);
    const auto only_spawn_replay = reader.ReadU32();
    const auto group = reader.ReadU8();
    const auto mob = reader.ReadU8();
    if (group == 0U || group > 19U || mob != 1U || !groups.insert(group).second)
      return false;
    const auto& expected = rows[group - 1U];
    if (expected.group != group || start != expected.start_ms ||
        macro != expected.macro || spawn != expected.spawn_point ||
        only_spawn_replay != 0U)
      return false;
  }
  reader.RequireEnd();
  return groups.size() == rows.size();
}

void ValidateLocalRelay(const protocol::Packet& packet) {
  // Validation only: none of these modules' apply/handle/prepare_action paths
  // are called. The native local queue owns NPC construction/combat/respawn.
  if (HordeTutorMatch::Accepts(packet.opcode)) {
    HordeTutorMatch::Validate(packet);
  } else if (PirateMapAgents::Accepts(packet.opcode)) {
    PirateMapAgents::Validate(packet);
  } else if (MatchObjectives::Accepts(packet.opcode)) {
    MatchObjectives::Validate(packet);
  } else if (protocol::IsPveEventOpcode(packet.opcode)) {
    protocol::ValidatePveEvent(packet.opcode, packet.body);
  } else if (CombatRelay::IsPeerEffectOpcode(packet.opcode)) {
    const auto length = packet.opcode == 32824U   ? 16U
                        : packet.opcode == 32832U ? 32U
                                                  : 8U;
    if (packet.body.size() != length)
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid local tutorial peer effect size"};
  } else if (packet.opcode == 32840U) {
    static_cast<void>(protocol::DecodeMatchLatencyProbe(packet.body));
  } else if (packet.opcode == protocol::kSmartPingOpcode) {
    static_cast<void>(protocol::DecodeSmartPing(packet.body));
  } else {
    // Also rejects unknown relay opcodes, rather than accepting arbitrary data.
    protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
  }
}
}  // namespace

ServiceResult<LocalTutorialUpdate> LocalHordeTutorCoordinator::Begin(
    application::PlayerMenuSnapshot snapshot, const protocol::Packet& request,
    const domain::MatchId& session_id, Time now) {
  if (request.opcode != 107U)
    return Error(ErrorCode::kUnsupportedOpcode,
                 "local tutorial entry requires native107");
  const auto start = protocol::DecodeTutorialStartReport(request.body);
  if (start.relation_key != u"rel_prologue_01" ||
      start.mission_key != u"rel_prologue_01" || start.trailing_value != 1U ||
      start.kind != 0x10U)
    return Error(ErrorCode::kPermissionDenied,
                 "only the native prologue tutorial is enabled");
  if (!snapshot.player_id.IsValid() ||
      snapshot.player_id.Value() > std::numeric_limits<std::uint32_t>::max())
    return Error(ErrorCode::kInvalidInput,
                 "local tutorial requires a legacy-range authenticated ID");
  const auto player = snapshot.player_id;
  if (sessions_.contains(player))
    return Error(ErrorCode::kConflict,
                 "local tutorial session is already owned or awaiting SQL");

  // Encode before inserting: invalid snapshots cannot leave a ghost session.
  auto menu = protocol::EncodeMenuStateResponse(snapshot);
  LocalTutorialUpdate update;
  update.deliveries.push_back(
      {player,
       {112U, protocol::EncodeSceneSessionResponse(LocalScene(snapshot))}});
  update.deliveries.push_back(
      {player,
       {510U, protocol::EncodeStartSceneResponse(
                  {.map_key = std::string{kHordeTutorMap},
                   .mode_key = std::string{kHordeTutorMap}})}});
  update.phase = LocalTutorialPhase::kLoading;
  update.persistence = application::TutorialSessionStart{
      .session_id = session_id,
      .player_id = player,
      .profile_hash = std::string{kProfileHash}};
  sessions_.emplace(player, Session{.id = session_id,
                                    .menu_scene = std::move(menu),
                                    .loading_started = now});
  return update;
}

ServiceResult<LocalTutorialUpdate> LocalHordeTutorCoordinator::Handle(
    domain::PlayerId player, const protocol::Packet& packet, Time now) {
  if (packet.opcode == 32840U) {
    const auto probe = protocol::DecodeMatchLatencyProbe(packet.body);
    if (!sessions_.contains(player))
      return Error(ErrorCode::kInvalidState,
                   "no local tutorial session for probe");
    LocalTutorialUpdate reply;
    if (probe.slot == 0U)
      reply.deliveries.push_back(
          {player, {32840U, protocol::EncodeMatchLatencyProbe(probe)}});
    return reply;  // Ordered authenticated liveness barrier, never scene/score
                   // authority.
  }
  const bool local_relay = packet.opcode >= 0x8000U;
  if (local_relay) {
    ValidateLocalRelay(packet);
  } else if (HordeTutorMatch::Accepts(packet.opcode)) {
    HordeTutorMatch::Validate(packet);
  } else if (packet.opcode == 709U) {
    static_cast<void>(protocol::DecodePlayerObjectIdentityRequest(packet.body));
  } else if (packet.opcode == 702U) {
    static_cast<void>(protocol::DecodeMapAssetsRequest(packet.body));
  } else if (packet.opcode == 701U) {
    static_cast<void>(protocol::DecodeLoadingCompleteRequest(packet.body));
  } else if (packet.opcode == 704U || packet.opcode == 705U) {
    static_cast<void>(
        protocol::DecodeNativeRespawn(packet.opcode, packet.body));
  } else if (packet.opcode == 707U) {
    MatchObjectives::Validate(
        packet);  // C2S707 is a twoDWORD event, not the fourDWORD S2C707.
  } else if (packet.opcode == 105U) {
    static_cast<void>(protocol::DecodeReturnToMenuRequest(packet.body));
  } else if (packet.opcode == 404U) {
    static_cast<void>(protocol::DecodeLeaveRoomRequest(packet.body));
  } else if (packet.opcode == 925U) {
    protocol::ValidatePveEvent(packet.opcode, packet.body);
  } else if (packet.opcode == 725U) {
    static_cast<void>(protocol::DecodeSpecialModeFinishClaim(packet.body));
  } else if (packet.opcode == 107U) {
    static_cast<void>(protocol::DecodeTutorialStartReport(packet.body));
    return Error(ErrorCode::kInvalidState,
                 "native107 must use the SQL-gated begin operation");
  } else {
    return Error(ErrorCode::kUnsupportedOpcode,
                 "opcode is not a local tutorial operation");
  }

  const auto found = sessions_.find(player);
  if (found == sessions_.end())
    return Error(ErrorCode::kInvalidState,
                 "authenticated player has no local tutorial session");
  auto& session = found->second;
  if (local_relay || packet.opcode == 906U || packet.opcode == 925U ||
      packet.opcode == 725U || packet.opcode == 705U || packet.opcode == 707U)
    return LocalTutorialUpdate{};  // No NPC/death/score/reward authority from
                                   // local or TCP reports.
  if (!session.begin_committed)
    return Error(ErrorCode::kInvalidState,
                 "local tutorial begin is awaiting SQL commit");

  if (session.stage == Stage::kFinishing) {
    if (packet.opcode == 905U && session.terminal &&
        std::get<application::TutorialSessionFinish>(
            *session.terminal->persistence)
            .reported_clear)
      return *session.terminal;  // Same UUID/command; caller still must commit
                                 // before sending.
    if (packet.opcode == 105U || packet.opcode == 404U)
      return LocalTutorialUpdate{};  // One interruption, never a second
                                     // terminal command.
    return Error(ErrorCode::kInvalidState,
                 "local tutorial finish is awaiting SQL commit");
  }
  if (packet.opcode == 105U || packet.opcode == 404U)
    return Finish(player, session, false, "client_leave", true);

  if (packet.opcode == 704U) {
    const auto respawn =
        protocol::DecodeNativeRespawn(packet.opcode, packet.body);
    if (session.stage != Stage::kPlaying && session.stage != Stage::kClearAck)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial respawn requires Playing");
    if (respawn.actor_object_id != kLocalObject)
      return Error(ErrorCode::kPermissionDenied,
                   "local tutorial respawn actor is not its owner");
    // This request uses directTCP, unlike local0x80xx restoration. There is no
    // server-observed local death to demand. Preserve the native selector
    // DWORD, not an NPC ID/group index (103 has no proven selector set).
    // Return native707's fourDWORD consumer layout. reset1/extra0 are
    // compatible server choices, not captured official values or score/reward
    // authority.
    protocol::PacketWriter writer;
    writer.WriteU32(kLocalObject);
    writer.WriteU32(respawn.respawn_point);
    writer.WriteU32(1U);
    writer.WriteU32(0U);
    LocalTutorialUpdate update;
    update.deliveries.push_back({player, {707U, writer.Release()}});
    return update;
  }

  if (packet.opcode == 709U || packet.opcode == 702U || packet.opcode == 701U) {
    if (packet.opcode == 709U &&
        protocol::DecodePlayerObjectIdentityRequest(packet.body).object_id !=
            kLocalObject)
      return Error(ErrorCode::kInvalidInput,
                   "local tutorial actor must be seat-zero object1000000");
    if (session.stage != Stage::kLoading)
      return LocalTutorialUpdate{};  // Strictly validated late loading packets
                                     // cannot restart play.
    if (now >= session.loading_started + kLoadingTimeout)
      return Finish(player, session, false, "loading_timeout", true);
    if (now < session.loading_started)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial loading clock rollback");
    if (packet.opcode == 709U)
      session.object_received = true;
    else if (packet.opcode == 702U)
      // Bounded original map/birthpoint data only. object_count and vectors do
      // not grant NPCs, construction budgets, equipment, kills or score.
      session.assets_received = true;
    else
      session.loading_received = true;
    if (!session.object_received || !session.assets_received ||
        !session.loading_received)
      return LocalTutorialUpdate{};

    LocalTutorialUpdate update;
    update.deliveries = {
        {player, {701U, protocol::EncodeActivateSlotResponse(0U)}},
        {player, {702U, protocol::EncodeLoadingOptionsResponse({})}},
        {player, {703U, protocol::EncodeCountdownResponse(0U)}},
        {player,
         {704U,
          protocol::EncodeMatchBeginResponse(
              // Echo the trusted native MapMeta clock sentinel only.
              // No Playing deadline or formal match duration is scheduled.
              {.duration_ms = 99'999'000U, .scene_id = kPrivateScene})}}};
    update.phase = LocalTutorialPhase::kPlaying;
    session.stage = Stage::kPlaying;
    if (session.early_wave_start) {
      session.early_wave_start = false;
      session.wave_started = now;
      update.deliveries.push_back(
          {player, {904U, WaveBody()}});  // Always after native704.
    }
    return update;
  }

  protocol::PacketReader reader{packet.body};
  if (packet.opcode == 903U) {
    if (reader.ReadU32() != 1U)
      return Error(ErrorCode::kInvalidInput, "local tutorial has only wave1");
    if (session.stage == Stage::kLoading) {
      session.early_wave_start =
          true;  // Native may request only once before loading completes.
      return LocalTutorialUpdate{};
    }
    if (session.stage != Stage::kPlaying)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial wave cannot restart after clear ack");
    if (session.wave_started)
      return LocalTutorialUpdate{};  // Duplicate903 never resets the minimum
                                     // elapsed clock.
    session.wave_started = now;
    LocalTutorialUpdate update;
    update.deliveries.push_back({player, {904U, WaveBody()}});
    return update;
  }
  if (packet.opcode == 907U)
    return Error(ErrorCode::kPermissionDenied,
                 "no additional local tutorial waves are enabled");
  if (packet.opcode == 908U) {
    if (session.stage != Stage::kPlaying || !session.wave_started)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial plan requires a playing wave");
    if (!session.plan.empty()) {
      if (session.plan != packet.body)
        return Error(ErrorCode::kInvalidInput,
                     "local tutorial plan replay changed bytes");
    } else {
      if (!ExactPlan(packet))
        return Error(ErrorCode::kInvalidInput,
                     "local tutorial plan differs from immutable19 rows");
      session.plan =
          packet
              .body;  // Atomic: no group is consumed before every row matches.
    }
    LocalTutorialUpdate update;
    update.deliveries.push_back({player, {909U, session.plan}});
    return update;
  }
  if (packet.opcode == 904U) {
    if (reader.ReadU32() != 1U)
      return Error(ErrorCode::kInvalidInput,
                   "local tutorial clear refers to a different wave");
    if (session.stage == Stage::kClearAck)
      return LocalTutorialUpdate{};  // Send the native acknowledgement only
                                     // once.
    if (session.stage != Stage::kPlaying || !session.wave_started ||
        session.plan.empty() ||
        now < *session.wave_started + kMinimumClearElapsed)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial clear claim is premature");
    if (!session.clear_due)
      session.clear_due = ClearAckDue(*session.wave_started);
    return AdvanceClear(
        player, session,
        now);  // Retain one-shot native904 until the policy window.
  }
  if (packet.opcode == 905U) {
    if (session.stage != Stage::kClearAck || !session.clear_ack_at ||
        now < *session.clear_ack_at + kFinalRequestDelay)
      return Error(ErrorCode::kInvalidState,
                   "local tutorial success requires clear ack and3s");
    return Finish(player, session, true, "client_reported_clear", true);
  }
  return Error(ErrorCode::kUnsupportedOpcode,
               "opcode is not a local tutorial operation");
}

LocalTutorialUpdate LocalHordeTutorCoordinator::AdvanceClear(
    domain::PlayerId player, Session& session, Time now) {
  if (session.stage != Stage::kPlaying || !session.clear_due ||
      now < *session.clear_due)
    return {};
  LocalTutorialUpdate update;
  update.deliveries.push_back({player, {905U, WaveBody()}});
  session.stage = Stage::kClearAck;
  session.clear_due.reset();
  session.clear_ack_at = now;
  return update;  // CLIENT CLAIM acknowledgement only; no server-side NPC/death
                  // assertion.
}

LocalTutorialUpdate LocalHordeTutorCoordinator::Finish(domain::PlayerId player,
                                                       Session& session,
                                                       bool reported_clear,
                                                       std::string reason,
                                                       bool return_to_menu) {
  if (session.terminal) return {};
  LocalTutorialUpdate update;
  update.persistence =
      application::TutorialSessionFinish{.session_id = session.id,
                                         .player_id = player,
                                         .reported_clear = reported_clear,
                                         .reason = std::move(reason)};
  update.phase = LocalTutorialPhase::kMenu;
  if (return_to_menu) {
    update.deliveries = {{player, {736U, {}}},
                         {player, {112U, session.menu_scene}}};
  }
  session.stage = Stage::kFinishing;
  session.terminal = update;
  return update;
}

LocalTutorialUpdate LocalHordeTutorCoordinator::Disconnect(
    domain::PlayerId player, std::string reason) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end()) return {};
  if (found->second.terminal) {
    found->second.terminal->deliveries
        .clear();  // A pending clear cannot become a different outcome.
    return {};
  }
  if (reason.empty()) reason = "disconnect";
  return Finish(player, found->second, false, std::move(reason), false);
}

bool LocalHordeTutorCoordinator::Acknowledge(domain::PlayerId player,
                                             const domain::MatchId& session_id,
                                             bool is_begin) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end() || found->second.id != session_id) return false;
  if (is_begin) {
    if (found->second.begin_committed ||
        found->second.stage != Stage::kLoading || found->second.terminal)
      return false;
    found->second.begin_committed = true;
    return true;
  }
  if (!found->second.terminal || found->second.stage != Stage::kFinishing)
    return false;
  sessions_.erase(found);
  return true;
}

bool LocalHordeTutorCoordinator::BeginFailed(
    domain::PlayerId player, const domain::MatchId& session_id) {
  const auto found = sessions_.find(player);
  if (found == sessions_.end() || found->second.id != session_id ||
      found->second.begin_committed || found->second.stage != Stage::kLoading ||
      found->second.terminal)
    return false;
  sessions_.erase(found);
  return true;
}

bool LocalHordeTutorCoordinator::Contains(domain::PlayerId player) const {
  return sessions_.contains(player);
}

std::optional<domain::MatchId> LocalHordeTutorCoordinator::SessionId(
    domain::PlayerId player) const {
  const auto found = sessions_.find(player);
  return found == sessions_.end() ? std::nullopt
                                  : std::optional{found->second.id};
}

std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>>
LocalHordeTutorCoordinator::Tick(Time now) {
  std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>> updates;
  for (auto& [player, session] : sessions_) {
    if (!session.begin_committed) continue;
    if (session.stage == Stage::kLoading &&
        now >= session.loading_started + kLoadingTimeout)
      updates.emplace_back(
          player, Finish(player, session, false, "loading_timeout", true));
    else if (session.stage == Stage::kPlaying && session.clear_due &&
             now >= *session.clear_due)
      updates.emplace_back(player, AdvanceClear(player, session, now));
  }
  return updates;
}

std::optional<LocalHordeTutorCoordinator::Time>
LocalHordeTutorCoordinator::Deadline() const {
  std::optional<Time> earliest;
  for (const auto& [player, session] : sessions_) {
    static_cast<void>(player);
    if (!session.begin_committed) continue;
    const auto due =
        session.stage == Stage::kLoading
            ? std::optional{session.loading_started + kLoadingTimeout}
        : session.stage == Stage::kPlaying ? session.clear_due
                                           : std::nullopt;
    if (due && (!earliest || *due < *earliest)) earliest = due;
  }
  return earliest;
}

}  // namespace bf::runtime
