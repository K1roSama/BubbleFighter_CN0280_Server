#include <algorithm>
#include <limits>
#include <utility>

#include "features/chat/user_message.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "sessions/world_coordinator.h"

namespace bf::runtime {
namespace {

void Status(WorldCoordinatorResult& result, domain::PlayerId recipient,
            std::uint16_t opcode, std::uint32_t value,
            std::string_view nickname) {
  protocol::PacketWriter writer;
  writer.WriteU32(value);
  writer.WriteUtf16(protocol::Utf8ToUtf16(nickname));
  if (opcode == 117U) writer.WriteU32(0U);
  result.deliveries.push_back({recipient, {opcode, writer.Release()}});
}

void Named(WorldCoordinatorResult& result, domain::PlayerId recipient,
           std::uint16_t opcode, std::string_view nickname) {
  protocol::PacketWriter writer;
  writer.WriteUtf16(protocol::Utf8ToUtf16(nickname));
  result.deliveries.push_back({recipient, {opcode, writer.Release()}});
}

bool Flag(protocol::PacketReader& reader) {
  const auto value = reader.ReadU32();
  if (value > 1U) {
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "party flag must be zero or one"};
  }
  return value != 0U;
}

void AppendPlaza(WorldCoordinatorResult& result,
                 PlazaCoordinatorResult source) {
  for (auto& delivery : source.deliveries)
    result.deliveries.push_back(
        {delivery.recipient, std::move(delivery.message)});
}

constexpr std::size_t kMaximumPartyMembers = 8U;

}  // namespace

void WorldCoordinator::GatewayPlayerOnline(
    application::PlayerMenuSnapshot snapshot) {
  const auto player = snapshot.player_id;
  PlayerOnline(std::move(snapshot));
  if (const auto found = online_players_.find(player);
      found != online_players_.end())
    found->second.state =
        {};  // A replacement gateway starts in its own Menu, not H1's phase.
}

