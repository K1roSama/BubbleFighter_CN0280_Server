#include "sessions/hide_seek_session.h"

#include <stdexcept>

namespace bf::runtime {
namespace {
hide_seek_wire::State Wire(const HideSeekSnapshot& s,
                           HideSeekReject rejection) {
  hide_seek_wire::State v;
  v.round = s.round;
  v.revision = s.revision;
  v.player = s.player;
  v.phase = static_cast<std::uint8_t>(s.phase);
  v.initial = static_cast<std::uint8_t>(s.initial_role);
  v.role = static_cast<std::uint8_t>(s.role);
  v.winner = static_cast<std::uint8_t>(s.winner);
  v.rejection = static_cast<std::uint8_t>(rejection);
  v.cats = s.cats;
  v.hunters = s.hunters;
  v.converting = s.converting;
  v.remaining_ms = s.remaining_ms;
  v.conversion_ms = s.conversion_ms;
  v.transform_ms = s.transform_ms;
  v.cooldown_ms = s.cooldown_ms;
  v.hp = s.hp;
  v.magazine = s.magazine;
  v.reserve = s.reserve;
  v.offered = s.offered;
  v.disguise = s.disguise;
  for (const auto& r : s.results)
    v.scores.push_back({r.player, static_cast<std::uint8_t>(r.initial_role),
                        static_cast<std::uint8_t>(r.role), r.survived_ms,
                        r.captures, r.task_success, r.departed});
  return v;
}
}  // namespace

HideSeekSnapshot HideSeekSession::Snapshot(HideSeekPlayer player) const {
  if (!observers_.contains(player)) return match_.Snapshot(player);
  const auto appearances = match_.Appearances();
  auto s = match_.Snapshot(appearances.front().player);
  s.player = player;
  s.initial_role = HideSeekRole::kHunter;
  for (const auto& a : appearances)
    if (a.player == player)
      s.initial_role = match_.Snapshot(player).initial_role;
  s.role = HideSeekRole::kDeparted;
  s.hp = s.magazine = s.reserve = 0;
  s.offered = {};
  s.disguise = s.conversion_ms = s.transform_ms = s.cooldown_ms = 0;
  return s;
}

std::vector<HideSeekDelivery> HideSeekSession::Deliveries(
    std::optional<std::pair<HideSeekPlayer, HideSeekReject>> rejection) {
  const auto appearances = match_.Appearances();
  hide_seek_wire::Appearances visual;
  hide_seek_wire::Hints audio;
  for (const auto& a : appearances) {
    visual.actors.push_back({a.player, a.actor, a.prop,
                             static_cast<std::uint8_t>(a.role), a.quarter_turn,
                             a.position});
    if (!visual.round) {
      const auto s = match_.Snapshot(a.player);
      visual.round = s.round;
      visual.revision = s.revision;
      audio.round = s.round;
    }
  }
  for (const auto& h : match_.TakeHints())
    audio.events.push_back({h.serial, h.actor, h.position});
  const auto visual_body = hide_seek_wire::encode(visual);
  const auto audio_body = audio.events.empty() ? std::vector<std::uint8_t>{}
                                               : hide_seek_wire::encode(audio);
  std::vector<HideSeekDelivery> out;
  std::set<HideSeekPlayer> recipients = observers_;
  for (const auto& a : appearances)
    if (a.role != HideSeekRole::kDeparted) recipients.insert(a.player);
  for (const auto player : recipients) {
    const auto why = rejection && rejection->first == player
                         ? rejection->second
                         : HideSeekReject::kNone;
    out.push_back({player, hide_seek_wire::state_opcode,
                   hide_seek_wire::encode(Wire(Snapshot(player), why))});
    out.push_back({player, hide_seek_wire::appearance_opcode, visual_body});
    if (!audio_body.empty())
      out.push_back({player, hide_seek_wire::hint_opcode, audio_body});
  }
  return out;
}

std::vector<HideSeekDelivery> HideSeekSession::Reject(HideSeekPlayer player,
                                                      HideSeekReject reason) {
  return {{player, hide_seek_wire::state_opcode,
           hide_seek_wire::encode(Wire(Snapshot(player), reason))}};
}

std::vector<HideSeekDelivery> HideSeekSession::Request(
    HideSeekPlayer player, std::span<const std::uint8_t> bytes,
    HideSeekTime now) {
  const auto r = hide_seek_wire::decode_request(bytes.data(), bytes.size());
  // Verify membership and epoch before advancing or touching any
  // replay/rate-limit ledger.
  const auto current = Snapshot(player);
  if (r.command == hide_seek_wire::Command::Hello || r.round != current.round ||
      (current.role == HideSeekRole::kDeparted && !observers_.contains(player)))
    throw std::invalid_argument(
        "hide_seek_v1 request is outside its negotiated round");
  const auto previous = last_serial_.find(player);
  if (previous != last_serial_.end() && r.serial <= previous->second)
    return Reject(player, HideSeekReject::kReplay);
  if (last_request_.contains(player) && now - last_request_.at(player) < 10)
    return Reject(player, HideSeekReject::kCooldown);
  last_request_[player] = now;
  last_serial_[player] = r.serial;
  if (observers_.contains(player) && r.command != hide_seek_wire::Command::Poll)
    return Reject(player, HideSeekReject::kWrongRole);
  HideSeekReject why = HideSeekReject::kNone;
  switch (r.command) {
    case hide_seek_wire::Command::Poll:
      if (last_poll_.contains(player) && now - last_poll_.at(player) < 250)
        return {};
      last_poll_[player] = now;
      match_.Tick(now);
      break;
    case hide_seek_wire::Command::Select:
      why = match_.SelectDisguise(player, r.value, now);
      break;
    case hide_seek_wire::Command::Rotate:
      why = match_.Rotate(player, now);
      break;
    case hide_seek_wire::Command::Reload:
      why = match_.Reload(player, now);
      break;
    case hide_seek_wire::Command::Attack:
      why = match_
                .Attack(player, r.serial, static_cast<HideSeekWeapon>(r.weapon),
                        r.point, now)
                .rejection;
      break;
    case hide_seek_wire::Command::AttackRay:
      why = match_
                .Attack(player, r.serial, static_cast<HideSeekWeapon>(r.weapon),
                        r.point, now, r.origin)
                .rejection;
      break;
    case hide_seek_wire::Command::Move:
      why = match_.Move(player, r.point, now);
      break;
    default:
      throw std::invalid_argument("hide_seek_v1 invalid round command");
  }
  // A poll, unchanged pose or rejected action must not rebroadcast the same
  // whole-room state to every peer. Besides wasted bandwidth that can bury
  // phase/capture events behind hundreds of redundant snapshots.
  if (Snapshot(player).revision == current.revision) return Reject(player, why);
  return Deliveries(std::pair{player, why});
}

std::vector<HideSeekDelivery> HideSeekSession::Tick(HideSeekTime now) {
  match_.Tick(now);
  return Deliveries();
}

std::vector<HideSeekDelivery> HideSeekSession::Leave(HideSeekPlayer player,
                                                     HideSeekTime now) {
  if (observers_.erase(player)) {
    return {};
  }
  match_.Leave(player, now);
  return Deliveries();
}

std::vector<HideSeekDelivery> HideSeekSession::Observe(HideSeekPlayer player,
                                                       HideSeekTime now) {
  if (!player || match_.Phase() == HideSeekPhase::kResults ||
      observers_.size() >= 8)
    throw std::invalid_argument("hide_seek_v1 invalid spectator subscription");
  for (const auto& a : match_.Appearances())
    if (a.player == player && a.role != HideSeekRole::kDeparted)
      throw std::invalid_argument(
          "hide_seek_v1 active player cannot become a late spectator");
  observers_.insert(player);
  last_serial_.erase(player);
  last_poll_.erase(player);
  last_request_.erase(player);
  match_.Tick(now);
  return Deliveries();  // Also deliver any phase/hint advanced by this join.
}
}  // namespace bf::runtime
