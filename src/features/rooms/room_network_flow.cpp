// The original movement payload is validated by CombatRelay. The extension
// only adds a shared sequence for UDP/TCP de-duplication and candidate
// exchange.
#include <algorithm>
#include <charconv>
#include <limits>
#include <random>

#include "features/rooms/room_command_flow.h"
#include "protocol/rooms/room_network_messages.h"

namespace bf::runtime {
namespace {
std::uint32_t Ipv4(std::string_view text) {
  std::uint32_t result = 0;
  for (unsigned n = 0; n < 4; ++n) {
    const auto end = text.find('.');
    const auto part = text.substr(0, end);
    unsigned value = 0;
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), value);
    if (part.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != part.data() + part.size() || value > 255U)
      return 0;
    result |= value << (n * 8U);
    if (n == 3U) {
      if (end != std::string_view::npos) return 0;
    } else {
      if (end == std::string_view::npos) return 0;
      text.remove_prefix(end + 1U);
    }
  }
  return result;
}

bool PrivateIpv4(std::uint32_t ip) {
  const auto a = ip & 255U, b = (ip >> 8U) & 255U;
  return a == 10U || (a == 172U && b >= 16U && b <= 31U) ||
         (a == 192U && b == 168U) || a == 127U;
}

std::array<std::uint8_t, 32> RandomKey() {
  // MSVC random_device uses the Windows OS random provider. Do not seed a PRNG
  // from time, player IDs or the room generation.
  std::random_device source;
  std::array<std::uint8_t, 32> key{};
  for (auto& byte : key) byte = static_cast<std::uint8_t>(source());
  return key;
}

