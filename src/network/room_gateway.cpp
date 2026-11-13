#include <algorithm>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>

#include "network/room_gateway_state.h"
#include "protocol/codec/packet_io.h"
#include "protocol/codec/text_codec.h"
#include "protocol/rooms/room_messages.h"

namespace bf::infrastructure::network {
namespace asio = boost::asio;
using Json = nlohmann::json;
using application::ServiceError;
using application::ServiceResult;
using protocol::internal::RoomMessageKind;

RoomGatewayOptions LoadRoomGatewayOptions(const std::filesystem::path& path) {
  const auto absolute = std::filesystem::absolute(path);
  if (std::filesystem::file_size(absolute) > 65536U)
    throw std::invalid_argument{"room gateway config size"};
  std::ifstream input{absolute, std::ios::binary};
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input},
                                        {}};
  const auto value = ParseRoomPayload(bytes);
  const auto resolve = [&](const char* field) {
    return std::filesystem::absolute(absolute.parent_path() /
                                     value.at(field).get<std::string>())
        .string();
  };
  RoomGatewayOptions options;
  options.tls = {
      resolve("certificate"), resolve("private_key"), resolve("authority"), {}};
  options.maximum_rooms = value.value("maximum_rooms", options.maximum_rooms);
  options.maximum_commands =
      value.value("maximum_commands", options.maximum_commands);
  options.maximum_queued_bytes =
      value.value("maximum_queued_bytes", options.maximum_queued_bytes);
  const auto capacity =
      value.value("room_capacity", std::uint64_t{options.room_capacity});
  if (capacity < 2U || capacity > 12U || capacity % 2U)
    throw std::invalid_argument{"invalid room capacity"};
  options.room_capacity = static_cast<std::uint16_t>(capacity);
  if (value.contains("drain_marker")) {
    const auto marker = value.at("drain_marker").get<std::string>();
    if (marker.empty()) throw std::invalid_argument{"empty room drain marker"};
    options.drain_marker =
        std::filesystem::absolute(absolute.parent_path() / marker);
  }
  for (const auto& node : value.at("nodes")) {
    const auto port = node.at("port").get<std::uint64_t>();
    if (!port || port > 65535U)
      throw std::invalid_argument{"invalid room node port"};
    options.nodes.push_back({node.at("name"), node.at("host"),
                             static_cast<std::uint16_t>(port),
                             node.at("peer_name")});
  }
  return options;
}

RoomGateway::Impl::Impl(RoomGatewayOptions options,
                        runtime::SessionRegistry& sessions,
                        application::BackgroundTaskExecutor& background,
                        application::RoomLeaseStore& leases,
                        application::MatchPreparationExecutor& preparation,
                        application::MatchIdSource& ids,
                        application::MatchStartExecutor& starts,
                        const application::SystemClock& clock,
                        runtime::DurableMatchSettlementFlow& settlements,
                        application::PlayerMenuSnapshotLoader& menus,
                        application::EventLogger& logger)
    : options(std::move(options)),
      sessions(sessions),
      background(background),
      leases(leases),
      preparation_executor(preparation),
      ids(ids),
      starts(starts),
      clock(clock),
      settlements(settlements),
      menus(menus),
      logger(logger) {
  if (!options.region_id || options.region_id > 32767U ||
      options.nodes.empty() || options.nodes.size() > 64U ||
      !options.maximum_rooms || options.maximum_rooms > 32767U ||
      options.maximum_commands < 128U || options.maximum_commands > 65536U ||
      options.maximum_queued_bytes < 4U * 1024U * 1024U ||
      options.maximum_queued_bytes > 512U * 1024U * 1024U ||
      options.room_capacity < 2U || options.room_capacity > 12U ||
      options.room_capacity % 2U)
    throw std::invalid_argument{"invalid room gateway capacity"};
  std::set<std::pair<std::string, std::uint16_t>> endpoints;
  std::set<std::string> names;
  for (const auto& endpoint : options.nodes) {
    if (endpoint.host.empty() || endpoint.host.size() > 253U ||
        !endpoint.port || endpoint.name.empty() ||
        endpoint.name.size() > 128U || endpoint.peer_name.empty() ||
        !endpoints.emplace(endpoint.host, endpoint.port).second ||
        !names.insert(endpoint.name).second)
      throw std::invalid_argument{"invalid or duplicate room node endpoint"};
    auto node = std::make_shared<Node>();
    node->endpoint = endpoint;
    nodes.push_back(std::move(node));
  }
}

