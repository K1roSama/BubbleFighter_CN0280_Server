#include "features/scenes/plaza_coordinator.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "features/characters/native_title_profile.h"
#include "protocol/chat/chat_messages.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_session_messages.h"

namespace bf::runtime {
namespace {

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

}  // namespace

PlazaCoordinator::PlazaCoordinator(std::uint16_t capacity,
                                   std::uint32_t scene_id)
    : human_capacity_{capacity} {
  if (capacity == 0U || capacity > 16U) {
    throw std::invalid_argument{"广场真人容量必须在 1 到 16 之间"};
  }
  if (scene_id == 0U || scene_id > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument{"广场场景标识必须在 1 到 65535 之间"};
  }
  base_scene_.scene_id = static_cast<std::uint16_t>(scene_id);
  base_scene_.kind = 0U;
  base_scene_.name = "Local Plaza";
  if (scene_id != 1U) {
    base_scene_.name += " " + std::to_string(scene_id);
  }
  base_scene_.occupants = 0U;
  base_scene_.observers = 0U;
  base_scene_.capacity = capacity;
  base_scene_.locked = 0U;
  base_scene_.field_40.clear();
  base_scene_.scene_type = 3U;
  base_scene_.field_68 = 0U;
  base_scene_.key = 0U;
  base_scene_.field_80.clear();
  base_scene_.field_104 = 0U;
  base_scene_.field_108 = 0U;
  base_scene_.field_112 = 0U;
  base_scene_.field_116 = 0U;
}

PlazaCoordinatorResult PlazaCoordinator::ClanMarkChanged(
    domain::PlayerId player, std::uint32_t mark) {
  PlazaCoordinatorResult result;
  const auto found = members_.find(player);
  if (found == members_.end()) {
    return result;
  }
  found->second.snapshot.clan_mark = mark;
  protocol::PacketWriter writer;
  writer.WriteU32(found->second.seat);
  writer.WriteU32(mark);
  const OutboundMessage message{.opcode = 991U, .body = writer.Release()};
  for (const auto& [recipient, member] : members_) {
    static_cast<void>(member);
    result.deliveries.push_back({.recipient = recipient, .message = message});
  }
  return result;
}

application::ServiceResult<PlazaCoordinatorResult> PlazaCoordinator::Enter(
    application::PlayerMenuSnapshot snapshot, protocol::ScenePartyState party) {
  if (!snapshot.player_id.IsValid()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "进入广场的玩家标识无效");
  }

  const auto player_id = snapshot.player_id;
  auto candidate = members_;
  auto candidate_host = host_player_id_;
  const auto existing = candidate.find(player_id);
  const bool refreshed = existing != candidate.end();
  if (refreshed) {
    existing->second.snapshot = std::move(snapshot);
    existing->second.movement.reset();
    existing->second.party = party;
  } else {
    if (candidate.size() >= human_capacity_) {
      return MakeError(application::ErrorCode::kOverloaded, "自由广场已满");
    }
    std::vector<bool> used(human_capacity_, false);
    for (const auto& [member_id, member] : candidate) {
      static_cast<void>(member_id);
      used[member.seat] = true;
    }
    const auto free_seat = std::ranges::find(used, false);
    if (free_seat == used.end()) {
      return MakeError(application::ErrorCode::kInternal,
                       "广场人数与席位索引不一致");
    }
    const auto seat =
        static_cast<std::uint8_t>(std::distance(used.begin(), free_seat));
    candidate.emplace(player_id, Member{.seat = seat,
                                        .snapshot = std::move(snapshot),
                                        .movement = std::nullopt,
                                        .party = party,
                                        .recruitment_text = {},
                                        .recruitment_blocked = {}});
    if (!candidate_host.IsValid()) {
      candidate_host = player_id;
    }
  }