void WorldCoordinator::PlayerOnline(application::PlayerMenuSnapshot snapshot) {
  const auto id = snapshot.player_id;
  if (!id.IsValid() || snapshot.nickname.empty()) return;
  const auto found = online_players_.find(id);
  if (found == online_players_.end()) {
    online_players_.emplace(id, OnlinePlayer{std::move(snapshot), {}});
  } else {
    found->second.snapshot = std::move(snapshot);
  }
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandlePlazaUserList(const WorldActorContext& actor,
                                      const protocol::Packet& packet) const {
  protocol::PacketReader{packet.body}.RequireEnd();
  WorldCoordinatorResult result;
  const auto room = rooms_.FindRoom(actor.player_id);
  if (!room || room->Phase() != RoomPhase::kWaiting) return result;
  const auto channel = [](std::uint32_t value) {
    return value == 0U ? 60U : value;
  };
  std::vector<protocol::PlazaUserListEntry> entries;
  for (const auto& [id, player] : online_players_) {
    if (id == actor.player_id || !plaza_.Contains(id) || rooms_.FindRoom(id) ||
        player.state.phase != WorldSessionPhase::kPlaza ||
        channel(player.state.channel) != channel(actor.state.channel))
      continue;
    if (player.snapshot.progress.level < 1 ||
        player.snapshot.progress.level > 1000)
      continue;
    entries.push_back(
        {.nickname = player.snapshot.nickname,
         .level = static_cast<std::uint32_t>(player.snapshot.progress.level)});
    if (entries.size() == protocol::kMaximumPlazaUserList) break;
  }
  result.deliveries.push_back(
      {actor.player_id,
       {113U, protocol::EncodePlazaUserListResponse(entries)}});
  return result;
}

std::vector<domain::PlayerId> WorldCoordinator::PartyMembers(
    domain::PlayerId player) const {
  for (const auto& [leader, members] : parties_) {
    static_cast<void>(leader);
    if (std::ranges::find(members, player) != members.end()) return members;
  }
  return {player};
}

protocol::ScenePartyState WorldCoordinator::PartySceneState(
    domain::PlayerId player, bool allow_recruiting) const {
  const auto members = PartyMembers(player);
  const auto leader = members.front();
  const bool recruiting = allow_recruiting &&
                          recruiting_parties_.contains(leader) &&
                          members.size() < kMaximumPartyMembers;
  std::uint32_t badge = 0xFFFFFFFFU;
  if (members.size() > 1U && allow_recruiting) {
    const auto scene = plaza_.Scene(player);
    if (scene && party_badges_.contains(scene->scene_id)) {
      const auto& badges = party_badges_.at(scene->scene_id);
      if (const auto found = badges.find(leader); found != badges.end())
        badge = found->second;
    }
  }
  return {.leader = leader == player,
          .party_id = badge,
          .recruiting = leader == player && recruiting};
}

void WorldCoordinator::SyncPartyPlaza(WorldCoordinatorResult& result,
                                      domain::PlayerId player) {
  const auto members = PartyMembers(player);
  if (members.size() >= kMaximumPartyMembers)
    recruiting_parties_.erase(members.front());
  std::map<std::uint16_t, std::set<domain::PlayerId>> visible;
  for (const auto& [id, online] : online_players_) {
    static_cast<void>(online);
    const auto scene = plaza_.Scene(id);
    const auto group = PartyMembers(id);
    if (scene && group.size() > 1U)
      visible[scene->scene_id].insert(group.front());
  }
  std::erase_if(party_badges_, [&](const auto& entry) {
    return !visible.contains(entry.first);
  });
  for (const auto& [scene, leaders] : visible) {
    auto& badges = party_badges_[scene];
    std::erase_if(badges, [&](const auto& entry) {
      return !leaders.contains(entry.first);
    });
    for (const auto leader : leaders) {
      if (badges.contains(leader)) continue;
      for (std::uint32_t index = 0; index < 8U; ++index) {
        if (std::ranges::none_of(badges, [index](const auto& entry) {
              return entry.second == index;
            })) {
          badges.emplace(leader, index);
          break;
        }
      }
      // More than eight fragmented parties: leave extra badges hidden (-1),
      // never alias unrelated parties or send an out-of-bounds texture index to
      // the native client.
    }
  }
  for (const auto& [id, online] : online_players_) {
    static_cast<void>(online);
    AppendPlaza(result, plaza_.UpdateParty(id, PartySceneState(id)));
  }
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleRecruitment(WorldActorContext actor,
                                    const protocol::Packet& packet,
                                    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{packet.body};
  bool enabled = false;
  std::u16string text;
  std::optional<std::uint8_t> recipient_slot;
  if (packet.opcode == 305U) {
    enabled = Flag(reader);
  } else {
    text = reader.ReadUtf16(20U);
    if (packet.opcode == 2031U)
      recipient_slot =
          reader.ReadU8();  // 9C1130 -> C72AB0: newcomer UserSlot index.
  }
  reader.RequireEnd();
  WorldCoordinatorResult result;
  const auto reject = [&](std::string_view message) {
    Named(result, actor.player_id, 109U, message);
    return result;
  };
  const auto members = PartyMembers(actor.player_id);
  if (!online_players_.contains(actor.player_id) ||
      !plaza_.Contains(actor.player_id) ||
      actor.state.phase != WorldSessionPhase::kPlaza ||
      actor.state.channel == 80U || rooms_.FindRoom(actor.player_id) ||
      members.front() != actor.player_id)
    return reject("只有自由广场中的队长可以发布招募");
  if (packet.opcode == 305U && !enabled) {
    recruiting_parties_.erase(actor.player_id);
    SyncPartyPlaza(result, actor.player_id);
    result.deliveries.push_back({actor.player_id, {306U, {}}});
    return result;
  }
  if (actor.muted) return reject("账号处于禁言期间，暂时无法发布招募信息");
  if (members.size() >= kMaximumPartyMembers)
    return reject("队伍已满，无法继续招募");
  if (packet.opcode == 2031U && !recruiting_parties_.contains(actor.player_id))
    return reject("请先开启组队招募");
  if (packet.opcode != 305U) {
    const auto validated = domain::UserMessage::Parse(text);
    if (const auto* error =
            std::get_if<domain::UserMessageValidationError>(&validated))
      return reject(domain::UserMessageValidationErrorMessage(*error));
  }
  if (!ConsumeChatQuota(actor.player_id, now))
    return reject("招募操作过于频繁，请稍后再试");
  if (packet.opcode == 305U) {
    recruiting_parties_.insert(actor.player_id);
    SyncPartyPlaza(result, actor.player_id);
    result.deliveries.push_back({actor.player_id, {306U, {}}});
  } else {
    AppendPlaza(result, plaza_.RecruitmentText(actor.player_id, std::move(text),
                                               actor.chat_blocked_peers,
                                               recipient_slot));
  }
  return result;
}

WorldCoordinatorResult WorldCoordinator::SuppressRecruitment(
    domain::PlayerId player) {
  WorldCoordinatorResult result;
  if (recruiting_parties_.erase(player) != 0U) {
    AppendPlaza(result, plaza_.RecruitmentText(player, u"", {}));
    SyncPartyPlaza(result, player);
  }
  return result;
}

void WorldCoordinator::PublishParty(WorldCoordinatorResult& result,
                                    domain::PlayerId player) {
  const auto members = PartyMembers(player);
  if (std::ranges::any_of(
          members, [&](auto id) { return !online_players_.contains(id); }))
    return;
  // CB1620 / 4883D0: state, PartyMember vector, equipment vector in identical
  // member order.
  protocol::PacketWriter writer;
  writer.WriteU32(2U);
  writer.WriteU32(static_cast<std::uint32_t>(members.size()));
  for (const auto id : members) {
    const auto& snapshot = online_players_.at(id).snapshot;
    writer.WriteU32(static_cast<std::uint32_t>(id.Value()));
    writer.WriteUtf16(protocol::Utf8ToUtf16(snapshot.nickname));
    writer.WriteU32(std::numeric_limits<std::uint32_t>::max());
    writer.WriteU32(std::numeric_limits<std::uint32_t>::max());
    writer.WriteU32(0U);
    writer.WriteU32(0U);
  }
  writer.WriteU32(static_cast<std::uint32_t>(members.size()));
  for (const auto id : members) {
    writer.WriteBytes(
        protocol::EncodeEquippedItemsResponse(online_players_.at(id).snapshot));
  }
  const auto body = writer.Release();
  for (const auto id : members) result.deliveries.push_back({id, {201U, body}});
  SyncPartyPlaza(result, player);
}

void WorldCoordinator::LeaveParty(WorldCoordinatorResult& result,
                                  domain::PlayerId player, bool notify) {
  // A solo player's cancel/leave must also revoke invitations still awaiting
  // consent.
  std::erase_if(party_invitations_, [player](const auto& invitation) {
    return invitation.requester == player || invitation.target == player;
  });
  auto members = PartyMembers(player);
  recruiting_parties_.erase(player);
  if (members.size() < 2U) {
    SyncPartyPlaza(result, player);
    return;
  }
  const auto leader = members.front();
  const auto name = online_players_.contains(player)
                        ? online_players_.at(player).snapshot.nickname
                        : std::string{};
  for (const auto id : members) {
    if (id != player) Named(result, id, 205U, name);
  }
  parties_.erase(leader);
  std::erase(members, player);
  if (members.size() > 1U) parties_.emplace(members.front(), members);
  PublishParty(result, members.front());
  SyncPartyPlaza(result, player);
  if (notify) {
    PublishParty(result, player);
    result.deliveries.push_back({player, {204U, {}}});
  }
}

application::ServiceResult<WorldCoordinatorResult> WorldCoordinator::VisitRoom(
    domain::PlayerId visitor, domain::RoomId room_id,
    std::chrono::steady_clock::time_point now) {
  WorldCoordinatorResult result;
  if (HasTutorial(visitor))
    return result;  // Includes SQL-pending begin/finish; no cross-actor room
                    // migration.
  const auto found = online_players_.find(visitor);
  const auto room = rooms_.FindRoom(room_id);
  const auto scene = rooms_.Scene(room_id);
  if (found == online_players_.end() || !room ||
      room->Phase() != RoomPhase::kWaiting || !scene || scene->locked != 0U ||
      rooms_.FindRoom(visitor)) {
    return result;
  }
  auto& peer = found->second;
  WorldActorContext actor{.player_id = visitor,
                          .state = peer.state,
                          .player_snapshot = peer.snapshot};
  if (!plaza_.Contains(visitor)) {
    if (peer.state.phase != WorldSessionPhase::kMenu) return result;
    auto entered = HandleEnterPlaza(actor, {.opcode = 106U, .body = {}});
    if (const auto* error = std::get_if<application::ServiceError>(&entered))
      return *error;
    result = std::get<WorldCoordinatorResult>(std::move(entered));
    peer.state = {WorldSessionPhase::kPlaza, 0U};
    actor.state = peer.state;
  }
  protocol::PacketWriter writer;
  writer.WriteU32(static_cast<std::uint32_t>(room_id.Value()));
  writer.WriteUtf16(u"");
  writer.WriteU32(0U);
  auto joined = HandleJoin(std::move(actor),
                           {.opcode = 403U, .body = writer.Release()}, now);
  if (const auto* error = std::get_if<application::ServiceError>(&joined))
    return *error;
  auto joined_result = std::get<WorldCoordinatorResult>(std::move(joined));
  if (joined_result.room_id) {
    result.session_updates.clear();
    peer.state.phase = WorldSessionPhase::kRoomWaiting;
  }
  result.deliveries.insert(
      result.deliveries.end(),
      std::make_move_iterator(joined_result.deliveries.begin()),
      std::make_move_iterator(joined_result.deliveries.end()));
  result.session_updates.insert(result.session_updates.end(),
                                joined_result.session_updates.begin(),
                                joined_result.session_updates.end());
  result.event = joined_result.event;
  result.room_id = joined_result.room_id;
  result.active_timers = std::move(joined_result.active_timers);
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleParty(WorldActorContext actor,
                              const protocol::Packet& packet,
                              std::chrono::steady_clock::time_point now) {
  WorldCoordinatorResult result;
  protocol::PacketReader reader{packet.body};
  bool requested_party = true;
  if (packet.opcode == 109U) {
    requested_party = Flag(reader);
    // This sender hint is not authority to bypass the recipient's confirmation.
    static_cast<void>(Flag(reader));
  } else if (packet.opcode == 110U) {
    requested_party = Flag(reader);
  }
  std::string name;
  if (packet.opcode != 202U)
    name = protocol::Utf16ToUtf8(reader.ReadUtf16(12U));
  std::uint32_t answer = 0U;
  if (packet.opcode == 110U) {
    // 原版回传 NetAddress；本机世界根据已认证邀请记录定位，绝不采用客户端地址。
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU16());
  } else if (packet.opcode == 112U) {
    answer = reader.ReadU32();
    if (answer > 3U) {
      throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                    "invalid visit response"};
    }
  }
  reader.RequireEnd();
  if (!online_players_.contains(actor.player_id)) return result;
  std::erase_if(party_invitations_, [&](const auto& invitation) {
    return invitation.expires <= now;
  });
  if (packet.opcode == 202U) {
    LeaveParty(result, actor.player_id, true);
    return result;
  }
  const auto target_it =
      std::ranges::find_if(online_players_, [&](const auto& entry) {
        return entry.second.snapshot.nickname == name;
      });
  const auto response =
      static_cast<std::uint16_t>(packet.opcode == 109U   ? 114U
                                 : packet.opcode == 110U ? 116U
                                                         : 117U);
  if (target_it == online_players_.end()) {
    Status(result, actor.player_id, response, packet.opcode == 110U ? 1U : 3U,
           name);
    return result;
  }
  const auto target = target_it->first;
  if (HasTutorial(actor.player_id) || HasTutorial(target))
    return result;  // A visit/follow/party acceptance cannot steal a private
                    // tutorial slot.
  const auto& sender_name =
      online_players_.at(actor.player_id).snapshot.nickname;
  auto members = PartyMembers(actor.player_id);
  if (packet.opcode == 302U || packet.opcode == 303U) {
    if (members.front() != actor.player_id || target == actor.player_id ||
        std::ranges::find(members, target) == members.end())
      return result;
    if (packet.opcode == 302U) {
      LeaveParty(result, target, true);
    } else {
      recruiting_parties_.erase(actor.player_id);
      recruiting_parties_.erase(target);
      parties_.erase(actor.player_id);
      std::iter_swap(members.begin(), std::ranges::find(members, target));
      parties_.emplace(target, std::move(members));
      PublishParty(result, target);
    }
    return result;
  }
  // Native public join (9C29A0 -> 9D3910) shares C2S110 with invitation
  // acceptance. Only an authoritative, same-plaza recruitment sign permits the
  // no-invitation path.
  if ((packet.opcode == 110U || packet.opcode == 111U) && requested_party &&
      recruiting_parties_.contains(target)) {
    auto lead_members = PartyMembers(target);
    const auto source_scene = plaza_.Scene(actor.player_id);
    const auto target_scene = plaza_.Scene(target);
    if (target == actor.player_id || !source_scene || !target_scene ||
        source_scene->scene_id != target_scene->scene_id ||
        actor.state.phase != WorldSessionPhase::kPlaza ||
        target_it->second.state.phase != WorldSessionPhase::kPlaza ||
        actor.state.channel == 80U || target_it->second.state.channel == 80U ||
        rooms_.FindRoom(actor.player_id) || rooms_.FindRoom(target) ||
        lead_members.front() != target ||
        lead_members.size() >= kMaximumPartyMembers || members.size() != 1U ||
        std::ranges::find(actor.chat_blocked_peers, target) !=
            actor.chat_blocked_peers.end()) {
      Status(result, actor.player_id, response,
             packet.opcode == 110U ? 7U : 10U, name);
      return result;
    }
    if (!ConsumeChatQuota(actor.player_id, now)) return result;
    recruiting_parties_.erase(actor.player_id);
    std::erase_if(party_invitations_, [&](const auto& pending) {
      return pending.requester == actor.player_id ||
             pending.target == actor.player_id;
    });
    lead_members.push_back(actor.player_id);
    parties_.insert_or_assign(target, lead_members);
    PublishParty(result, target);
    for (const auto id : lead_members) Named(result, id, 206U, sender_name);
    Status(result, actor.player_id, response, 0U, name);
    return result;
  }
  if (packet.opcode == 109U || packet.opcode == 111U) {
    if (target == actor.player_id) {
      Status(result, actor.player_id, response, 1U, name);
      return result;
    }
    if (!ConsumeChatQuota(actor.player_id, now)) return result;
    if (std::ranges::find(actor.chat_blocked_peers, target) !=
        actor.chat_blocked_peers.end()) {
      Status(result, actor.player_id, response, 3U, name);
      return result;
    }
    // 跨家族频道需要 clan_id 级凭证；当前原生组队仅开放自由频道。
    if (actor.state.channel == 80U || target_it->second.state.channel == 80U) {
      Status(result, actor.player_id, response, 9U, name);
      return result;
    }
    const auto leader = packet.opcode == 109U ? actor.player_id : target;
    const auto visitor = packet.opcode == 109U ? target : actor.player_id;
    const auto lead_members = PartyMembers(leader);
    const auto visitor_members = PartyMembers(visitor);
    const auto room = rooms_.FindRoom(leader);
    const auto scene = room ? rooms_.Scene(room->RoomId()) : std::nullopt;
    if ((room && (room->Phase() != RoomPhase::kWaiting || !scene ||
                  scene->locked != 0U)) ||
        rooms_.FindRoom(visitor)) {
      Status(result, actor.player_id, response, 4U, name);
      return result;
    }
    if (lead_members.front() != leader ||
        lead_members.size() >= kMaximumPartyMembers ||
        visitor_members.size() != 1U || (!requested_party && !room)) {
      Status(result, actor.player_id, response, 7U, name);
      return result;
    }
    if (party_invitations_.size() >= 4096U ||
        std::ranges::count_if(party_invitations_,
                              [target](const auto& invitation) {
                                return invitation.target == target;
                              }) >= 16)
      return result;
    const bool visit = packet.opcode == 111U;
    if (std::ranges::any_of(party_invitations_, [&](const auto& invitation) {
          return invitation.requester == actor.player_id &&
                 invitation.target == target;
        }))
      return result;
    party_invitations_.push_back(
        {actor.player_id, target, requested_party, visit,
         room ? std::optional{room->RoomId()} : std::nullopt,
         now + std::chrono::seconds(30)});
    if (visit) {
      Status(result, target, 117U, 17U, sender_name);
    } else {
      protocol::PacketWriter writer;
      writer.WriteU32(requested_party ? 1U : 0U);
      writer.WriteUtf16(protocol::Utf8ToUtf16(sender_name));
      writer.WriteU32(0U);
      writer.WriteU16(0U);
      // B9E2E0 -> B67B10: true skips the dialog and immediately sends C2S110.
      // Ordinary invitations must always take the recipient's confirmation
      // branch.
      writer.WriteU32(0U);
      result.deliveries.push_back({target, {115U, writer.Release()}});
      Status(result, actor.player_id, 114U, 0U, name);
    }
    return result;
  }
  const auto invitation =
      std::ranges::find_if(party_invitations_, [&](const auto& pending) {
        return pending.target == actor.player_id &&
               pending.requester == target &&
               (packet.opcode == 112U ||
                (!pending.visit && pending.party == requested_party));
      });
  if (invitation == party_invitations_.end()) {
    Status(result, actor.player_id, response, packet.opcode == 110U ? 7U : 10U,
           name);
    return result;
  }
  const auto pending = *invitation;
  party_invitations_.erase(invitation);
  if (packet.opcode == 112U && (!pending.visit || answer != 1U)) {
    Status(result, target, 117U, pending.visit ? 18U : 21U, sender_name);
    return result;
  }
  const auto leader = pending.visit ? actor.player_id : target;
  const auto visitor = pending.visit ? target : actor.player_id;
  auto lead_members = PartyMembers(leader);
  const auto room = rooms_.FindRoom(leader);
  const auto current_room = room ? std::optional{room->RoomId()} : std::nullopt;
  const auto scene = room ? rooms_.Scene(room->RoomId()) : std::nullopt;
  if (lead_members.front() != leader ||
      lead_members.size() >= kMaximumPartyMembers ||
      PartyMembers(visitor).size() != 1U || rooms_.FindRoom(visitor) ||
      current_room != pending.room ||
      (room && (room->Phase() != RoomPhase::kWaiting || !scene ||
                scene->locked != 0U)) ||
      online_players_.at(leader).state.channel == 80U ||
      online_players_.at(visitor).state.channel == 80U ||
      std::ranges::find(actor.chat_blocked_peers, target) !=
          actor.chat_blocked_peers.end()) {
    Status(result, visitor, pending.visit ? 117U : 116U,
           pending.visit ? 10U : 7U,
           online_players_.at(leader).snapshot.nickname);
    return result;
  }
  if (room) {
    auto joined = VisitRoom(visitor, room->RoomId(), now);
    if (const auto* error = std::get_if<application::ServiceError>(&joined))
      return *error;
    result = std::get<WorldCoordinatorResult>(std::move(joined));
    if (!result.room_id) return result;
  }
  if (pending.party) {
    recruiting_parties_.erase(visitor);
    lead_members.push_back(visitor);
    parties_.insert_or_assign(leader, lead_members);
    PublishParty(result, leader);
    for (const auto id : lead_members) {
      Named(result, id, 206U, online_players_.at(visitor).snapshot.nickname);
    }
  }
  Status(result, visitor, pending.visit ? 117U : 116U, 0U,
         online_players_.at(leader).snapshot.nickname);
  return result;
}

application::ServiceResult<WorldCoordinatorResult>
WorldCoordinator::HandleRoomKick(WorldActorContext actor,
                                 const protocol::Packet& packet,
                                 std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{packet.body};
  const auto name = protocol::Utf16ToUtf8(reader.ReadUtf16(12U));
  reader.RequireEnd();
  const auto reject = [&](std::string_view message) {
    WorldCoordinatorResult result;
    Named(result, actor.player_id, 109U, message);
    return result;
  };
  const auto room = rooms_.FindRoom(actor.player_id);
  if (!room || room->Phase() != RoomPhase::kWaiting ||
      room->HostPlayerId() != actor.player_id)
    return reject("只有等待房间的房主可以踢出玩家");
  // Resolve only among current occupants, never an arbitrary online nickname.
  std::optional<application::PlayerMenuSnapshot> target;
  for (const auto& member : room->Members()) {
    auto snapshot = rooms_.PlayerSnapshot(member.player_id);
    if (snapshot && snapshot->nickname == name) {
      target = std::move(snapshot);
      break;
    }
  }
  if (!target || target->player_id == actor.player_id)
    return reject("目标玩家已离开房间，或目标为本人");
  const auto members = PartyMembers(actor.player_id);
  if (std::ranges::find(members, target->player_id) != members.end())
    return reject("同队伍成员请先通过队伍菜单退队或移出队伍");
  const auto peer = online_players_.find(target->player_id);
  if (peer == online_players_.end()) return reject("目标玩家已经离线");
  const auto victim = target->player_id;
  auto removed = HandleLeaveRoom({.player_id = victim,
                                  .state = peer->second.state,
                                  .player_snapshot = std::move(target)},
                                 {.opcode = 404U, .body = {}}, now);
  if (auto* result = std::get_if<WorldCoordinatorResult>(&removed)) {
    Named(*result, victim, 109U, "你已被房主移出房间");
    std::erase_if(party_invitations_, [victim](const auto& pending) {
      return pending.requester == victim || pending.target == victim;
    });
  }
  return removed;
}

}  // namespace bf::runtime