void RoomGateway::Impl::Start(runtime::WorldExecutor& lobby_world) {
  {
    std::scoped_lock lock{mutex};
    if (started) throw std::logic_error{"room gateway starts once"};
    lobby = &lobby_world;
    started = accepting = true;
  }
  asio::post(io, [self = shared_from_this()] { self->Tick(); });
  worker = std::thread{[self = shared_from_this()] {
    try {
      self->io.run();
    } catch (...) {
      self->logger.Log(application::LogLevel::kCritical,
                       "room gateway event loop failed");
      std::terminate();
    }
  }};
}

void RoomGateway::Impl::Stop() {
  {
    std::scoped_lock lock{mutex};
    if (!started) {
      work_guard.reset();
      return;
    }
    accepting = false;
  }
  if (!worker.joinable()) return;
  asio::post(io, [self = shared_from_this()] {
    self->stopping = true;
    self->timer.cancel();
    const auto rooms = self->rooms;
    for (const auto& [id, room] : rooms) {
      static_cast<void>(id);
      self->FailRoom(room);
    }
    for (auto& node : self->nodes)
      if (node->rpc) node->rpc->Close();
    if (!self->external) self->work_guard.reset();
  });
  worker.join();
}

void RoomGateway::Impl::FinishExternal() {
  --external;
  if (stopping && !external) work_guard.reset();
}

ServiceError RoomGateway::Impl::Failure(std::string reason) {
  return {.code = application::ErrorCode::kStorageUnavailable,
          .safe_message = std::move(reason),
          .is_retryable = true};
}

runtime::OutboundMessage RoomGateway::Impl::Notice(std::string text) {
  protocol::PacketWriter out;
  out.WriteUtf16(protocol::Utf8ToUtf16(text));
  return {906U, out.Release()};
}

application::RoomOwnership RoomGateway::Impl::Owner(
    const std::shared_ptr<Room>& room) const {
  return {*domain::MatchId::FromBytes(room->identity.instance_id),
          room->identity.owner_epoch};
}

bool RoomGateway::Impl::Current(const runtime::WorldActorContext& actor) const {
  return actor.orig_session &&
         sessions.IsCurrentPlayerSession(
             actor.player_id,
             {actor.orig_session->first, actor.orig_session->second});
}

std::shared_ptr<RoomGateway::Impl::Room> RoomGateway::Impl::Find(
    domain::PlayerId player) const {
  std::scoped_lock lock{mutex};
  const auto found = player_rooms.find(player);
  return found == player_rooms.end() ? nullptr : found->second;
}

RoomGateway::Impl::Admission::~Admission() {
  if (const auto self = owner.lock()) {
    std::scoped_lock lock{self->mutex};
    --self->metrics.queued_commands;
    self->metrics.queued_bytes -= bytes;
    const auto found = self->budgets.find(key);
    if (--found->second.first == 0U)
      self->budgets.erase(found);
    else
      found->second.second -= bytes;
  }
}

std::shared_ptr<RoomGateway::Impl::Admission> RoomGateway::Impl::Admit(
    domain::PlayerId player, std::size_t bytes) {
  std::scoped_lock lock{mutex};
  const auto found = player_rooms.find(player);
  const auto key =
      found != player_rooms.end()
          ? std::pair{true, found->second->identity.room_id.Value()}
          : std::pair{false, player.Value()};
  const auto budget = budgets.find(key);
  if (!accepting || bytes > 4U * 1024U * 1024U ||
      metrics.queued_commands >= options.maximum_commands ||
      bytes > options.maximum_queued_bytes - metrics.queued_bytes ||
      (budget != budgets.end() &&
       (budget->second.first >= 256U ||
        bytes > 4U * 1024U * 1024U - budget->second.second))) {
    ++metrics.rejected_commands;
    return {};
  }
  auto token =
      std::shared_ptr<Admission>{new Admission{weak_from_this(), key, bytes}};
  auto& value = budgets[key];
  ++value.first;
  value.second += bytes;
  ++metrics.queued_commands;
  metrics.queued_bytes += bytes;
  return token;
}

