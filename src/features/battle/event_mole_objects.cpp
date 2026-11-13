#include "features/battle/event_mole_objects.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace bf::runtime {
namespace {
bool ValidVector(const std::array<float, 3U>& vector) {
  return std::ranges::all_of(vector, [](float value) {
    return std::isfinite(value) && std::abs(value) <= 1.0e7F;
  });
}

bool SamePoint(const std::array<float, 3U>& a, const std::array<float, 3U>& b) {
  for (std::size_t i = 0U; i < a.size(); ++i)
    if (!std::isfinite(a[i]) || std::abs(a[i] - b[i]) > 0.01F) return false;
  return true;
}
}  // namespace

EventMoleObjects::EventMoleObjects(std::span<const EventMoleOwner> owners,
                                   domain::PlayerId authority)
    : authority_(authority) {
  std::set<std::uint32_t> seats;
  for (const auto& owner : owners) {
    const auto expected = 1'000'000U + owner.seat * 100'000U;
    if (!owner.player.IsValid() || owner.seat >= 16U ||
        owner.actor != expected || owner.confirmed_next_object <= expected ||
        owner.confirmed_next_object > expected + 99'999U ||
        !seats.insert(owner.seat).second ||
        !owners_
             .emplace(owner.player, Owner{owner, owner.confirmed_next_object})
             .second)
      throw std::invalid_argument{"invalid EventMole native owner"};
    used_objects_.insert(owner.actor);
  }
  if (owners_.empty() || !owners_.contains(authority))
    throw std::invalid_argument{"EventMole has no authenticated authority"};
}

std::optional<std::uint32_t> EventMoleObjects::AdvanceCreation(
    domain::PlayerId player, std::uint32_t expected_next, std::uint32_t count) {
  const auto found = owners_.find(player);
  if (found == owners_.end() || !found->second.active ||
      !found->second.ordered || count == 0U || count > 64U ||
      found->second.next != expected_next)
    return std::nullopt;
  auto& owner = found->second;
  const auto limit = owner.identity.actor + 99'999U;
  if (expected_next > limit || count > limit - expected_next)
    return std::nullopt;
  for (std::uint32_t i = 0U; i < count; ++i)
    if (used_objects_.contains(expected_next + i)) return std::nullopt;
  for (std::uint32_t i = 0U; i < count; ++i)
    used_objects_.insert(expected_next + i);
  owner.next += count;
  return expected_next;
}

void EventMoleObjects::MarkOrderGap(domain::PlayerId owner) noexcept {
  if (const auto found = owners_.find(owner); found != owners_.end())
    found->second.ordered = false;
}

std::optional<std::uint32_t> EventMoleObjects::NextObject(
    domain::PlayerId player) const noexcept {
  const auto found = owners_.find(player);
  return found == owners_.end() || !found->second.active ||
                 !found->second.ordered
             ? std::nullopt
             : std::optional{found->second.next};
}

bool EventMoleObjects::ChangeAuthority(domain::PlayerId player) noexcept {
  if (!NextObject(player) ||
      authority_generation_ == std::numeric_limits<std::uint64_t>::max())
    return false;
  if (player == authority_) return true;
  authority_ = player;
  ++authority_generation_;
  return true;
}

std::uint64_t EventMoleObjects::AuthorityGeneration() const noexcept {
  return authority_generation_;
}

bool EventMoleObjects::CheckpointMatches(
    domain::PlayerId player,
    const protocol::NativeObjectCheckpointRequest& request) const noexcept {
  const auto found = owners_.find(player);
  if (found == owners_.end() || !found->second.active ||
      !found->second.ordered ||
      request.reported.seat != found->second.identity.seat ||
      request.reported.next_object != found->second.next)
    return false;
  return std::ranges::any_of(owners_, [&request](const auto& recipient) {
    return recipient.second.active &&
           recipient.second.identity.seat == request.recipient_seat;
  });
}

std::optional<EventMoleObject> EventMoleObjects::CreateMole(
    const EventMoleConstruction& fact) {
  const auto owner = fact.owner;
  const auto& request = fact.spawn;
  if (owner != authority_ ||
      fact.authority_generation != authority_generation_ ||
      request.slot >= slots_.size() || slots_[request.slot])
    return std::nullopt;
  const auto& profile = GetEventMoleProfile();
  const auto spawner = std::ranges::find(
      profile.active_spawners, request.spawner, &EventMoleSpawner::id);
  if (spawner == profile.active_spawners.end() ||
      spawner->property != request.property ||
      spawner->team != kEventMoleNpcTeam)
    return std::nullopt;
  const auto point = std::ranges::find(
      profile.action_points, spawner->position_id, &EventMoleActionPoint::id);
  if (point == profile.action_points.end() ||
      !SamePoint(request.position, point->position))
    return std::nullopt;
  const auto next = NextObject(owner);
  if (!next || *next != fact.expected_counter ||
      !AdvanceCreation(owner, *next, 1U))
    return std::nullopt;
  const EventMoleObject object{*next,
                               request.slot,
                               request.spawner,
                               request.property,
                               owner,
                               request.position,
                               false};
  objects_.emplace(object.object, object);
  slots_[request.slot] = object.object;
  return object;
}