void IceText(protocol::PacketWriter& writer, const std::string& text) {
  writer.WriteU32(static_cast<std::uint32_t>(text.size()));
  writer.WriteBytes(
      {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
}  // namespace

void RoomCommandFlow::AppendNetworkPeers(RoomCommandResult& result) {
  const bool selected =
      room_->NetworkMode() == protocol::RoomNetworkMode::kP2pPreferred;
  if (selected &&
      (network_generation_ != room_->Generation() || network_epoch_ == 0)) {
    network_keys_.clear();
    network_generation_ = room_->Generation();
    const auto bytes = RandomKey();
    network_epoch_ = 0;
    for (unsigned i = 0; i < 8; ++i)
      network_epoch_ |= std::uint64_t{bytes[i]} << (i * 8U);
    if (!network_epoch_) network_epoch_ = 1;
  }
  for (const auto player : network_setting_clients_) {
    const auto self = room_->GetMember(player);
    if (!self) continue;
    const auto endpoint = network_endpoints_.find(player);
    std::vector<domain::PlayerId> peers;
    if (selected && endpoint != network_endpoints_.end())
      for (const auto& member : room_->Members())
        if (member.player_id != player &&
            network_endpoints_.contains(member.player_id))
          peers.push_back(member.player_id);
    protocol::PacketWriter out;
    out.WriteU32(1);
    out.WriteU32(static_cast<std::uint32_t>(room_->RoomId().Value()));
    out.WriteU64(room_->Generation());
    out.WriteU64(selected ? network_epoch_ : 0);
    out.WriteU32(selected ? 1U : 0U);
    out.WriteU32(room_->Phase() == RoomPhase::kPlaying ? 1U : 0U);
    out.WriteU64(player.Value());
    out.WriteU32(self->object_id.value_or(0U));
    out.WriteU32(
        endpoint == network_endpoints_.end() ? 0U : endpoint->second.public_ip);
    out.WriteU32(static_cast<std::uint32_t>(peers.size()));
    out.WriteU32(static_cast<std::uint32_t>(room_->Members().size() - 1U));
    for (const auto peer : peers) {
      const auto member = room_->GetMember(peer);
      const auto& target = network_endpoints_.at(peer);
      const auto pair = std::minmax(player, peer);
      const auto [key, inserted] =
          network_keys_.try_emplace({pair.first, pair.second});
      if (inserted) key->second = RandomKey();
      out.WriteU64(peer.Value());
      out.WriteU32(member->object_id.value_or(0U));
      out.WriteU32(target.public_ip);
      out.WriteU32(target.private_ip);
      out.WriteU32(target.port);
      out.WriteBytes(key->second);
    }
    result.deliveries.push_back(
        {player, {protocol::kRoomNetworkPeers, out.Release()}});
    if (endpoint != network_endpoints_.end() && endpoint->second.ice) {
      const auto lease =
          endpoint->second.lease.value_or(application::IceLease{});
      protocol::PacketWriter ice;
      ice.WriteU32(1);
      ice.WriteU32(static_cast<std::uint32_t>(room_->RoomId().Value()));
      ice.WriteU64(room_->Generation());
      ice.WriteU64(selected ? network_epoch_ : 0);
      ice.WriteU64(lease.expires_at);
      ice.WriteU32(lease.relay_only ? 1U : 0U);
      IceText(ice, lease.stun_host);
      ice.WriteU32(lease.stun_port);
      IceText(ice, lease.turn_host);
      ice.WriteU32(lease.turn_port);
      IceText(ice, lease.username);
      IceText(ice, lease.password);
      std::vector<domain::PlayerId> capable;
      for (const auto peer : peers)
        if (network_endpoints_.at(peer).ice) capable.push_back(peer);
      ice.WriteU32(static_cast<std::uint32_t>(capable.size()));
      for (const auto peer : capable) ice.WriteU64(peer.Value());
      result.deliveries.push_back({player, {65079U, ice.Release()}});
    }
  }
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleNetworkRegister(
    domain::PlayerId actor, std::span<const std::uint8_t> body,
    std::string_view peer_address,
    const std::optional<application::IceLease>& ice_lease) {
  protocol::PacketReader reader{body};
  const auto version = reader.ReadU32();
  if (version != 1U && version != 2U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "P2P registration version"};
  const auto room = reader.ReadU32();
  const auto generation = reader.ReadU64();
  const auto port = reader.ReadU32(), local = reader.ReadU32();
  const auto capability = version == 2U ? reader.ReadU32() : 0U;
  reader.RequireEnd();
  if (capability > 1U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "ICE capability"};
  if (port > 65535U || port < 1024U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "P2P port range"};
  auto result = MakeResult(RoomEvent::kNoChange);
  if (!room_->GetMember(actor))
    return application::ServiceError{application::ErrorCode::kPermissionDenied,
                                     "玩家不在当前房间"};
  network_setting_clients_.insert(actor);
  const auto public_ip = Ipv4(peer_address);
  if (room == room_->RoomId().Value() && generation == room_->Generation() &&
      public_ip &&
      room_->NetworkMode() == protocol::RoomNetworkMode::kP2pPreferred) {
    auto& endpoint = network_endpoints_[actor];
    endpoint.public_ip = public_ip;
    endpoint.port = port;
    endpoint.private_ip = PrivateIpv4(local) && ((local & 255U) != 127U ||
                                                 (public_ip & 255U) == 127U)
                              ? local
                              : 0U;
    if (endpoint.lease && ice_lease && endpoint.lease != ice_lease) {
      network_keys_.clear();
      network_epoch_ = 0;
      network_generation_ = 0;
    }
    endpoint.ice = capability == 1U;
    endpoint.lease = endpoint.ice ? ice_lease : std::nullopt;
  }
  AppendNetworkState(result);
  AppendNetworkPeers(result);
  return result;
}

application::ServiceResult<RoomCommandResult> RoomCommandFlow::HandleIceSignal(
    domain::PlayerId actor, std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{body};
  if (reader.ReadU32() != 1U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "ICE signalling version"};
  const auto room = reader.ReadU32();
  const auto epoch = reader.ReadU64();
  const domain::PlayerId peer{reader.ReadU64()};
  const auto kind = reader.ReadU32(), length = reader.ReadU32();
  if (kind > 2U || length > 3072U || (kind == 2U && length))
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "ICE signalling limit"};
  const auto text = reader.ReadBytes(length);
  reader.RequireEnd();
  for (const auto byte : text)
    if ((byte < 32U && byte != 9U && byte != 10U && byte != 13U) || byte > 126U)
      throw protocol::ProtocolError{
          protocol::ProtocolErrorCode::kInvalidValue,
          "ICE signalling requires bounded ASCII SDP"};
  auto result = MakeResult(RoomEvent::kNoChange);
  const auto self = network_endpoints_.find(actor),
             target = network_endpoints_.find(peer);
  if (room != room_->RoomId().Value() || epoch != network_epoch_ ||
      network_generation_ != room_->Generation() ||
      room_->NetworkMode() != protocol::RoomNetworkMode::kP2pPreferred ||
      !room_->GetMember(actor) || !room_->GetMember(peer) || actor == peer ||
      self == network_endpoints_.end() || target == network_endpoints_.end() ||
      !self->second.ice || !target->second.ice)
    return result;
  if (now - self->second.signal_window >= std::chrono::seconds{1}) {
    self->second.signal_window = now;
    self->second.signals = 0;
  }
  if (++self->second.signals > 128U) return result;
  if (kind == 2U) {
    if (ice_restarted_at_ &&
        now - *ice_restarted_at_ < std::chrono::seconds{30})
      return result;
    ice_restarted_at_ = now;
    network_keys_.clear();
    network_epoch_ = 0;
    network_generation_ = 0;
    AppendNetworkState(result);
    AppendNetworkPeers(result);
    return result;
  }
  protocol::PacketWriter out;
  out.WriteU32(1);
  out.WriteU32(room);
  out.WriteU64(epoch);
  out.WriteU64(actor.Value());
  out.WriteU32(kind);
  out.WriteU32(length);
  out.WriteBytes(text);
  result.deliveries.push_back({peer, {65078U, out.Release()}});
  return result;
}

application::ServiceResult<RoomCommandResult>
RoomCommandFlow::HandleNetworkMovement(
    domain::PlayerId actor, std::span<const std::uint8_t> body,
    std::chrono::steady_clock::time_point now) {
  protocol::PacketReader reader{body};
  if (reader.ReadU32() != 1U)
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "P2P movement version"};
  const auto room = reader.ReadU32();
  const auto epoch = reader.ReadU64(), serial = reader.ReadU64();
  const auto opcode = reader.ReadU16(), size = reader.ReadU16();
  if (!((opcode == 32769U && size == 47U) || (opcode == 32885U && size == 21U)))
    throw protocol::ProtocolError{protocol::ProtocolErrorCode::kInvalidValue,
                                  "P2P movement whitelist"};
  const auto payload = reader.ReadBytes(size);
  reader.RequireEnd();
  const auto endpoint = network_endpoints_.find(actor);
  if (room != room_->RoomId().Value() ||
      network_generation_ != room_->Generation() || epoch != network_epoch_ ||
      room_->NetworkMode() != protocol::RoomNetworkMode::kP2pPreferred ||
      endpoint == network_endpoints_.end() || !serial ||
      serial <= endpoint->second.serial)
    return MakeResult(RoomEvent::kNoChange);
  auto result =
      HandleImpl(actor, {opcode, {payload.begin(), payload.end()}}, now);
  auto* accepted = std::get_if<RoomCommandResult>(&result);
  if (!accepted || accepted->combat_reject_reason) return result;
  endpoint->second.serial = serial;
  pending_network_movements_.insert_or_assign(
      actor,
      PendingNetworkMovement{.serial = serial,
                             .opcode = opcode,
                             .payload = {payload.begin(), payload.end()}});
  for (auto& delivery : accepted->deliveries) {
    if (delivery.message.opcode != opcode ||
        !network_setting_clients_.contains(delivery.recipient) ||
        !network_endpoints_.contains(delivery.recipient))
      continue;
    protocol::PacketWriter out;
    out.WriteU32(1);
    out.WriteU32(room);
    out.WriteU64(epoch);
    out.WriteU64(actor.Value());
    out.WriteU64(serial);
    out.WriteU16(opcode);
    out.WriteU16(size);
    out.WriteBytes(payload);
    delivery.message = {protocol::kRoomNetworkMovementRelay, out.Release()};
  }
  return result;
}

void RoomCommandFlow::AppendMovementTick(RoomCommandResult& result) {
  if (!combat_) {
    pending_network_movements_.clear();
    return;
  }
  const auto movements = combat_->FlushMovementTick();
  result.deliveries.reserve(result.deliveries.size() + movements.size());
  for (const auto& movement : movements) {
    auto message = movement.message;
    const auto pending = pending_network_movements_.find(movement.sender);
    const auto target = network_endpoints_.find(movement.recipient);
    const bool p2p =
        room_->NetworkMode() == protocol::RoomNetworkMode::kP2pPreferred;
    if (p2p && pending != pending_network_movements_.end() &&
        network_setting_clients_.contains(movement.recipient) &&
        target != network_endpoints_.end() &&
        pending->second.payload.size() <=
            std::numeric_limits<std::uint16_t>::max()) {
      protocol::PacketWriter out;
      out.WriteU32(1U);
      out.WriteU32(static_cast<std::uint32_t>(room_->RoomId().Value()));
      out.WriteU64(network_epoch_);
      out.WriteU64(movement.sender.Value());
      out.WriteU64(pending->second.serial);
      out.WriteU16(pending->second.opcode);
      out.WriteU16(static_cast<std::uint16_t>(pending->second.payload.size()));
      out.WriteBytes(pending->second.payload);
      message = {.opcode = protocol::kRoomNetworkMovementRelay,
                 .body = out.Release()};
    }
    result.deliveries.push_back(
        {.recipient = movement.recipient, .message = std::move(message)});
  }
  pending_network_movements_.clear();
}
}  // namespace bf::runtime
