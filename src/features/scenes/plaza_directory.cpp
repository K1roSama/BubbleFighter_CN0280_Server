#include "features/scenes/plaza_directory.h"

#include <iterator>
#include <limits>

namespace bf::runtime {
namespace {
application::ServiceError Outside() {
  return {.code = application::ErrorCode::kInvalidState,
          .safe_message = "玩家不在自由广场中"};
}
}  // namespace

PlazaDirectory::PlazaDirectory(std::uint16_t capacity) : capacity_(capacity) {
  // Reuse the instance's capacity validation without allocating an empty scene.
  static_cast<void>(PlazaCoordinator{capacity});
}

PlazaCoordinator* PlazaDirectory::Find(domain::PlayerId player) {
  const auto found = membership_.find(player);
  return found == membership_.end()
             ? nullptr
             : instances_.at(found->second).coordinator.get();
}

const PlazaCoordinator* PlazaDirectory::Find(domain::PlayerId player) const {
  const auto found = membership_.find(player);
  return found == membership_.end()
             ? nullptr
             : instances_.at(found->second).coordinator.get();
}

application::ServiceResult<PlazaCoordinatorResult> PlazaDirectory::Enter(
    application::PlayerMenuSnapshot snapshot, std::uint32_t channel,
    protocol::ScenePartyState party) {
  const auto player = snapshot.player_id;
  const auto previous = membership_.find(player);
  if (previous != membership_.end() &&
      instances_.at(previous->second).channel == channel) {
    return instances_.at(previous->second)
        .coordinator->Enter(std::move(snapshot), party);
  }
  auto selected = instances_.end();
  for (auto it = instances_.begin(); it != instances_.end(); ++it) {
    if (it->second.channel == channel &&
        it->second.coordinator->MemberCount() < capacity_) {
      selected = it;
      break;
    }
  }
  std::uint32_t scene_id = 1U;
  std::unique_ptr<PlazaCoordinator> created;
  if (selected == instances_.end()) {
    while (instances_.contains(scene_id)) {
      if (scene_id == std::numeric_limits<std::uint16_t>::max()) {
        return application::ServiceError{
            .code = application::ErrorCode::kOverloaded,
            .safe_message = "可用广场编号已耗尽"};
      }
      ++scene_id;
    }
    created = std::make_unique<PlazaCoordinator>(capacity_, scene_id);
  } else {
    scene_id = selected->first;
  }
  auto* target = created ? created.get() : selected->second.coordinator.get();
  auto entered = target->Enter(std::move(snapshot), party);
  if (const auto* error = std::get_if<application::ServiceError>(&entered)) {
    return *error;
  }
  PlazaCoordinatorResult result;
  if (previous != membership_.end()) {
    auto left = Leave(player);
    if (const auto* error = std::get_if<application::ServiceError>(&left)) {
      static_cast<void>(target->Leave(player));
      return *error;
    }
    result.deliveries =
        std::move(std::get<PlazaCoordinatorResult>(left).deliveries);
  }
  if (created) {
    instances_.emplace(scene_id, Instance{.channel = channel,
                                          .coordinator = std::move(created)});
  }
  membership_.emplace(player, scene_id);
  auto& joined = std::get<PlazaCoordinatorResult>(entered);
  result.event = joined.event;
  result.session_updates = std::move(joined.session_updates);
  result.deliveries.insert(result.deliveries.end(),
                           std::make_move_iterator(joined.deliveries.begin()),
                           std::make_move_iterator(joined.deliveries.end()));
  return result;
}

application::ServiceResult<PlazaCoordinatorResult> PlazaDirectory::Leave(
    domain::PlayerId player) {
  const auto member = membership_.find(player);
  if (member == membership_.end()) {
    return PlazaCoordinator{capacity_}.Leave(player);
  }
  const auto instance = instances_.find(member->second);
  auto result = instance->second.coordinator->Leave(player);
  if (std::holds_alternative<application::ServiceError>(result)) {
    return result;
  }
  membership_.erase(member);
  if (instance->second.coordinator->MemberCount() == 0U) {
    instances_.erase(instance);
  }
  return result;
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaDirectory::HandleMovement(domain::PlayerId player,
                               std::span<const std::uint8_t> body) {
  auto* instance = Find(player);
  return instance
             ? instance->HandleMovement(player, body)
             : application::ServiceResult<PlazaCoordinatorResult>{Outside()};
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaDirectory::RelayMinigame(domain::PlayerId player,
                              const protocol::LobbyMinigameSync& sync) const {
  const auto* instance = Find(player);
  return instance
             ? instance->RelayMinigame(player, sync)
             : application::ServiceResult<PlazaCoordinatorResult>{Outside()};
}

application::ServiceResult<PlazaCoordinatorResult> PlazaDirectory::HandleChat(
    domain::PlayerId player, const domain::UserMessage& message,
    std::span<const domain::PlayerId> blocked_peers) const {
  const auto* instance = Find(player);
  return instance
             ? instance->HandleChat(player, message, blocked_peers)
             : application::ServiceResult<PlazaCoordinatorResult>{Outside()};
}

application::ServiceResult<bool> PlazaDirectory::UpdatePlayerSnapshot(
    application::PlayerMenuSnapshot snapshot) {
  auto* instance = Find(snapshot.player_id);
  return instance ? instance->UpdatePlayerSnapshot(std::move(snapshot))
                  : application::ServiceResult<bool>{Outside()};
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaDirectory::EquipmentChanged(application::PlayerMenuSnapshot snapshot) {
  auto* instance = Find(snapshot.player_id);
  return instance ? instance->EquipmentChanged(std::move(snapshot))
                  : PlazaCoordinatorResult{};
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaDirectory::CoupleChanged(const application::PlayerMenuSnapshot& snapshot) {
  auto* instance = Find(snapshot.player_id);
  return instance ? instance->CoupleChanged(snapshot)
                  : PlazaCoordinatorResult{};
}

PlazaCoordinatorResult PlazaDirectory::ClanMarkChanged(domain::PlayerId player,
                                                       std::uint32_t mark) {
  auto* instance = Find(player);
  return instance ? instance->ClanMarkChanged(player, mark)
                  : PlazaCoordinatorResult{};
}

bool PlazaDirectory::Contains(domain::PlayerId player) const noexcept {
  return membership_.contains(player);
}

PlazaCoordinatorResult PlazaDirectory::UpdateParty(
    domain::PlayerId player, protocol::ScenePartyState party) {
  auto* instance = Find(player);
  return instance ? instance->UpdateParty(player, party)
                  : PlazaCoordinatorResult{};
}

PlazaCoordinatorResult PlazaDirectory::RecruitmentText(
    domain::PlayerId player, std::u16string text,
    std::span<const domain::PlayerId> blocked,
    std::optional<std::uint8_t> recipient_slot) {
  auto* instance = Find(player);
  return instance ? instance->RecruitmentText(player, std::move(text), blocked,
                                              recipient_slot)
                  : PlazaCoordinatorResult{};
}

std::size_t PlazaDirectory::MemberCount() const noexcept {
  return membership_.size();
}

std::size_t PlazaDirectory::SceneCount() const noexcept {
  return instances_.size();
}

std::optional<protocol::SceneInfo> PlazaDirectory::Scene(
    domain::PlayerId player) const {
  const auto* instance = Find(player);
  return instance ? std::optional{instance->Scene()} : std::nullopt;
}

application::ServiceResult<PlazaCoordinatorResult>
PlazaDirectory::LevelIconChanged(application::PlayerMenuSnapshot snapshot) {
  auto* instance = Find(snapshot.player_id);
  return instance ? instance->LevelIconChanged(std::move(snapshot))
                  : PlazaCoordinatorResult{};
}

}  // namespace bf::runtime
