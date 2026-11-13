#include <algorithm>

#include "network/room_gateway_state.h"
#include "protocol/battle/combat_messages.h"

namespace bf::infrastructure::network {
namespace asio = boost::asio;
using Json = nlohmann::json;
using application::ServiceError;
using application::ServiceResult;

void RoomGateway::Impl::BeginStart(const std::shared_ptr<Room>& room,
                                   Json request) {
  if (room->starting || room->dead || room->match) {
    FailRoom(room);
    return;
  }
  room->blocked = room->starting = true;
  room->activation = request.at("activation");
  application::MatchPreparationRequest preparation{
      .room_id = room->identity.room_id,
      .mode_id = request.at("mode_id").get<std::uint32_t>(),
      .restricted_items =
          request.at("restricted_items").get<std::vector<std::uint32_t>>()};
  for (const auto& row : request.at("participants"))
    preparation.participants.push_back(
        {domain::PlayerId{row.at("player_id").get<std::uint64_t>()},
         row.at("seat_index").get<std::uint8_t>(),
         row.at("team_code").get<std::uint8_t>(),
         row.at("actor_object_id").get<std::uint32_t>()});
  room->viewers.clear();
  for (const auto& row : request.at("viewers")) {
    const auto player =
        domain::PlayerId{row.at("player_id").get<std::uint64_t>()};
    if (!room->members.contains(player)) {
      FailRoom(room);
      return;
    }
    room->viewers.push_back({player, row.at("seat").get<std::uint8_t>(),
                             row.at("nickname").get<std::string>()});
  }
  const auto id = ids.Next();
  if (const auto* error = std::get_if<ServiceError>(&id)) {
    static_cast<void>(error);
    FailRoom(room);
    return;
  }
  room->match = std::get<domain::MatchId>(id);
  {
    std::scoped_lock lock{mutex};
    matches.insert_or_assign(room->match->ToUuidString(), room);
  }
  room->start_in_flight = true;
  Database(
      [self = shared_from_this(), preparation] {
        return self->preparation_executor.Prepare(preparation);
      },
      [self = shared_from_this(), room](auto result) {
        room->start_in_flight = false;
        if (room->dead) return;
        const auto* prepared = std::get_if<application::PreparedMatch>(&result);
        if (!prepared) {
          self->FailRoom(room);
          return;
        }
        room->prepared = *prepared;
        application::MatchStartCommand command{
            .match_id = *room->match,
            .source_room_id = room->identity.room_id,
            .mode_id = prepared->mode_id,
            .configuration_version_id = prepared->configuration_version_id,
            .reward_rules_version_id = prepared->reward_rules_version_id,
            .score_model = prepared->score_model,
            .started_at = self->clock.Now(),
            .source_channel = room->channel,
            .room_owner = self->Owner(room)};
        for (const auto& member : prepared->participants)
          command.participants.push_back({member.identity.player_id,
                                          member.identity.seat_index,
                                          member.identity.team_code});
        room->start_command = std::move(command);
        self->PersistStart(room);
      });
}

void RoomGateway::Impl::PersistStart(const std::shared_ptr<Room>& room) {
  if (room->dead || !room->start_command || room->start_in_flight) return;
  room->start_in_flight = true;
  Database(
      [self = shared_from_this(), command = *room->start_command] {
        return self->starts.Start(command);
      },
      [self = shared_from_this(), room](auto result) {
        room->start_in_flight = false;
        if (room->dead) return;
        if (const auto* error = std::get_if<ServiceError>(&result)) {
          if (error->is_retryable ||
              error->code == application::ErrorCode::kStorageUnavailable) {
            room->next_retry =
                std::chrono::steady_clock::now() + std::chrono::seconds{1};
          } else
            self->FailRoom(room);
          return;
        }
        self->Bootstrap(room);
      });
}

void RoomGateway::Impl::Bootstrap(const std::shared_ptr<Room>& room) {
  try {
    // The native client must create every actor with 701 before the 601 relay
    // invitation.
    Deliver(room, ReadRoomResult(room->activation));
    if (!options.equipment_relay) {
      Install(room);
      return;
    }
    auto issued = options.equipment_relay->Begin(*room->prepared, room->channel,
                                                 room->viewers);
    if (std::holds_alternative<ServiceError>(issued)) {
      FailRoom(room);
      return;
    }
    runtime::RoomCoordinatorResult invitation;
    for (const auto& row :
         std::get<std::vector<runtime::EquipmentRelayInvitation>>(issued))
      invitation.deliveries.push_back(
          {row.player,
           {601U, protocol::EncodeEquipmentRelayEndpoint(
                      options.equipment_relay_address,
                      options.equipment_relay_port, row.key)}});
    Deliver(room, std::move(invitation));
    room->waiting_equipment = true;
    room->equipment_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{30};
  } catch (...) {
    FailRoom(room);
  }
}

void RoomGateway::Impl::Install(const std::shared_ptr<Room>& room) {
  room->waiting_equipment = false;
  room->start_in_flight = true;
  runtime::RoomMatchInstallation installation{
      .match_id = room->match,
      .score_model = room->prepared->score_model,
      .started_at = std::chrono::steady_clock::now()};
  for (const auto& member : room->prepared->participants)
    installation.participants.push_back(
        {.player_id = member.identity.player_id,
         .actor_object_id = member.identity.actor_object_id,
         .team_code = member.identity.team_code,
         .weapon_templates = member.weapon_templates,
         .ordinary_respawn_allowed = room->prepared->ordinary_respawn_allowed,
         .seat_index = member.identity.seat_index,
         .lucci_pouch = member.lucci_pouch,
         .chocolate_box = member.chocolate_box,
         .fighter = member.fighter,
         .equipment_items = member.equipment_items});
  Rpc(room,
      {{"op", "install"},
       {"installation", WriteRoomInstallation(installation)}},
      [self = shared_from_this(), room](auto result) {
        room->start_in_flight = false;
        if (room->dead) return;
        if (const auto* value = std::get_if<Json>(&result)) {
          room->starting = false;
          self->Apply(room, *value);
          if (value->contains("initial"))
            self->Apply(room, value->at("initial"));
          room->start_command.reset();
          room->prepared.reset();
          room->blocked = false;
          self->Pump(room);
        } else
          self->FailRoom(room);
      });
}

void RoomGateway::Impl::BeginTerminal(const std::shared_ptr<Room>& room) {
  if (room->dead || !room->terminal || room->terminal_in_flight) return;
  room->terminal_in_flight = true;
  const auto command = ReadRoomTerminal(room->terminal->at("settlement"));
  Database([self = shared_from_this(),
            command] { return self->settlements.Submit(command); },
           [self = shared_from_this(), room](auto staged) {
             room->terminal_in_flight = false;
             if (room->dead) return;
             if (std::holds_alternative<ServiceError>(staged)) {
               room->next_retry =
                   std::chrono::steady_clock::now() + std::chrono::seconds{1};
               return;
             }
             self->Rpc(room, {{"op", "settlement_staged"}},
                       [self, room](auto reply) {
                         if (room->dead) return;
                         if (std::holds_alternative<ServiceError>(reply)) {
                           self->FailRoom(room);
                           return;
                         }
                         auto terminal = ReadRoomResult(*room->terminal);
                         terminal.settlement_command.reset();
                         room->terminal.reset();
                         room->terminal_staged = true;
                         self->Deliver(room, std::move(terminal));
                         room->blocked = false;
                         if (room->early_reward)
                           self->Reward(room, std::move(*room->early_reward));
                         room->early_reward.reset();
                         if (room->closed)
                           self->Retire(room, false);
                         else
                           self->Pump(room);
                       });
           });
}

void RoomGateway::Impl::Reward(const std::shared_ptr<Room>& room,
                               application::MatchSettlementResult result) {
  if (room->dead || !room->match || result.match_id != *room->match) return;
  if (!room->terminal_staged) {
    room->early_reward = std::move(result);
    return;
  }
  Rpc(room,
      {{"op", "settlement_result"},
       {"settlement", WriteRoomSettlement(result)}},
      [self = shared_from_this(), room](auto result) {
        if (const auto* value = std::get_if<Json>(&result))
          self->Apply(room, *value);
      });
}

bool RoomGateway::SettlementResult(application::MatchSettlementResult result) {
  std::shared_ptr<Impl::Room> room;
  {
    std::scoped_lock lock{impl_->mutex};
    if (const auto found = impl_->matches.find(result.match_id.ToUuidString());
        found != impl_->matches.end())
      room = found->second.lock();
  }
  if (!room) return false;
  asio::post(impl_->io,
             [self = impl_, room, result = std::move(result)]() mutable {
               self->Reward(room, std::move(result));
             });
  return true;
}
}  // namespace bf::infrastructure::network
