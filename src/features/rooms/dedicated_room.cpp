#include "features/rooms/dedicated_room.h"

#include <stdexcept>

namespace bf::runtime {
namespace {
application::ServiceError Rejection(std::string message) {
  return {.code = application::ErrorCode::kConflict,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = false};
}
}  // namespace

DedicatedRoom::DedicatedRoom(protocol::internal::RoomIdentity identity,
                             RoomMatchTiming timing, std::uint16_t capacity)
    : identity_(identity),
      coordinator_(std::move(timing), identity.room_id, capacity) {
  if (!identity.IsValid() || capacity < 2U || capacity > 16U ||
      capacity % 2U != 0U)
    throw std::invalid_argument{"invalid dedicated room identity or capacity"};
}

bool DedicatedRoom::Accepts(
    const protocol::internal::RoomEnvelope& envelope) const noexcept {
  return envelope.identity == identity_ &&
         envelope.room_generation == Generation();
}

application::ServiceResult<RoomCoordinatorResult> DedicatedRoom::Command(
    const protocol::internal::RoomEnvelope& envelope, RoomActorContext actor,
    const protocol::Packet& packet, std::chrono::steady_clock::time_point now) {
  if (!Accepts(envelope) || actor.player_id != envelope.player_id ||
      !envelope.player_id.IsValid() || !envelope.session_id ||
      !envelope.session_generation || !envelope.sequence || Closed()) {
    return Rejection("room instance, generation or session does not match");
  }
  const auto current = coordinator_.FindRoom(actor.player_id);
  const bool joining = packet.opcode == 402U || packet.opcode == 403U ||
                       packet.opcode == 141U ||
                       packet.opcode == kTrainingEntryOpcode;
  if (!current && !joining)
    return Rejection("player is not a member of this room");
  if (const auto previous = origins_.find(actor.player_id);
      previous != origins_.end()) {
    const auto& origin = previous->second;
    const auto prior = std::pair{origin.session_id, origin.session_generation};
    const auto incoming =
        std::pair{envelope.session_id, envelope.session_generation};
    if ((current && incoming != prior) || incoming < prior ||
        (incoming == prior && envelope.sequence <= origin.sequence)) {
      return Rejection("stale session or duplicate room command");
    }
  }
  if (!origins_.contains(actor.player_id) && origins_.size() >= 4096U)
    return Rejection("room session history capacity exceeded");
  // Consume the sequence before handling: a malformed command must never
  // execute on retry.
  origins_.insert_or_assign(
      actor.player_id,
      RoomCommandOrigin{envelope.session_id, envelope.session_generation,
                        envelope.sequence});
  auto result = coordinator_.Handle(std::move(actor), packet, now);
  created_ = created_ || coordinator_.RoomCount() != 0U;
  return result;
}

std::vector<RoomMemberSnapshot> DedicatedRoom::Members() const {
  const auto room = coordinator_.FindRoom(identity_.room_id);
  return room ? room->Members() : std::vector<RoomMemberSnapshot>{};
}

std::vector<RoomTimerToken> DedicatedRoom::Timers() const {
  const auto room = coordinator_.FindRoom(identity_.room_id);
  return room ? room->ActiveTimers() : std::vector<RoomTimerToken>{};
}

application::ServiceResult<RoomCoordinatorResult> DedicatedRoom::ApplyTimer(
    const RoomTimerToken& token, std::chrono::steady_clock::time_point now) {
  if (token.room_id != identity_.room_id)
    return Rejection("timer belongs to a different room");
  return coordinator_.ApplyTimer(token, now);
}

std::uint64_t DedicatedRoom::Generation() const noexcept {
  const auto room = coordinator_.FindRoom(identity_.room_id);
  return room ? room->Generation() : 0U;
}

bool DedicatedRoom::Closed() const noexcept {
  return created_ && coordinator_.RoomCount() == 0U;
}

RoomCoordinator& DedicatedRoom::Coordinator() noexcept { return coordinator_; }

const protocol::internal::RoomIdentity& DedicatedRoom::Identity()
    const noexcept {
  return identity_;
}
}  // namespace bf::runtime