  PlazaCoordinatorResult result{
      .event = refreshed ? PlazaCoordinatorResult::Event::kMemberRefreshed
                         : PlazaCoordinatorResult::Event::kMemberEntered,
      .deliveries = {},
      .session_updates = {
          {.player_id = player_id, .stage = PlazaSessionStage::kPlaza}}};
  try {
    const auto& entered = candidate.at(player_id);
    const protocol::SceneSessionSlot entered_slot{.slot = entered.seat,
                                                  .team = 0U,
                                                  .occupied = true,
                                                  .locked = false,
                                                  .ready = false,
                                                  .player = entered.snapshot,
                                                  .party = entered.party};
    const OutboundMessage member_update{
        .opcode = 503U,
        .body = protocol::EncodeSceneMemberUpdateResponse(entered_slot)};
    const OutboundMessage scene_update{
        .opcode = 501U,
        .body = protocol::EncodeRoomSceneUpdateResponse(SceneFor(candidate))};
    for (const auto recipient : OrderedPlayers(candidate)) {
      if (recipient == player_id) {
        continue;
      }
      result.deliveries.push_back(
          {.recipient = recipient, .message = member_update});
      result.deliveries.push_back(
          {.recipient = recipient, .message = scene_update});
    }
    result.deliveries.push_back(
        {.recipient = player_id,
         .message = {
             .opcode = 112U,
             .body = protocol::EncodeSceneSessionResponse(SessionSnapshot(
                 candidate,
                 {.host = candidate_host, .selected = player_id}))}});
    for (const auto other_id : OrderedPlayers(candidate)) {
      if (other_id == player_id) {
        continue;
      }
      const auto& other = candidate.at(other_id);
      if (other.party.recruiting && !other.recruitment_text.empty() &&
          std::ranges::find(other.recruitment_blocked, player_id) ==
              other.recruitment_blocked.end()) {
        result.deliveries.push_back(
            {player_id,
             {516U, protocol::EncodePartyRecruitmentTextResponse(
                        other.seat, other.recruitment_text)}});
      }
      if (other.movement.has_value()) {
        result.deliveries.push_back(
            {.recipient = player_id,
             .message = {.opcode = 514U,
                         .body = protocol::EncodeSceneMovementResponse(
                             other_id, *other.movement)}});
      }
    }
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "广场资料不能编码为已验证协议");
  } catch (const std::logic_error&) {
    return MakeError(application::ErrorCode::kInternal, "广场成员快照契约失效");
  }

  members_ = std::move(candidate);
  host_player_id_ = candidate_host;
  return result;
}

application::ServiceResult<PlazaCoordinatorResult> PlazaCoordinator::Leave(
    domain::PlayerId player_id) {
  if (!player_id.IsValid()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "离开广场的玩家标识无效");
  }
  PlazaCoordinatorResult result{
      .event = PlazaCoordinatorResult::Event::kNoChange,
      .deliveries = {},
      .session_updates = {
          {.player_id = player_id, .stage = PlazaSessionStage::kOutsidePlaza}}};
  const auto current = members_.find(player_id);
  if (current == members_.end()) {
    return result;
  }

  const auto vacated_seat = current->second.seat;
  auto candidate = members_;
  candidate.erase(player_id);
  auto candidate_host = host_player_id_;
  if (candidate_host == player_id) {
    const auto ordered = OrderedPlayers(candidate);
    candidate_host = ordered.empty() ? domain::PlayerId{} : ordered.front();
  }
  result.event = PlazaCoordinatorResult::Event::kMemberLeft;
  try {
    const std::array messages{
        OutboundMessage{
            .opcode = 511U,
            .body = protocol::EncodeSceneMemberRemovedResponse(vacated_seat)},
        OutboundMessage{.opcode = 501U,
                        .body = protocol::EncodeRoomSceneUpdateResponse(
                            SceneFor(candidate))}};
    for (const auto recipient : OrderedPlayers(candidate)) {
      for (const auto& message : messages) {
        result.deliveries.push_back(
            {.recipient = recipient, .message = message});
      }
    }
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "广场离开广播不能编码为已验证协议");
  }

  members_ = std::move(candidate);
  host_player_id_ = candidate_host;
  return result;
}