void RoomGateway::Impl::NodeLost(const std::shared_ptr<Node>& node) {
  node->connected = node->polling = false;
  node->valid_until = {};
  if (stopping) return;
  const auto room_snapshot = rooms;
  for (const auto& [id, room] : room_snapshot) {
    static_cast<void>(id);
    if (room->node == node) FailRoom(room);
  }
}

void RoomGateway::Impl::PollNode(const std::shared_ptr<Node>& node) {
  if (stopping || node->polling) return;
  if (!node->connected) {
    node->polling = true;
    auto tls = options.tls;
    tls.peer_name = node->endpoint.peer_name;
    node->rpc = std::make_shared<RoomRpcClient>(io, tls);
    node->rpc->Connect(
        node->endpoint.host, node->endpoint.port,
        [self = shared_from_this(), node](bool ready) {
          node->polling = false;
          node->connected = ready;
          if (ready) self->PollNode(node);
        },
        {}, [self = shared_from_this(), node] { self->NodeLost(node); });
    return;
  }
  node->polling = true;
  protocol::internal::RoomEnvelope request;
  const std::string text = R"({"op":"status"})";
  request.payload.assign(text.begin(), text.end());
  const bool accepted = node->rpc->Request(
      std::move(request), [self = shared_from_this(), node](auto result) {
        if (self->stopping) {
          node->polling = false;
          return;
        }
        const auto* envelope =
            std::get_if<protocol::internal::RoomEnvelope>(&result);
        if (!envelope) {
          self->NodeLost(node);
          return;
        }
        try {
          const auto value = ParseRoomPayload(envelope->payload);
          if (!value.value("ok", false))
            throw std::invalid_argument{"node status failed"};
          const auto id = domain::MatchId::FromUuidString(
              value.at("node_id").get<std::string>());
          if (!id)
            throw std::invalid_argument{"invalid node lifetime identity"};
          if (node->id != id) {
            if (node->id) {
              const auto rooms = self->rooms;
              for (const auto& [room_id, room] : rooms) {
                static_cast<void>(room_id);
                if (room->node == node) self->FailRoom(room);
              }
            }
            node->id = id;
            node->reservations.clear();
            node->registered = node->retired = false;
          }
          node->draining = value.at("draining").get<bool>();
          node->cpu = value.at("cpu_per_mille").get<std::uint64_t>();
          node->memory = value.at("private_bytes").get<std::uint64_t>();
          node->budget = value.at("memory_budget_bytes").get<std::uint64_t>();
          node->handles = value.at("handles").get<std::uint64_t>();
          node->room_count = value.at("rooms").size();
          node->available_rooms =
              value.at("available_rooms").get<std::size_t>();
          if (node->available_rooms > 4096U || node->room_count > 4096U)
            throw std::invalid_argument{"invalid node room capacity"};
          // Keep local reservations until an Agent snapshot includes their
          // allocations.
          for (const auto& child : value.at("rooms"))
            if (!child.at("instance_id").is_null())
              node->reservations.erase(
                  child.at("instance_id").get<domain::MatchId::ByteArray>());
          if (node->retired) {
            node->polling = false;
            return;
          }
          std::vector<protocol::internal::RoomIdentity> identities;
          const auto rooms = self->rooms;
          for (const auto& [room_id, room] : rooms) {
            if (room->node != node || room->dead) continue;
            if (room->ready &&
                std::ranges::none_of(value.at("rooms"), [room_id,
                                                         identity =
                                                             room->identity](
                                                            const auto& child) {
                  return child.at("room_id").template get<std::uint64_t>() ==
                             room_id.Value() &&
                         child.at("instance_id")
                                 .template get<domain::MatchId::ByteArray>() ==
                             identity.instance_id &&
                         child.at("owner_epoch")
                                 .template get<std::uint64_t>() ==
                             identity.owner_epoch;
                })) {
              self->FailRoom(room);
              continue;
            }
            identities.push_back(room->identity);
          }
          const auto sent = std::chrono::steady_clock::now();
          const auto registered = node->registered;
          const auto region = self->options.region_id;
          const auto draining = node->draining;
          using Renewal = std::pair<
              bool,
              std::vector<std::pair<protocol::internal::RoomIdentity, bool>>>;
          self->Database(
              [self, id = *id, registered, region, draining,
               name = node->endpoint.name,
               identities]() -> ServiceResult<Renewal> {
                auto lease =
                    registered ? self->leases.RenewNode(id, draining)
                               : self->leases.RegisterNode({id, region, name});
                if (const auto* error = std::get_if<ServiceError>(&lease))
                  return *error;
                if (!std::get<bool>(lease)) return Renewal{false, {}};
                Renewal renewal{true, {}};
                for (const auto& identity : identities) {
                  auto result = self->leases.RenewRoom(
                      {*domain::MatchId::FromBytes(identity.instance_id),
                       identity.owner_epoch});
                  if (const auto* error = std::get_if<ServiceError>(&result))
                    return *error;
                  renewal.second.emplace_back(identity, std::get<bool>(result));
                }
                return renewal;
              },
              [self, node, id, sent](ServiceResult<Renewal> renewal) {
                node->polling = false;
                if (node->id != id || self->stopping) return;
                const auto* refreshed = std::get_if<Renewal>(&renewal);
                if (!refreshed) return;
                if (!refreshed->first) {
                  node->retired = true;
                  self->NodeLost(node);
                  self->logger.Log(
                      application::LogLevel::kError,
                      "DS node lease cannot be revived; restart agent");
                  return;
                }
                node->registered = true;
                node->valid_until = sent + std::chrono::seconds{12};
                for (const auto& [identity, live] : refreshed->second) {
                  const auto found = self->rooms.find(identity.room_id);
                  if (found == self->rooms.end() ||
                      found->second->identity != identity)
                    continue;
                  const auto room = found->second;
                  if (!live) {
                    self->FailRoom(room);
                    continue;
                  }
                  room->lease_until = sent + std::chrono::seconds{8};
                  const auto remaining =
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          room->lease_until - std::chrono::steady_clock::now())
                          .count();
                  if (remaining <= 0) {
                    self->FailRoom(room);
                    continue;
                  }
                  if (room->ready)
                    self->Rpc(
                        room, {{"op", "lease"}, {"lease_ms", remaining}},
                        [self, room](auto result) {
                          if (std::holds_alternative<ServiceError>(result))
                            self->FailRoom(room);
                          else
                            room->timer_p99_us = std::get<Json>(result).value(
                                "timer_p99_us", std::uint64_t{0U});
                        },
                        RoomMessageKind::kHeartbeat);
                }
              });
        } catch (...) {
          self->NodeLost(node);
        }
      });
  // Local control backpressure is not evidence that the Agent or its rooms
  // died.
  if (!accepted) node->polling = false;
}

