#include <algorithm>

#include "network/room_gateway_state.h"

namespace bf::infrastructure::network {
namespace asio = boost::asio;
using Json = nlohmann::json;
using application::ServiceError;
using application::ServiceResult;

void RoomGateway::Impl::Equipment(const std::shared_ptr<Room>& room,
                                  domain::PlayerId player,
                                  runtime::EquipmentWorldTask work, Done done,
                                  bool preset) {
  if (room->dead || !room->members.contains(player) ||
      !Current(room->members.at(player).actor)) {
    done(Failure("equipment session expired"));
    Finish(room);
    return;
  }
  Rpc(room,
      {{"op", "business_begin"},
       {"business_player", player.Value()},
       {"preset", preset}},
      [self = shared_from_this(), room, player, work, done](auto reply) {
        const auto* value = std::get_if<Json>(&reply);
        if (!value || room->dead) {
          done(Failure("equipment reservation failed"));
          self->Finish(room);
          return;
        }
        const bool allowed = value->value("allowed", false);
        const auto mode = value->value("combat_mode", 0U);
        self->Database(
            [work, allowed,
             mode]() -> ServiceResult<runtime::EquipmentWorldResult> {
              return work(allowed, mode);
            },
            [self, room, player, done](auto result) {
              if (room->dead) {
                done(Failure("equipment room closed after transaction"));
                self->Finish(room);
                return;
              }
              Json release{{"op", "business_end"}};
              std::vector<runtime::OutboundMessage> replies;
              std::optional<ServiceError> error;
              if (const auto* rejected = std::get_if<ServiceError>(&result))
                error = *rejected;
              else {
                auto& value = std::get<runtime::EquipmentWorldResult>(result);
                replies = std::move(value.replies);
                if (value.changed) {
                  release["player"] = WriteRoomPlayer(*value.changed);
                  release["level_icon"] = value.level_icon_only;
                  if (const auto found = room->members.find(player);
                      found != room->members.end())
                    found->second.actor.player_snapshot = *value.changed;
                }
              }
              self->Rpc(
                  room, std::move(release),
                  [self, room, player, replies = std::move(replies), error,
                   done](auto released) mutable {
                    if (const auto* value = std::get_if<Json>(&released)) {
                      auto updates = ReadRoomResult(*value);
                      // Sender's committed inventory receipt must precede its
                      // new room appearance.
                      for (auto iterator = updates.deliveries.begin();
                           iterator != updates.deliveries.end();) {
                        if (iterator->recipient == player) {
                          replies.push_back(std::move(iterator->message));
                          iterator = updates.deliveries.erase(iterator);
                        } else
                          ++iterator;
                      }
                      self->Deliver(room, std::move(updates));
                      if (error)
                        done(*error);
                      else
                        done(std::move(replies));
                    } else {
                      self->FailRoom(room);
                      done(Failure("equipment commit synchronization failed"));
                    }
                    self->Finish(room);
                  });
            });
      });
}

void RoomGateway::Impl::Respawn(const std::shared_ptr<Room>& room,
                                domain::PlayerId player, std::uint32_t object,
                                runtime::InstantRespawnWorldTask work,
                                Done done) {
  if (room->dead || !room->members.contains(player) ||
      !Current(room->members.at(player).actor)) {
    done(Failure("respawn session expired"));
    Finish(room);
    return;
  }
  Rpc(room,
      {{"op", "respawn_begin"},
       {"business_player", player.Value()},
       {"object", object}},
      [self = shared_from_this(), room, player, work, done](auto reply) {
        const auto* value = std::get_if<Json>(&reply);
        if (!value || !value->value("allowed", false) || room->dead) {
          done(std::vector<runtime::OutboundMessage>{});
          self->Finish(room);
          return;
        }
        const auto bytes =
            value->at("match_id").get<domain::MatchId::ByteArray>();
        application::UseItemCommand command{
            .player_id = player,
            .item_id =
                domain::ItemId{value->at("item_id").get<std::uint64_t>()},
            .operation = application::ItemUseOperation::kInstantRespawn,
            .match_id = domain::MatchId::FromBytes(bytes),
            .life_generation =
                value->at("life_generation").get<std::uint64_t>(),
            .room_owner = self->Owner(room)};
        self->Database(
            [work,
             command]() -> ServiceResult<runtime::InstantRespawnWorldResult> {
              return work(command);
            },
            [self, room, done](auto result) {
              if (room->dead) {
                done(Failure("respawn room closed after transaction"));
                self->Finish(room);
                return;
              }
              const auto* value =
                  std::get_if<runtime::InstantRespawnWorldResult>(&result);
              self->Rpc(
                  room,
                  {{"op", "respawn_end"},
                   {"authorized", value && value->authorized}},
                  [self, room, result = std::move(result),
                   done](auto released) mutable {
                    if (std::holds_alternative<ServiceError>(released)) {
                      self->FailRoom(room);
                      done(Failure("respawn commit synchronization failed"));
                    } else if (const auto* error =
                                   std::get_if<ServiceError>(&result))
                      done(*error);
                    else
                      done(std::move(
                          std::get<runtime::InstantRespawnWorldResult>(result)
                              .replies));
                    self->Finish(room);
                  });
            });
      });
}

std::optional<ServiceError> RoomGateway::Equipment(
    domain::PlayerId player, runtime::EquipmentWorldTask work,
    runtime::WorldCommandCompletion completion, bool preset,
    std::optional<runtime::SessionHandle> origin) {
  const auto room = impl_->Find(player);
  const auto admission = impl_->Admit(player, 65536U);
  if (!room || !admission || !work || !completion)
    return Impl::Failure("equipment room unavailable");
  auto done = [admission, completion](auto result) {
    completion(std::move(result));
  };
  asio::post(impl_->io,
             [self = impl_, room, player, work, done, preset, origin] {
               const auto found = room->members.find(player);
               if (found == room->members.end() ||
                   (origin && found->second.origin != *origin)) {
                 done(Impl::Failure("equipment origin expired"));
                 return;
               }
               self->Enqueue(room, [self, room, player, work, done, preset] {
                 self->Equipment(room, player, work, done, preset);
               });
             });
  return std::nullopt;
}

std::optional<ServiceError> RoomGateway::InstantRespawn(
    domain::PlayerId player, std::uint32_t object,
    runtime::InstantRespawnWorldTask work,
    runtime::WorldCommandCompletion completion) {
  const auto room = impl_->Find(player);
  const auto admission = impl_->Admit(player, 4096U);
  if (!room || !admission || !work || !completion)
    return Impl::Failure("respawn room unavailable");
  auto done = [admission, completion](auto result) {
    completion(std::move(result));
  };
  asio::post(impl_->io, [self = impl_, room, player, object, work, done] {
    self->Enqueue(room, [self, room, player, object, work, done] {
      self->Respawn(room, player, object, work, done);
    });
  });
  return std::nullopt;
}
}  // namespace bf::infrastructure::network