PlazaCoordinatorResult PlazaCoordinator::UpdateParty(
    domain::PlayerId player, protocol::ScenePartyState party) {
  PlazaCoordinatorResult result;
  const auto found = members_.find(player);
  if (found == members_.end() || found->second.party == party) return result;
  auto& member = found->second;
  member.party = party;
  // B3FD40: u32 seat followed by a complete UserSlot, including +128/+132/+136.
  const OutboundMessage message{
      511U, protocol::EncodeSceneMemberStateResponse({.slot = member.seat,
                                                      .team = 0U,
                                                      .occupied = true,
                                                      .player = member.snapshot,
                                                      .party = member.party})};
  for (const auto recipient : OrderedPlayers(members_)) {
    result.deliveries.push_back({recipient, message});
    if (party.recruiting && !member.recruitment_text.empty() &&
        std::ranges::find(member.recruitment_blocked, recipient) ==
            member.recruitment_blocked.end()) {
      result.deliveries.push_back(
          {recipient,
           {516U, protocol::EncodePartyRecruitmentTextResponse(
                      member.seat, member.recruitment_text)}});
    }
  }
  return result;
}

PlazaCoordinatorResult PlazaCoordinator::RecruitmentText(
    domain::PlayerId player, std::u16string text,
    std::span<const domain::PlayerId> blocked,
    std::optional<std::uint8_t> recipient_slot) {
  PlazaCoordinatorResult result;
  const auto found = members_.find(player);
  if (found == members_.end()) return result;
  const OutboundMessage message{
      516U,
      protocol::EncodePartyRecruitmentTextResponse(found->second.seat, text)};
  found->second.recruitment_text = std::move(text);
  found->second.recruitment_blocked.assign(blocked.begin(), blocked.end());
  for (const auto recipient : OrderedPlayers(members_)) {
    if (recipient_slot && members_.at(recipient).seat != *recipient_slot)
      continue;
    if (recipient == player ||
        std::ranges::find(blocked, recipient) == blocked.end())
      result.deliveries.push_back({recipient, message});
  }
  return result;
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaCoordinator::HandleMovement(domain::PlayerId player_id,
                                 std::span<const std::uint8_t> body) {
  const auto member = members_.find(player_id);
  if (member == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在自由广场中");
  }
  const auto movement = protocol::DecodeSceneMovementRequest(body);
  PlazaCoordinatorResult result{
      .event = PlazaCoordinatorResult::Event::kMovementBroadcast,
      .deliveries = {},
      .session_updates = {}};
  const OutboundMessage outbound{
      .opcode = 514U,
      .body = protocol::EncodeSceneMovementResponse(player_id, movement)};
  for (const auto recipient : OrderedPlayers(members_)) {
    if (recipient != player_id) {
      result.deliveries.push_back(
          {.recipient = recipient, .message = outbound});
    }
  }
  member->second.movement = movement;
  return result;
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaCoordinator::RelayMinigame(domain::PlayerId player_id,
                                const protocol::LobbyMinigameSync& sync) const {
  if (!members_.contains(player_id)) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在自由广场中");
  }
  if (player_id.Value() > 0xFFFFFFFFULL) {
    return MakeError(application::ErrorCode::kInternal,
                     "广场玩家 ID 超出客户端范围");
  }
  PlazaCoordinatorResult result{
      .event = PlazaCoordinatorResult::Event::kMovementBroadcast,
      .deliveries = {},
      .session_updates = {}};
  const OutboundMessage outbound{
      .opcode = protocol::kLobbyMinigameSyncOpcode,
      .body = protocol::EncodeLobbyMinigameSync(
          static_cast<std::uint32_t>(player_id.Value()), sync)};
  for (const auto recipient : OrderedPlayers(members_)) {
    if (recipient != player_id) {
      result.deliveries.push_back(
          {.recipient = recipient, .message = outbound});
    }
  }
  return result;
}

application::ServiceResult<PlazaCoordinatorResult> PlazaCoordinator::HandleChat(
    domain::PlayerId player_id, const domain::UserMessage& message,
    std::span<const domain::PlayerId> blocked_peers) const {
  const auto sender = members_.find(player_id);
  if (sender == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "玩家不在自由广场中");
  }

  OutboundMessage outbound;
  try {
    outbound = {
        .opcode = 509U,
        .body = protocol::EncodeNearbyChatResponse(
            {.sender_slot = static_cast<std::int16_t>(sender->second.seat),
             .sender_name =
                 protocol::Utf8ToUtf16(sender->second.snapshot.nickname),
             .team_only = false,
             .text = message.Text()})};
  } catch (const protocol::ProtocolError&) {
    return MakeError(application::ErrorCode::kInternal,
                     "广场聊天身份不能编码为已验证协议");
  }

  PlazaCoordinatorResult result{
      .event = PlazaCoordinatorResult::Event::kChatBroadcast,
      .deliveries = {},
      .session_updates = {}};
  for (const auto recipient : OrderedPlayers(members_)) {
    if (recipient != player_id &&
        std::ranges::find(blocked_peers, recipient) == blocked_peers.end()) {
      result.deliveries.push_back(
          {.recipient = recipient, .message = outbound});
    }
  }
  result.deliveries.push_back(
      {.recipient = player_id, .message = std::move(outbound)});
  return result;
}