void RoomGateway::Impl::Tick() {
  if (stopping) return;
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_nodes) {
    next_nodes = now + std::chrono::seconds{2};
    for (const auto& node : nodes) PollNode(node);
    if (!options.drain_marker.empty() && !checking_drain) {
      checking_drain = true;
      Database(
          [marker = options.drain_marker]() -> ServiceResult<bool> {
            return std::filesystem::exists(marker);
          },
          [self = shared_from_this()](auto result) {
            self->checking_drain = false;
            if (const auto* requested = std::get_if<bool>(&result);
                requested && *requested) {
              std::scoped_lock lock{self->mutex};
              self->metrics.draining = true;
            }
          });
    }
  }
  const auto room_snapshot = rooms;
  for (const auto& [id, room] : room_snapshot) {
    static_cast<void>(id);
    if (now >= room->lease_until) {
      FailRoom(room);
      continue;
    }
    if (room->starting && !room->start_in_flight && !room->waiting_equipment &&
        now >= room->next_retry)
      PersistStart(room);
    if (room->terminal && !room->terminal_in_flight && now >= room->next_retry)
      BeginTerminal(room);
    if (room->waiting_equipment) {
      const auto state =
          options.equipment_relay->Readiness(room->identity.room_id);
      if (state == runtime::EquipmentRelayReadiness::kFailed ||
          now >= room->equipment_deadline)
        FailRoom(room);
      else if (state == runtime::EquipmentRelayReadiness::kReady)
        Install(room);
    }
  }
  if (!recovering && now >= next_recovery) {
    recovering = true;
    next_recovery = now + std::chrono::seconds{5};
    Database(
        [self = shared_from_this()] {
          return self->leases.RecoverExpired(64U);
        },
        [self = shared_from_this()](auto) { self->recovering = false; });
  }
  {
    std::scoped_lock lock{mutex};
    metrics.rooms = rooms.size();
    metrics.command_queue = command_queue_latency.Snapshot();
    metrics.immediate_rpc = immediate_rpc_latency.Snapshot();
    metrics.worst_room_timer_p99_us = 0U;
    for (const auto& [id, room] : rooms) {
      static_cast<void>(id);
      metrics.worst_room_timer_p99_us =
          std::max(metrics.worst_room_timer_p99_us, room->timer_p99_us);
    }
    metrics.healthy_nodes =
        std::ranges::count_if(nodes, [now](const auto& node) {
          return node->connected && !node->retired && !node->draining &&
                 now < node->valid_until;
        });
  }
  timer.expires_after(std::chrono::milliseconds{50});
  timer.async_wait([self = shared_from_this()](auto error) {
    if (!error) self->Tick();
  });
}

