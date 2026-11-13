#include <algorithm>

#include "network/room_gateway_state.h"
#include "protocol/codec/packet_io.h"
#include "protocol/rooms/room_messages.h"

namespace bf::infrastructure::network {
namespace asio = boost::asio;
using application::ServiceError;
using application::ServiceResult;
using protocol::internal::RoomEnvelope;
using protocol::internal::RoomMessageKind;
using Json = nlohmann::json;

Json RoomGateway::Impl::EncodePacket(const runtime::WorldActorContext& actor,
                                     const protocol::Packet& packet) {
  Json value{{"op", "packet"},
             {"opcode", packet.opcode},
             {"body", packet.body},
             {"peer_address", actor.peer_address}};
  if (actor.player_snapshot)
    value["player"] = WriteRoomPlayer(*actor.player_snapshot);
  return value;
}

void RoomGateway::Impl::EntryFailed(domain::PlayerId player,
                                    std::uint16_t opcode, Done done,
                                    std::string reason) {
  {
    std::scoped_lock lock{mutex};
    pending_players.erase(player);
  }
  done(std::vector<runtime::OutboundMessage>{
      {static_cast<std::uint16_t>(opcode == 402U ? 102U : 103U),
       opcode == 402U ? protocol::EncodeCreateRoomResponse(1U)
                      : protocol::EncodeJoinRoomResponse(1U)},
      Notice(std::move(reason))});
}

void RoomGateway::Impl::Handoff(const std::shared_ptr<Room>& room,
                                domain::PlayerId player, bool entering,
                                Done done) {
  const auto found = room->members.find(player);
  if (found == room->members.end() || !Current(found->second.actor)) {
    done(Failure("room handoff session expired"));
    return;
  }
  const auto origin = found->second.origin;
  found->second.handing_off = entering;
  ++external;
  const auto rejected = lobby->PostRoomHandoff(
      found->second.actor, entering,
      [self = shared_from_this(), room, player, origin, entering,
       done](auto result) mutable {
        asio::post(self->io, [self, room, player, origin, entering, done,
                              result = std::move(result)]() mutable {
          if (auto member = room->members.find(player);
              member != room->members.end() &&
              member->second.origin == origin) {
            member->second.handing_off = false;
            if (!std::holds_alternative<ServiceError>(result))
              member->second.handed_off = entering;
            if (entering && room->dead) self->ReturnPlayer(room, player);
          }
          done(std::move(result));
          self->FinishExternal();
        });
      });
  if (rejected) {
    found->second.handing_off = false;
    done(*rejected);
    FinishExternal();
  }
}

void RoomGateway::Impl::ForgetPlayer(const std::shared_ptr<Room>& room,
                                     domain::PlayerId player) {
  room->members.erase(player);
  std::scoped_lock lock{mutex};
  pending_players.erase(player);
  if (const auto found = player_rooms.find(player);
      found != player_rooms.end() && found->second == room)
    player_rooms.erase(found);
}

void RoomGateway::Impl::ReturnPlayer(const std::shared_ptr<Room>& room,
                                     domain::PlayerId player) {
  const auto found = room->members.find(player);
  if (found == room->members.end()) return;
  const auto origin = found->second.origin;
  if (found->second.handing_off && !stopping) return;
  if (!found->second.handed_off || !Current(found->second.actor) || stopping) {
    ForgetPlayer(room, player);
    return;
  }
  if (!room->returning.insert(player).second) return;
  Database(
      [self = shared_from_this(), player] { return self->menus.Load(player); },
      [self = shared_from_this(), room, player, origin](auto loaded) {
        const auto member = room->members.find(player);
        if (member == room->members.end() || member->second.origin != origin)
          return;
        if (const auto* snapshot =
                std::get_if<application::PlayerMenuSnapshot>(&loaded))
          member->second.actor.player_snapshot = *snapshot;
        self->Handoff(room, player, false,
                      [self, room, player, origin](auto result) {
                        if (std::holds_alternative<ServiceError>(result))
                          self->sessions.DisconnectIfCurrent(player, origin);
                        self->ForgetPlayer(room, player);
                      });
      });
}

void RoomGateway::Impl::Retire(const std::shared_ptr<Room>& room, bool failed) {
  if (room->dead) return;
  room->dead = true;
  room->node->reservations.erase(room->identity.instance_id);
  rooms.erase(room->identity.room_id);
  {
    std::scoped_lock lock{mutex};
    if (room->match) matches.erase(room->match->ToUuidString());
    if (failed) ++metrics.failed_rooms;
  }
  if (options.equipment_relay)
    options.equipment_relay->RevokeRoom(room->identity.room_id);
  if (room->rpc) room->rpc->Close();
  auto queue = std::move(room->queue);
  for (auto& work : queue) work.execute();
  if (!stopping) {
    const auto ownership = Owner(room);
    const auto release = [self = shared_from_this(), ownership] {
      self->Database(
          [self, ownership] { return self->leases.ReleaseRoom(ownership); },
          [](auto) {});
    };
    if (room->node->connected) {
      RoomEnvelope request;
      request.identity = room->identity;
      const std::string text = R"({"op":"release"})";
      request.payload.assign(text.begin(), text.end());
      static_cast<void>(
          room->node->rpc->Request(std::move(request), [release](auto result) {
            if (const auto* envelope = std::get_if<RoomEnvelope>(&result)) {
              try {
                if (ParseRoomPayload(envelope->payload).value("ok", false))
                  release();
              } catch (...) {
              }
            }
          }));
    }
  }
}

void RoomGateway::Impl::FailRoom(const std::shared_ptr<Room>& room) {
  if (room->dead) return;
  logger.Log(application::LogLevel::kWarning,
             "dedicated room interrupted: room=" +
                 std::to_string(room->identity.room_id.Value()) +
                 " epoch=" + std::to_string(room->identity.owner_epoch));
  Retire(room, true);
  const auto members = room->members;
  for (const auto& [player, member] : members) {
    if (member.handed_off)
      static_cast<void>(sessions.DispatchIfCurrent(
          player, member.origin,
          std::vector<runtime::OutboundMessage>{
              {119U, protocol::EncodeLeaveRoomResponse()},
              Notice("房间已中断，正在返回大厅")}));
    ReturnPlayer(room, player);
  }
}

void RoomGateway::Impl::Create(runtime::WorldActorContext actor,
                               protocol::Packet packet, Done done) {
  const auto player = actor.player_id;
  const auto request = protocol::DecodeCreateRoomRequest(packet.body);
  bool draining;
  {
    std::scoped_lock lock{mutex};
    draining = metrics.draining;
  }
  if (!Current(actor) || !actor.player_snapshot ||
      actor.state.phase != runtime::WorldSessionPhase::kPlaza || draining ||
      rooms.size() + allocating >= options.maximum_rooms ||
      request.requested_scene.configuration.mode_id != 1U ||
      request.requested_scene.configuration.submode_id == 4U) {
    EntryFailed(player, packet.opcode, done,
                "独立房间当前开放常规团队战，请确认频道、模式和节点容量");
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::shared_ptr<Node> selected;
  auto score = [](const auto& node) {
    const auto memory =
        node->budget ? node->memory * 1000U / node->budget : 1000U;
    return node->cpu * 4U + memory * 2U + node->handles / 64U +
           (node->room_count + node->reservations.size()) * 10U;
  };
  for (const auto& node : nodes)
    if (node->connected && node->registered && !node->retired &&
        !node->draining && node->valid_until > now + std::chrono::seconds{3} &&
        node->cpu < 700U && node->memory < node->budget &&
        node->available_rooms > node->reservations.size() &&
        (!selected || score(node) < score(selected)))
      selected = node;
  if (!selected) {
    EntryFailed(player, 402U, done, "房间节点容量不足，请稍后重试");
    return;
  }
  const auto next = ids.Next();
  if (const auto* error = std::get_if<ServiceError>(&next)) {
    EntryFailed(player, 402U, done, error->safe_message);
    return;
  }
  auto room = std::make_shared<Room>();
  room->node = selected;
  room->channel = actor.state.channel;
  room->allocation_started = now;
  room->members.emplace(
      player,
      Member{{actor.orig_session->first, actor.orig_session->second}, actor});
  const auto instance = std::get<domain::MatchId>(next);
  const auto node_id = *selected->id;
  selected->reservations.insert(instance.Bytes());
  ++allocating;
  Database(
      [self = shared_from_this(), node_id, instance] {
        return self->leases.AllocateRoom(node_id, instance);
      },
      [self = shared_from_this(), room, player, instance, node_id,
       packet = std::move(packet), done, now](auto result) mutable {
        --self->allocating;
        const auto* lease = std::get_if<application::RoomLease>(&result);
        if (!lease) {
          room->node->reservations.erase(instance.Bytes());
          self->EntryFailed(player, 402U, done, "房间租约分配失败，请稍后重试");
          return;
        }
        room->identity = {lease->room_id, lease->owner.instance_id.Bytes(),
                          lease->owner.owner_epoch};
        room->lease_until = now + std::chrono::seconds{8};
        self->rooms.emplace(room->identity.room_id, room);
        {
          std::scoped_lock lock{self->mutex};
          self->player_rooms.insert_or_assign(player, room);
          self->pending_players.erase(player);
        }
        if (self->stopping || !self->Current(room->members.at(player).actor) ||
            room->node->id != node_id || !room->node->connected ||
            !room->node->registered || room->node->retired ||
            room->node->draining) {
          self->FailRoom(room);
          done(Failure("room allocation session expired"));
          return;
        }
        self->Allocate(room, player, std::move(packet), done);
      });
}

void RoomGateway::Impl::Allocate(const std::shared_ptr<Room>& room,
                                 domain::PlayerId player,
                                 protocol::Packet packet, Done done) {
  if (room->dead || stopping ||
      std::chrono::steady_clock::now() >=
          room->allocation_started + std::chrono::seconds{6}) {
    FailRoom(room);
    EntryFailed(player, 402U, done, "房间进程启动超时或节点容量不足");
    return;
  }
  RoomEnvelope request;
  request.identity = room->identity;
  const std::string text = R"({"op":"allocate"})";
  request.payload.assign(text.begin(), text.end());
  const bool accepted = room->node->rpc->Request(
      std::move(request),
      [self = shared_from_this(), room, player, packet = std::move(packet),
       done](auto reply) mutable {
        const auto* envelope = std::get_if<RoomEnvelope>(&reply);
        if (!envelope || room->dead) {
          self->FailRoom(room);
          self->EntryFailed(player, 402U, done, "房间节点连接不可用");
          return;
        }
        try {
          const auto result = ParseRoomPayload(envelope->payload);
          if (!result.value("ok", false)) {
            auto delay = std::make_shared<asio::steady_timer>(
                self->io, std::chrono::milliseconds{100});
            delay->async_wait([self, room, player, packet = std::move(packet),
                               done, delay](auto error) mutable {
              if (!error) self->Allocate(room, player, std::move(packet), done);
            });
            return;
          }
          auto tls = self->options.tls;
          tls.peer_name = room->node->endpoint.peer_name;
          room->rpc = std::make_shared<RoomRpcClient>(self->io, tls);
          const auto port = result.at("port").get<unsigned>();
          const auto host = result.at("host").get<std::string>();
          if (!port || port > 65535U || host.empty() || host.size() > 253U)
            throw std::invalid_argument{"invalid DS endpoint"};
          room->rpc->Connect(
              host, static_cast<std::uint16_t>(port),
              [self, room, player, packet = std::move(packet),
               done](bool connected) mutable {
                if (!connected || room->dead) {
                  self->FailRoom(room);
                  self->EntryFailed(player, 402U, done, "独立房间握手失败");
                  return;
                }
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        room->lease_until - std::chrono::steady_clock::now())
                        .count();
                self->Rpc(
                    room,
                    {{"op", "bind"},
                     {"capacity", self->options.room_capacity},
                     {"lease_ms", remaining}},
                    [self, room, player, packet = std::move(packet),
                     done](auto bound) mutable {
                      if (std::holds_alternative<ServiceError>(bound)) {
                        self->FailRoom(room);
                        self->EntryFailed(player, 402U, done,
                                          "独立房间绑定失败");
                        return;
                      }
                      room->ready = true;
                      self->Handoff(
                          room, player, true,
                          [self, room, player, packet = std::move(packet),
                           done](auto detached) mutable {
                            if (std::holds_alternative<ServiceError>(
                                    detached) ||
                                room->dead) {
                              self->FailRoom(room);
                              self->EntryFailed(player, 402U, done,
                                                "进入独立房间失败");
                              return;
                            }
                            self->Enqueue(room, [self, room, player,
                                                 packet = std::move(packet),
                                                 done]() mutable {
                              self->Packet(room, room->members.at(player).actor,
                                           std::move(packet), done);
                            });
                          });
                    },
                    RoomMessageKind::kBind);
              },
              [self, room](auto event) {
                self->Receive(room, std::move(event));
              },
              [self, room] { self->FailRoom(room); });
        } catch (...) {
          self->FailRoom(room);
          self->EntryFailed(player, 402U, done, "房间节点返回无效数据");
        }
      });
  if (!accepted) {
    FailRoom(room);
    EntryFailed(player, 402U, done, "房间节点队列已满");
  }
}

void RoomGateway::Impl::Join(runtime::WorldActorContext actor,
                             protocol::Packet packet, Done done) {
  const auto request = protocol::DecodeJoinRoomRequest(packet.body);
  const auto found = rooms.find(domain::RoomId{request.scene_id});
  bool draining;
  {
    std::scoped_lock lock{mutex};
    draining = metrics.draining;
  }
  if (draining || !Current(actor) ||
      actor.state.phase != runtime::WorldSessionPhase::kPlaza ||
      !actor.player_snapshot || found == rooms.end() || found->second->dead ||
      !found->second->ready || found->second->channel != actor.state.channel) {
    EntryFailed(actor.player_id, 403U, done, "房间不存在、已关闭或频道不匹配");
    return;
  }
  const auto room = found->second;
  const auto player = actor.player_id;
  const runtime::SessionHandle origin{actor.orig_session->first,
                                      actor.orig_session->second};
  room->members.emplace(player, Member{origin, actor});
  {
    std::scoped_lock lock{mutex};
    player_rooms.emplace(player, room);
    pending_players.erase(player);
  }
  Handoff(room, player, true,
          [self = shared_from_this(), room, player, packet = std::move(packet),
           done](auto result) mutable {
            if (std::holds_alternative<ServiceError>(result) || room->dead) {
              self->ForgetPlayer(room, player);
              self->EntryFailed(player, 403U, done, "进入房间失败，请稍后重试");
              return;
            }
            self->Enqueue(room, [self, room, player, packet = std::move(packet),
                                 done]() mutable {
              const auto member = room->members.find(player);
              if (member == room->members.end()) {
                done(Failure("room member expired"));
                self->Finish(room);
                return;
              }
              self->Packet(room, member->second.actor, std::move(packet), done);
            });
          });
}

void RoomGateway::Impl::Enqueue(const std::shared_ptr<Room>& room,
                                std::function<void()> work, Time received) {
  if (room->dead) {
    work();
    return;
  }
  room->queue.push_back({std::move(work), received == Time{}
                                              ? std::chrono::steady_clock::now()
                                              : received});
  Pump(room);
}

void RoomGateway::Impl::Pump(const std::shared_ptr<Room>& room) {
  if (room->busy || room->blocked || room->queue.empty() || room->dead) return;
  room->busy = true;
  auto work = std::move(room->queue.front());
  room->queue.pop_front();
  command_queue_latency.Observe(std::chrono::steady_clock::now() -
                                work.received);
  work.execute();
}

void RoomGateway::Impl::Finish(const std::shared_ptr<Room>& room) {
  room->busy = false;
  if (!room->dead) Pump(room);
}

void RoomGateway::Impl::Rpc(const std::shared_ptr<Room>& room, Json value,
                            JsonReply reply, RoomMessageKind kind,
                            std::optional<domain::PlayerId> player) {
  if (room->dead || !room->rpc) {
    reply(Failure("room connection closed"));
    return;
  }
  RoomEnvelope envelope;
  envelope.identity = room->identity;
  envelope.room_generation = room->generation;
  envelope.kind = kind;
  envelope.match_id =
      room->match ? room->match->Bytes() : domain::MatchId::ByteArray{};
  if (player) {
    const auto found = room->members.find(*player);
    if (found == room->members.end()) {
      reply(Failure("room session expired"));
      return;
    }
    envelope.player_id = *player;
    envelope.session_id = found->second.origin.id;
    envelope.session_generation = found->second.origin.generation;
    envelope.sequence = ++found->second.sequence;
  } else {
    envelope.sequence = ++room->control;
    if (value.contains("business_player"))
      envelope.player_id =
          domain::PlayerId{value.at("business_player").get<std::uint64_t>()};
  }
  const auto text = value.dump();
  envelope.payload.assign(text.begin(), text.end());
  if (!room->rpc->Request(std::move(envelope), [self = shared_from_this(), room,
                                                reply](auto result) {
        if (auto* message = std::get_if<RoomEnvelope>(&result))
          self->Receive(room, std::move(*message), reply);
        else
          reply(std::get<ServiceError>(std::move(result)));
      }))
    reply(Failure("room RPC queue full"));
}

void RoomGateway::Impl::Receive(const std::shared_ptr<Room>& room,
                                RoomEnvelope envelope, JsonReply reply) {
  if (room->dead) {
    if (reply) reply(Failure("room instance closed"));
    return;
  }
  try {
    if (envelope.identity != room->identity)
      throw std::invalid_argument{"room output identity mismatch"};
    room->generation = envelope.room_generation;
    const auto value = ParseRoomPayload(envelope.payload);
    if (!value.value("ok", false)) {
      if (reply) reply(Failure(value.value("detail", "room rejected request")));
      return;
    }
    if (value.contains("scene")) {
      const auto bytes = value.at("scene").get<std::vector<std::uint8_t>>();
      protocol::PacketReader reader{bytes};
      auto scene = protocol::DecodeSceneInfo(reader);
      reader.RequireEnd();
      if (scene.scene_id != room->identity.room_id.Value())
        throw std::invalid_argument{"room scene mismatch"};
      room->scene = std::move(scene);
    }
    room->closed = value.value("closed", false);
    if (reply)
      reply(value);
    else
      Apply(room, value);
  } catch (...) {
    FailRoom(room);
    if (reply) reply(Failure("invalid dedicated room output"));
  }
}

void RoomGateway::Impl::Deliver(const std::shared_ptr<Room>& room,
                                runtime::RoomCoordinatorResult result) {
  std::map<domain::PlayerId, runtime::SessionDispatch> batches;
  for (auto& row : result.deliveries) {
    if (!room->members.contains(row.recipient))
      throw std::invalid_argument{"cross-room recipient"};
    batches[row.recipient].messages.push_back(std::move(row.message));
  }
  for (const auto& update : result.session_updates) {
    if (!room->members.contains(update.player_id))
      throw std::invalid_argument{"cross-room stage update"};
    auto& member = room->members.at(update.player_id);
    using Stage = runtime::RoomSessionStage;
    using Phase = runtime::WorldSessionPhase;
    if (update.stage == Stage::kOutsideRoom) continue;
    const auto phase = update.stage == Stage::kWaiting   ? Phase::kRoomWaiting
                       : update.stage == Stage::kLoading ? Phase::kRoomLoading
                       : update.stage == Stage::kCountdown
                           ? Phase::kRoomCountdown
                       : update.stage == Stage::kPlaying ? Phase::kRoomPlaying
                                                         : Phase::kRoomResults;
    member.actor.state.phase = phase;
    batches[update.player_id].world_phase = phase;
  }
  for (auto& [player, dispatch] : batches) {
    const auto origin = room->members.at(player).origin;
    const auto delivered =
        sessions.DispatchIfCurrent(player, origin, std::move(dispatch));
    if (std::holds_alternative<ServiceError>(delivered))
      sessions.DisconnectIfCurrent(player, origin);
  }
  for (const auto& update : result.session_updates)
    if (update.stage == runtime::RoomSessionStage::kOutsideRoom)
      ReturnPlayer(room, update.player_id);
}

void RoomGateway::Impl::Apply(const std::shared_ptr<Room>& room, Json value) {
  if (room->dead) return;
  if (value.value("start_required", false)) {
    BeginStart(room, std::move(value));
    return;
  }
  if (!value.contains("deliveries")) return;
  auto result = ReadRoomResult(value);
  if (result.settlement_command) {
    if (!room->match || result.settlement_command->match_id != *room->match ||
        result.settlement_command->room_owner != Owner(room)) {
      FailRoom(room);
      return;
    }
    if (!room->terminal) {
      room->terminal = std::move(value);
      room->blocked = true;
      BeginTerminal(room);
    }
    return;
  }
  Deliver(room, std::move(result));
  if (room->closed) Retire(room, false);
  if (room->match && room->scene &&
      value.value("event", runtime::RoomEvent::kNoChange) ==
          runtime::RoomEvent::kReturnedToWaiting) {
    std::scoped_lock lock{mutex};
    matches.erase(room->match->ToUuidString());
    room->match.reset();
    room->terminal_staged = false;
    room->early_reward.reset();
    if (options.equipment_relay)
      options.equipment_relay->RevokeRoom(room->identity.room_id);
  }
}

void RoomGateway::Impl::Packet(const std::shared_ptr<Room>& room,
                               runtime::WorldActorContext actor,
                               protocol::Packet packet, Done done) {
  if (room->dead || !Current(actor)) {
    done(Failure("room session or instance expired"));
    Finish(room);
    return;
  }
  const auto player = actor.player_id;
  const auto opcode = packet.opcode;
  const auto value = EncodePacket(actor, packet);
  const auto sent = std::chrono::steady_clock::now();
  Rpc(
      room, value,
      [self = shared_from_this(), room, player, opcode, done,
       sent](auto result) {
        if (const auto* error = std::get_if<ServiceError>(&result)) {
          done(std::vector<runtime::OutboundMessage>{
              Notice("房间当前状态无法执行该操作")});
          if (opcode == 402U || opcode == 403U) {
            self->ReturnPlayer(room, player);
            if (opcode == 402U) self->Retire(room, false);
          }
          self->logger.Log(application::LogLevel::kDebug,
                           "room command rejected: " + error->safe_message);
        } else {
          const auto& value = std::get<Json>(result);
          self->Apply(room, value);
          if ((opcode == 402U || opcode == 403U) &&
              !value.value("start_required", false)) {
            const auto updates = ReadRoomResult(value).session_updates;
            if (std::ranges::none_of(updates, [player](const auto& update) {
                  return update.player_id == player &&
                         update.stage !=
                             runtime::RoomSessionStage::kOutsideRoom;
                }))
              self->ReturnPlayer(room, player);
          }
          done(std::vector<runtime::OutboundMessage>{});
          if (opcode != 32769U && opcode != 32885U && opcode != 65075U)
            self->immediate_rpc_latency.Observe(
                std::chrono::steady_clock::now() - sent);
        }
        self->Finish(room);
      },
      RoomMessageKind::kCommand, player);
}

std::optional<ServiceError> RoomGateway::Post(
    runtime::WorldActorContext actor, protocol::Packet packet,
    runtime::WorldCommandCompletion completion) {
  if (!completion || !impl_->Current(actor))
    return Impl::Failure("room command session expired");
  const auto estimate =
      packet.body.size() + 512U +
      (actor.player_snapshot ? actor.player_snapshot->items.size() * 256U : 0U);
  const auto admission = impl_->Admit(actor.player_id, estimate);
  if (!admission)
    return ServiceError{.code = application::ErrorCode::kOverloaded,
                        .safe_message = "room command budget exhausted",
                        .is_retryable = true};
  if (packet.opcode == 402U || packet.opcode == 403U) {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->pending_players.contains(actor.player_id) ||
        impl_->player_rooms.contains(actor.player_id))
      return Impl::Failure("player already entering or inside a room");
    impl_->pending_players.insert(actor.player_id);
  }
  auto done = [admission, completion](auto result) {
    completion(std::move(result));
  };
  const auto received = std::chrono::steady_clock::now();
  asio::post(impl_->io, [self = impl_, actor = std::move(actor),
                         packet = std::move(packet), done, received]() mutable {
    try {
      if (self->stopping) {
        done(Impl::Failure("room gateway stopping"));
        return;
      }
      if (packet.opcode == 402U) {
        self->Create(std::move(actor), std::move(packet), done);
        return;
      }
      if (packet.opcode == 403U) {
        self->Join(std::move(actor), std::move(packet), done);
        return;
      }
      if (packet.opcode == 401U) {
        const auto query = protocol::DecodeRoomListRequest(packet.body);
        std::vector<protocol::SceneInfo> scenes;
        for (const auto& [id, room] : self->rooms) {
          static_cast<void>(id);
          if (room->scene && !room->dead &&
              room->channel == actor.state.channel)
            scenes.push_back(*room->scene);
        }
        const auto begin = std::min<std::size_t>(
            scenes.size(), std::size_t{query.page} * query.page_size);
        const auto count =
            std::min<std::size_t>(scenes.size() - begin, query.page_size);
        done(std::vector<runtime::OutboundMessage>{
            {101U, protocol::EncodeRoomListResponse(
                       query.page, begin + count < scenes.size(),
                       std::span{scenes}.subspan(begin, count))}});
        return;
      }
      const auto room = self->Find(actor.player_id);
      if (!room) {
        done(std::vector<runtime::OutboundMessage>{
            Impl::Notice("该玩法入口尚未迁移到独立房间")});
        return;
      }
      if (packet.opcode == 105U || packet.opcode == 106U) packet = {404U, {}};
      self->Enqueue(
          room,
          [self, room, actor = std::move(actor), packet = std::move(packet),
           done]() mutable {
            self->Packet(room, std::move(actor), std::move(packet), done);
          },
          received);
    } catch (...) {
      {
        std::scoped_lock lock{self->mutex};
        self->pending_players.erase(actor.player_id);
      }
      done(ServiceError{.code = application::ErrorCode::kInvalidInput,
                        .safe_message = "invalid room command"});
    }
  });
  return std::nullopt;
}

void RoomGateway::Disconnect(domain::PlayerId player,
                             std::optional<runtime::SessionHandle> origin) {
  const auto room = impl_->Find(player);
  if (!room) return;
  asio::post(impl_->io, [self = impl_, room, player, origin] {
    const auto member = room->members.find(player);
    if (member == room->members.end() ||
        (origin && member->second.origin != *origin))
      return;
    if (room->dead) {
      self->ForgetPlayer(room, player);
      return;
    }
    auto actor = member->second.actor;
    self->Enqueue(room, [self, room, player, actor = std::move(actor)] {
      if (room->dead) {
        self->ForgetPlayer(room, player);
        return;
      }
      self->Rpc(
          room, Impl::EncodePacket(actor, {404U, {}}),
          [self, room, player](auto result) {
            if (const auto* value = std::get_if<Json>(&result))
              self->Apply(room, *value);
            else
              self->FailRoom(room);
            self->ForgetPlayer(room, player);
            self->Finish(room);
          },
          RoomMessageKind::kCommand, player);
    });
  });
}
}  // namespace bf::infrastructure::network