application::ServiceResult<bool> PlazaCoordinator::UpdatePlayerSnapshot(
    application::PlayerMenuSnapshot snapshot) {
  if (!snapshot.player_id.IsValid()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "广场资料玩家标识无效");
  }
  const auto member = members_.find(snapshot.player_id);
  if (member == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "资料快照玩家不在自由广场中");
  }
  member->second.snapshot = std::move(snapshot);
  return true;
}

bool PlazaCoordinator::Contains(domain::PlayerId player_id) const noexcept {
  return members_.contains(player_id);
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaCoordinator::EquipmentChanged(application::PlayerMenuSnapshot snapshot) {
  const auto player = snapshot.player_id;
  if (!members_.contains(player)) {
    return PlazaCoordinatorResult{};
  }
  const auto seat = members_.at(player).seat;
  const OutboundMessage update{
      .opcode = 503U,
      .body = protocol::EncodeSceneMemberUpdateResponse(
          {.slot = seat,
           .team = 0U,
           .occupied = true,
           .locked = false,
           .ready = false,
           .player = snapshot,
           .party = members_.at(player).party})};
  const OutboundMessage equipment{
      .opcode = 806U, .body = protocol::EncodeEquippedItemsResponse(snapshot)};
  members_.at(player).snapshot = std::move(snapshot);
  PlazaCoordinatorResult result;
  for (const auto& [recipient, member] : members_) {
    static_cast<void>(member);
    result.deliveries.push_back({.recipient = recipient, .message = update});
    if (recipient != player) {
      result.deliveries.push_back(
          {.recipient = recipient, .message = equipment});
    }
  }
  return result;
}

std::size_t PlazaCoordinator::MemberCount() const noexcept {
  return members_.size();
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaCoordinator::CoupleChanged(
    const application::PlayerMenuSnapshot& snapshot) {
  const auto found = members_.find(snapshot.player_id);
  if (found == members_.end()) return PlazaCoordinatorResult{};
  application::SyncNativeTitleEquipmentFlags(found->second.snapshot, snapshot);
  found->second.snapshot.couple = snapshot.couple;
  const OutboundMessage message{.opcode = 949U,
                                .body = protocol::EncodeCoupleSlotResponse(
                                    {.slot = found->second.seat,
                                     .team = 0U,
                                     .occupied = true,
                                     .locked = false,
                                     .ready = false,
                                     .player = found->second.snapshot,
                                     .party = found->second.party})};
  PlazaCoordinatorResult result;
  for (const auto& [recipient, member] : members_) {
    static_cast<void>(member);
    result.deliveries.push_back({.recipient = recipient, .message = message});
  }
  return result;
}

protocol::SceneInfo PlazaCoordinator::Scene() const {
  return SceneFor(members_);
}

protocol::SceneInfo PlazaCoordinator::SceneFor(const MemberMap& members) const {
  auto result = base_scene_;
  result.occupants = static_cast<std::uint16_t>(members.size());
  return result;
}

std::vector<domain::PlayerId> PlazaCoordinator::OrderedPlayers(
    const MemberMap& members) const {
  std::vector<std::pair<std::uint8_t, domain::PlayerId>> by_seat;
  by_seat.reserve(members.size());
  for (const auto& [player_id, member] : members) {
    by_seat.emplace_back(member.seat, player_id);
  }
  std::ranges::sort(by_seat);
  std::vector<domain::PlayerId> result;
  result.reserve(by_seat.size());
  for (const auto& [seat, player_id] : by_seat) {
    static_cast<void>(seat);
    result.push_back(player_id);
  }
  return result;
}

std::uint8_t PlazaCoordinator::HostSeat(const MemberMap& members,
                                        domain::PlayerId host) const {
  if (members.empty()) {
    return 0U;
  }
  const auto member = members.find(host);
  if (member == members.end()) {
    throw std::logic_error{"广场快照缺少房主"};
  }
  return member->second.seat;
}

protocol::SceneSessionSnapshot PlazaCoordinator::SessionSnapshot(
    const MemberMap& members, SnapshotSelection selection) const {
  const auto selected_member = members.find(selection.selected);
  if (selected_member == members.end()) {
    throw std::logic_error{"广场快照缺少本地玩家"};
  }
  std::vector<protocol::SceneSessionSlot> slots;
  slots.reserve(base_scene_.capacity);
  for (std::uint16_t seat = 0U; seat < base_scene_.capacity; ++seat) {
    const auto member = std::ranges::find_if(
        members,
        [seat](const auto& entry) { return entry.second.seat == seat; });
    if (member == members.end()) {
      slots.push_back({.slot = static_cast<std::uint8_t>(seat),
                       .team = 255U,
                       .occupied = false,
                       .locked = false,
                       .ready = false,
                       .player = std::nullopt});
    } else {
      slots.push_back({.slot = static_cast<std::uint8_t>(seat),
                       .team = 0U,
                       .occupied = true,
                       .locked = false,
                       .ready = false,
                       .player = member->second.snapshot,
                       .party = member->second.party});
    }
  }
  return {.scene = SceneFor(members),
          .host_slot = HostSeat(members, selection.host),
          .selected_slot = selected_member->second.seat,
          .slots = std::move(slots),
          .include_equipment_records = true};
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaCoordinator::LevelIconChanged(application::PlayerMenuSnapshot snapshot) {
  const auto found = members_.find(snapshot.player_id);
  if (found == members_.end()) return PlazaCoordinatorResult{};
  const OutboundMessage message{.opcode = 973U,
                                .body = protocol::EncodeLevelIconResponse(
                                    {.slot = found->second.seat,
                                     .team = 0U,
                                     .occupied = true,
                                     .locked = false,
                                     .ready = false,
                                     .player = snapshot,
                                     .party = found->second.party})};
  found->second.snapshot = std::move(snapshot);
  PlazaCoordinatorResult result;
  for (const auto& [recipient, member] : members_) {
    static_cast<void>(member);
    result.deliveries.push_back({.recipient = recipient, .message = message});
  }
  return result;
}

}  // namespace bf::runtime