RoomGateway::RoomGateway(RoomGatewayOptions options,
                         runtime::SessionRegistry& sessions,
                         application::BackgroundTaskExecutor& background,
                         application::RoomLeaseStore& leases,
                         application::MatchPreparationExecutor& preparation,
                         application::MatchIdSource& ids,
                         application::MatchStartExecutor& starts,
                         const application::SystemClock& clock,
                         runtime::DurableMatchSettlementFlow& settlements,
                         application::PlayerMenuSnapshotLoader& menus,
                         application::EventLogger& logger)
    : impl_(std::make_shared<Impl>(std::move(options), sessions, background,
                                   leases, preparation, ids, starts, clock,
                                   settlements, menus, logger)) {}

RoomGateway::~RoomGateway() { Stop(); }

void RoomGateway::Start(runtime::WorldExecutor& lobby) { impl_->Start(lobby); }

void RoomGateway::Stop() { impl_->Stop(); }

void RoomGateway::Drain() {
  std::scoped_lock lock{impl_->mutex};
  impl_->metrics.draining = true;
}

RoomGatewayMetrics RoomGateway::Metrics() const {
  std::scoped_lock lock{impl_->mutex};
  return impl_->metrics;
}

bool RoomGateway::Contains(domain::PlayerId player) const {
  std::scoped_lock lock{impl_->mutex};
  return impl_->player_rooms.contains(player) ||
         impl_->pending_players.contains(player);
}

bool RoomGateway::Routes(const runtime::WorldActorContext& actor,
                         const protocol::Packet& packet) const {
  if (Contains(actor.player_id)) return true;
  if (actor.state.phase >= runtime::WorldSessionPhase::kRoomWaiting &&
      actor.state.phase <= runtime::WorldSessionPhase::kRoomResults)
    return true;
  switch (packet.opcode) {
    case 401U:
    case 402U:
    case 403U:
    case 114U:
    case 141U:
    case 65033U:
    case 65087U:
    case 65088U:
      return true;
    default:
      return false;
  }
}
}  // namespace bf::infrastructure::network