const EventMoleObject* EventMoleObjects::Object(
    std::uint32_t id) const noexcept {
  const auto found = objects_.find(id);
  return found == objects_.end() ? nullptr : &found->second;
}

bool EventMoleObjects::SetState(domain::PlayerId player,
                                const protocol::NativeMoleState& state) {
  const auto found = objects_.find(state.object);
  if (found == objects_.end() || found->second.owner != player ||
      !NextObject(player) || (state.state != 0U && state.state != 12U))
    return false;
  found->second.visible = state.state == 0U;
  return true;
}

bool EventMoleObjects::DestroyMole(domain::PlayerId player, std::uint32_t id) {
  const auto found = objects_.find(id);
  if (found == objects_.end() || found->second.owner != player ||
      !NextObject(player))
    return false;
  slots_[found->second.slot].reset();
  objects_.erase(found);
  std::erase_if(hits_,
                [id](const auto& hit) { return hit.second.target == id; });
  return true;
}

std::optional<std::uint64_t> EventMoleObjects::ConfirmHit(
    const EventMoleHit& hit) {
  const auto target = Object(hit.target);
  if (!NextObject(hit.attacker) || !target || !target->visible ||
      !NextObject(target->owner) || hit.source == 0U ||
      hit.attacker_life == 0U ||
      hit_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
      hit_keys_.size() >= 4096U)
    return std::nullopt;
  const std::array key{hit.attacker.Value(),
                       static_cast<std::uint64_t>(hit.target), hit.source,
                       hit.attacker_life};
  if (!hit_keys_.insert(key).second) return std::nullopt;
  hits_.emplace(++hit_sequence_, hit);
  return hit_sequence_;
}

bool EventMoleObjects::RegisterAuthorizedCoin(
    std::uint64_t hit, const protocol::NativeMoleCoinSpawn& coin,
    Clock::time_point now) {
  const auto found = hits_.find(hit);
  const auto amount = protocol::ObservedMoleCoinAmount(coin.kind);
  // S2C720 coin IDs are allocated by the server and are a separate namespace;
  // they never advance a player's GameObjectManager counter.
  if (found == hits_.end() || !amount || coin.object == 0U ||
      used_objects_.contains(coin.object) || !ValidVector(coin.position) ||
      !ValidVector(coin.velocity) || coins_.size() >= 4096U)
    return false;
  used_objects_.insert(coin.object);
  coins_.emplace(coin.object,
                 Coin{*amount, now + std::chrono::milliseconds{1000},
                      now + std::chrono::milliseconds{10000}});
  hits_.erase(found);
  return true;
}

std::optional<std::uint32_t> EventMoleObjects::Collect(
    domain::PlayerId player, const protocol::NativeMoleCoinPickup& request,
    Clock::time_point now) {
  const auto owner = owners_.find(player);
  const auto coin = coins_.find(request.coin);
  if (owner == owners_.end() || !owner->second.active ||
      owner->second.identity.actor != request.collector ||
      coin == coins_.end() || coin->second.consumed ||
      now < coin->second.activate || now >= coin->second.expire ||
      owner->second.collected >
          std::numeric_limits<std::uint64_t>::max() - coin->second.amount)
    return std::nullopt;
  coin->second.consumed = true;
  owner->second.collected += coin->second.amount;
  ++owner->second.pending_stats[coin->second.amount == 1U ? 0U : 1U];
  return coin->second.amount;
}

std::uint64_t EventMoleObjects::Collected(
    domain::PlayerId player) const noexcept {
  const auto found = owners_.find(player);
  return found == owners_.end() ? 0U : found->second.collected;
}

bool EventMoleObjects::ObserveCollectionStat(
    domain::PlayerId player, const protocol::NativeMoleCoinStat& report) {
  const auto found = owners_.find(player);
  if (found == owners_.end() || !found->second.active ||
      report.collector != found->second.identity.actor ||
      report.seat != found->second.identity.seat ||
      (report.event != 10U && report.event != 13U))
    return false;
  auto& pending = found->second.pending_stats[report.event == 10U ? 0U : 1U];
  if (pending == 0U) return false;
  --pending;
  return true;
}

void EventMoleObjects::Leave(domain::PlayerId player) noexcept {
  if (const auto found = owners_.find(player); found != owners_.end()) {
    found->second.active = false;
    found->second.ordered = false;
  }
  std::erase_if(objects_, [this, player](const auto& object) {
    if (object.second.owner != player) return false;
    slots_[object.second.slot].reset();
    return true;
  });
  std::erase_if(hits_, [this, player](const auto& hit) {
    return hit.second.attacker == player ||
           !objects_.contains(hit.second.target);
  });
}
}  // namespace bf::runtime
