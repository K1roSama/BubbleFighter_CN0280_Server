#include "features/battle/hide_seek_match.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace bf::runtime {
namespace {
bool Finite(const HideSeekPoint& p) {
  return std::all_of(p.begin(), p.end(), [](float v) {
    return std::isfinite(v) && std::abs(v) <= 100'000.F;
  });
}

float Length(const HideSeekPoint& p) {
  return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}

HideSeekPoint Unit(HideSeekPoint p) {
  const auto n = Length(p);
  for (auto& v : p) v /= n;
  return p;
}

HideSeekPoint Cross(const HideSeekPoint& a, const HideSeekPoint& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

bool Overlaps(const HideSeekBox& a, const HideSeekBox& b) {
  for (std::size_t i = 0; i < 3; ++i)
    if (a.maximum[i] <= b.minimum[i] + 0.001F ||
        b.maximum[i] <= a.minimum[i] + 0.001F)
      return false;
  return true;
}

std::optional<float> Ray(const HideSeekBox& b, const HideSeekPoint& o,
                         const HideSeekPoint& d, float maximum) {
  float near = 0.F, far = maximum;
  for (std::size_t i = 0; i < 3; ++i) {
    if (std::abs(d[i]) < 0.000001F) {
      if (o[i] < b.minimum[i] || o[i] > b.maximum[i]) return std::nullopt;
    } else {
      float a = (b.minimum[i] - o[i]) / d[i], z = (b.maximum[i] - o[i]) / d[i];
      if (a > z) std::swap(a, z);
      near = std::max(near, a);
      far = std::min(far, z);
      if (near > far) return std::nullopt;
    }
  }
  return near;
}

std::uint32_t Remaining(HideSeekTime due, HideSeekTime now) {
  return static_cast<std::uint32_t>(std::clamp<HideSeekTime>(
      due - now, 0, std::numeric_limits<std::uint32_t>::max()));
}
}  // namespace

bool HideSeekRules::Valid() const noexcept {
  return hiding_ms > 0 && hiding_ms <= 120'000 && hunting_ms > 0 &&
         hunting_ms <= 600'000 && frenzy_ms > 0 && frenzy_ms < hunting_ms &&
         conversion_ms > 0 && conversion_ms <= 30'000 && transform_ms > 0 &&
         transform_ms <= 5'000 && disguise_cooldown_ms >= transform_ms &&
         disguise_cooldown_ms <= 120'000 && hint_ms >= 1'000 &&
         hint_ms <= hunting_ms && shotgun_interval_ms >= 100 &&
         hammer_interval_ms >= 100 && reload_ms >= 100 &&
         shotgun_interval_ms <= 10'000 && hammer_interval_ms <= 10'000 &&
         reload_ms <= 30'000 && hunter_hp > 0 && cat_hp > 0 &&
         shotgun_cost > 0 && hammer_cost > 0 && pellet_damage > 0 &&
         hammer_damage > 0 && magazine > 0 && magazine <= 100 &&
         reserve <= 1000 && std::isfinite(hunter_speed) && hunter_speed > 0.F &&
         hunter_speed <= 100.F && std::isfinite(cat_speed_ratio) &&
         cat_speed_ratio >= 1.F && cat_speed_ratio <= 2.F &&
         std::isfinite(shotgun_range) && shotgun_range > 0.F &&
         shotgun_range <= 1000.F && std::isfinite(hammer_range) &&
         hammer_range > 0.F && hammer_range <= 5.F &&
         std::isfinite(shotgun_spread_radians) &&
         shotgun_spread_radians > 0.F && shotgun_spread_radians <= 0.3F;
}

std::size_t HideSeekRules::HunterCount(std::size_t n) noexcept {
  return n < 4 || n > 8 ? 0U : n <= 5 ? 1U : 2U;
}

bool HideSeekBox::Valid() const noexcept {
  if (!Finite(minimum) || !Finite(maximum)) return false;
  for (std::size_t i = 0; i < 3; ++i)
    if (minimum[i] >= maximum[i]) return false;
  return true;
}

bool HideSeekScene::Valid() const noexcept {
  if (map_key != "rel_Village01_Byway" || props.size() < 3 ||
      props.size() > 128 || solid.size() > 100'000 ||
      triangles.size() > 500'000 || red_spawns.size() < 6 ||
      blue_spawns.empty() || blue_spawns.size() > 16)
    return false;
  std::set<std::uint32_t> ids;
  for (const auto& p : props) {
    if (!p.id || !ids.insert(p.id).second || p.name.empty() ||
        p.name.size() > 128 || p.model.empty() || p.model.size() > 256 ||
        p.model.find("..") != std::string::npos || p.model.front() == '/' ||
        p.model.front() == '\\' || p.model.find(':') != std::string::npos ||
        !p.bounds.Valid())
      return false;
    if (std::abs(p.bounds.minimum[1]) > .005F ||
        std::abs(p.bounds.minimum[0] + p.bounds.maximum[0]) > .01F ||
        std::abs(p.bounds.minimum[2] + p.bounds.maximum[2]) > .01F)
      return false;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto extent = p.bounds.maximum[i] - p.bounds.minimum[i];
      if (extent < 0.2F || extent > 3.F) return false;
    }
    if (p.geometry && (p.geometry->id != p.id || p.geometry->name != p.name ||
                       p.geometry->minimum != p.bounds.minimum ||
                       p.geometry->maximum != p.bounds.maximum ||
                       p.geometry->surfaces.empty()))
      return false;
  }
  for (const auto& b : solid)
    if (!b.Valid()) return false;
  for (const auto& t : triangles)
    if (!Finite(t.a) || !Finite(t.b) || !Finite(t.c)) return false;
  for (const auto& p : red_spawns)
    if (!Finite(p)) return false;
  for (const auto& p : blue_spawns)
    if (!Finite(p)) return false;
  return true;
}

HideSeekMatch::HideSeekMatch(HideSeekRules rules, HideSeekScene scene,
                             std::span<const HideSeekMember> members,
                             std::uint64_t round, std::uint32_t seed,
                             HideSeekTime now,
                             std::span<const HideSeekPlayer> selected_hunters)
    : rules_(rules),
      scene_(std::move(scene)),
      start_(now),
      now_(now),
      round_(round),
      seed_(seed) {
  if (!rules_.Valid() || !scene_.Valid() ||
      !HideSeekRules::HunterCount(members.size()) || !round || now < 0 ||
      now > std::numeric_limits<HideSeekTime>::max() - 1'000'000)
    throw std::invalid_argument(
        "invalid hide_seek_v1 rules, scene, roster or clock");
  std::set<std::uint32_t> actors;
  std::set<std::uint8_t> seats;
  for (const auto& m : members) {
    if (!m.player || m.seat >= 8 ||
        m.actor != 1'000'000U + std::uint32_t(m.seat) * 100'000U ||
        !actors.insert(m.actor).second || !seats.insert(m.seat).second ||
        states_.contains(m.player))
      throw std::invalid_argument("invalid hide_seek_v1 identity");
    State state;
    state.member = m;
    state.hp = rules_.cat_hp;
    state.magazine = rules_.magazine;
    state.reserve = rules_.reserve;
    state.last_move = now;
    states_.emplace(m.player, state);
  }
  std::mt19937 random(seed);
  std::vector<HideSeekPlayer> shuffled;
  for (const auto& [id, s] : states_) {
    static_cast<void>(s);
    shuffled.push_back(id);
  }
  std::shuffle(shuffled.begin(), shuffled.end(), random);
  if (!selected_hunters.empty()) {
    const std::set<HideSeekPlayer> selected(selected_hunters.begin(),
                                            selected_hunters.end());
    if (selected.size() != selected_hunters.size() ||
        selected.size() != HideSeekRules::HunterCount(members.size()))
      throw std::invalid_argument("invalid frozen hide_seek_v1 hunters");
    for (const auto id : selected)
      if (!states_.contains(id))
        throw std::invalid_argument("unregistered frozen hide_seek_v1 hunter");
    std::stable_partition(shuffled.begin(), shuffled.end(),
                          [&](auto id) { return selected.contains(id); });
  }
  std::vector<std::uint32_t> candidates;
  for (const auto& p : scene_.props) candidates.push_back(p.id);
  for (std::size_t i = 0; i < shuffled.size(); ++i) {
    auto& s = states_.at(shuffled[i]);
    if (i < HideSeekRules::HunterCount(members.size())) {
      s.role = s.initial = HideSeekRole::kHunter;
      s.hp = rules_.hunter_hp;
      s.position = scene_.blue_spawns[i % scene_.blue_spawns.size()];
    } else {
      std::shuffle(candidates.begin(), candidates.end(), random);
      std::copy_n(candidates.begin(), 3, s.offered.begin());
      s.prop = s.offered[0];
      s.position =
          scene_.red_spawns[(i - HideSeekRules::HunterCount(members.size())) %
                            scene_.red_spawns.size()];
    }
    if (!Space(s, s.prop, s.position, 0))
      throw std::invalid_argument("hide_seek_v1 spawn intersects world");
  }
  hunt_ = start_ + rules_.hiding_ms;
  end_ = hunt_ + rules_.hunting_ms;
  frenzy_ = end_ - rules_.frenzy_ms;
  next_hint_ = hunt_ + rules_.hint_ms;
}

const HideSeekProp* HideSeekMatch::Prop(std::uint32_t id) const noexcept {
  const auto it = std::find_if(scene_.props.begin(), scene_.props.end(),
                               [id](const auto& p) { return p.id == id; });
  return it == scene_.props.end() ? nullptr : &*it;
}

HideSeekBox HideSeekMatch::Box(const State&, std::uint32_t id,
                               const HideSeekPoint& p,
                               std::uint8_t turn) const {
  const auto* model = Prop(id);
  HideSeekBox local =
      model ? model->bounds : HideSeekBox{{-.3F, 0.F, -.3F}, {.3F, 1.8F, .3F}};
  HideSeekBox out{{100'000.F, 100'000.F, 100'000.F},
                  {-100'000.F, -100'000.F, -100'000.F}};
  for (unsigned corner = 0; corner < 8; ++corner) {
    HideSeekPoint v;
    for (unsigned i = 0; i < 3; ++i)
      v[i] = (corner & (1U << i)) ? local.maximum[i] : local.minimum[i];
    for (unsigned t = 0; t < turn; ++t) {
      const float x = v[0];
      v[0] = -v[2];
      v[2] = x;
    }
    for (unsigned i = 0; i < 3; ++i) {
      out.minimum[i] = std::min(out.minimum[i], v[i] + p[i]);
      out.maximum[i] = std::max(out.maximum[i], v[i] + p[i]);
    }
  }
  return out;
}

bool HideSeekMatch::Space(const State& s, std::uint32_t id,
                          const HideSeekPoint& p, std::uint8_t turn) const {
  const auto bounds = Box(s, id, p, turn);
  return std::none_of(scene_.solid.begin(), scene_.solid.end(),
                      [&](const auto& b) { return Overlaps(bounds, b); }) &&
         std::none_of(scene_.triangles.begin(), scene_.triangles.end(),
                      [&](const auto& t) {
                        return HsSweep(t, bounds.minimum, bounds.maximum);
                      });
}

void HideSeekMatch::Finish(HideSeekWinner winner) {
  if (phase_ == HideSeekPhase::kResults) return;
  winner_ = winner;
  phase_ = HideSeekPhase::kResults;
  ++revision_;
  hints_.clear();
  for (auto& [id, s] : states_) {
    static_cast<void>(id);
    s.transform_due = s.reload_due = 0;
  }
}

void HideSeekMatch::Decide() {
  if (phase_ == HideSeekPhase::kResults) return;
  std::size_t cats = 0, hunters = 0, present = 0;
  for (const auto& [id, s] : states_) {
    static_cast<void>(id);
    if (s.role != HideSeekRole::kDeparted) ++present;
    if (s.role == HideSeekRole::kCat) ++cats;
    if (s.role == HideSeekRole::kHunter || s.role == HideSeekRole::kConverting)
      ++hunters;
  }
  if (!present)
    Finish(HideSeekWinner::kAbandoned);
  else if (!cats)
    Finish(HideSeekWinner::kHunters);
  else if (!hunters || now_ >= end_)
    Finish(HideSeekWinner::kCats);
}

void HideSeekMatch::Advance(HideSeekTime now, bool check) {
  if (now < now_)
    throw std::invalid_argument("hide_seek_v1 clock moved backwards");
  if (phase_ == HideSeekPhase::kResults) return;
  now_ = std::min(now, end_);
  const auto phase = now_ >= frenzy_ ? HideSeekPhase::kFrenzy
                     : now_ >= hunt_ ? HideSeekPhase::kHunting
                                     : HideSeekPhase::kHiding;
  if (phase != phase_) {
    phase_ = phase;
    ++revision_;
    if (phase_ == HideSeekPhase::kFrenzy)
      for (auto& [id, s] : states_) {
        static_cast<void>(id);
        s.reload_due = 0;
        if (s.role == HideSeekRole::kHunter) s.magazine = rules_.magazine;
      }
  }
  for (auto& [id, s] : states_) {
    static_cast<void>(id);
    if (s.role == HideSeekRole::kConverting && now_ >= s.conversion_due) {
      s.role = HideSeekRole::kHunter;
      s.hp = rules_.hunter_hp;
      s.magazine = rules_.magazine;
      s.reserve = rules_.reserve;
      s.position =
          scene_.blue_spawns[s.member.seat % scene_.blue_spawns.size()];
      s.last_move = now_;
      s.next_attack = now_;
      s.prop = 0;
      s.turn = 0;
      s.conversion_due = 0;
      ++revision_;
    }
    if (s.role == HideSeekRole::kCat && s.transform_due &&
        now_ >= s.transform_due) {
      if (Space(s, s.pending_prop, s.position, s.turn)) {
        s.prop = s.pending_prop;
        s.cooldown_due = now_ + rules_.disguise_cooldown_ms;
      }
      s.transform_due = 0;
      s.pending_prop = 0;
      ++revision_;
    }
    if (s.reload_due && now_ >= s.reload_due) {
      const auto amount =
          std::min<std::uint16_t>(rules_.magazine - s.magazine, s.reserve);
      s.magazine = static_cast<std::uint16_t>(s.magazine + amount);
      s.reserve = static_cast<std::uint16_t>(s.reserve - amount);
      s.reload_due = 0;
      ++revision_;
    }
  }
  if (check) Decide();
  if (phase_ != HideSeekPhase::kResults && now_ >= next_hint_) {
    hints_.clear();  // No burst or unbounded backlog after a delayed executor
                     // tick.
    for (const auto& [id, s] : states_) {
      static_cast<void>(id);
      if (s.role == HideSeekRole::kCat)
        hints_.push_back({++hint_serial_, s.member.actor, s.position});
    }
    next_hint_ = hunt_ + ((now_ - hunt_) / rules_.hint_ms + 1) * rules_.hint_ms;
    ++revision_;
  }
}

void HideSeekMatch::Tick(HideSeekTime now) { Advance(now, true); }

HideSeekReject HideSeekMatch::Move(HideSeekPlayer id, const HideSeekPoint& p,
                                   HideSeekTime now) {
  Advance(now, true);
  auto it = states_.find(id);
  if (it == states_.end()) return HideSeekReject::kUnknownPlayer;
  if (phase_ == HideSeekPhase::kResults) return HideSeekReject::kWrongPhase;
  auto& s = it->second;
  if (s.role != HideSeekRole::kCat && s.role != HideSeekRole::kHunter)
    return HideSeekReject::kWrongRole;
  if (phase_ == HideSeekPhase::kHiding && s.role == HideSeekRole::kHunter)
    return HideSeekReject::kWrongPhase;
  if (!Finite(p)) return HideSeekReject::kInvalidValue;
  HideSeekPoint delta;
  for (unsigned i = 0; i < 3; ++i) delta[i] = p[i] - s.position[i];
  const float speed =
      rules_.hunter_speed *
      (s.role == HideSeekRole::kCat ? rules_.cat_speed_ratio : 1.F);
  // Bounded tolerance is independent of packet rate; invalid/same-position
  // packets never refill it.
  const auto elapsed = std::min<HideSeekTime>(now_ - s.last_move, 1'000);
  const float budget =
      speed * (static_cast<float>(elapsed) / 1'000.F) + s.movement_credit;
  const float horizontal = std::sqrt(delta[0] * delta[0] + delta[2] * delta[2]);
  const float vertical_budget =
      rules_.hunter_speed * 3.F * (static_cast<float>(elapsed) / 1'000.F) +
      s.vertical_credit;
  // Jump/fall velocity is independent of ground movement. A diagonal jump must
  // not consume the horizontal speed allowance merely because its Y component
  // is nonzero.
  if (horizontal > budget || std::abs(delta[1]) > vertical_budget)
    return HideSeekReject::kMovement;
  if (!Space(s, s.prop, p, s.turn)) return HideSeekReject::kNoSpace;
  // Sweep world boxes so a legal endpoint cannot tunnel across a thin wall.
  const float distance = Length(delta);
  if (distance > 0.0001F) {
    const auto direction = Unit(delta);
    const auto start_bounds = Box(s, s.prop, s.position, s.turn);
    for (const auto& triangle : scene_.triangles)
      if (HsSweep(triangle, start_bounds.minimum, start_bounds.maximum, delta))
        return HideSeekReject::kNoSpace;
    const auto body = Box(s, s.prop, {0.F, 0.F, 0.F}, s.turn);
    for (const auto& solid : scene_.solid) {
      HideSeekBox expanded;
      for (unsigned i = 0; i < 3; ++i) {
        expanded.minimum[i] = solid.minimum[i] - body.maximum[i] + 0.002F;
        expanded.maximum[i] = solid.maximum[i] - body.minimum[i] - 0.002F;
      }
      if (Ray(expanded, s.position, direction, distance))
        return HideSeekReject::kNoSpace;
    }
    s.movement_credit = std::min(0.05F, budget - horizontal);
    s.vertical_credit = std::min(0.05F, vertical_budget - std::abs(delta[1]));
    s.position = p;
    s.last_move = now_;
    s.pending_prop = 0;
    s.transform_due = 0;
    ++revision_;
  }
  return HideSeekReject::kNone;
}

HideSeekReject HideSeekMatch::SelectDisguise(HideSeekPlayer id,
                                             std::uint32_t choice,
                                             HideSeekTime now) {
  Advance(now, true);
  auto it = states_.find(id);
  if (it == states_.end()) return HideSeekReject::kUnknownPlayer;
  auto& s = it->second;
  if (phase_ == HideSeekPhase::kResults) return HideSeekReject::kWrongPhase;
  if (s.role != HideSeekRole::kCat) return HideSeekReject::kWrongRole;
  if (std::find(s.offered.begin(), s.offered.end(), choice) == s.offered.end())
    return HideSeekReject::kNotOffered;
  if (choice == s.prop) return HideSeekReject::kInvalidValue;
  if (s.transform_due || now_ < s.cooldown_due)
    return HideSeekReject::kCooldown;
  if (!Space(s, choice, s.position, s.turn)) return HideSeekReject::kNoSpace;
  s.pending_prop = choice;
  s.transform_due = now_ + rules_.transform_ms;
  ++revision_;
  return HideSeekReject::kNone;
}

HideSeekReject HideSeekMatch::Rotate(HideSeekPlayer id, HideSeekTime now) {
  Advance(now, true);
  auto it = states_.find(id);
  if (it == states_.end()) return HideSeekReject::kUnknownPlayer;
  auto& s = it->second;
  if (phase_ == HideSeekPhase::kResults) return HideSeekReject::kWrongPhase;
  if (s.role != HideSeekRole::kCat) return HideSeekReject::kWrongRole;
  const auto turn = static_cast<std::uint8_t>((s.turn + 1U) % 4U);
  if (!Space(s, s.prop, s.position, turn)) return HideSeekReject::kNoSpace;
  s.turn = turn;
  s.transform_due = 0;
  s.pending_prop = 0;
  ++revision_;
  return HideSeekReject::kNone;
}

HideSeekReject HideSeekMatch::Reload(HideSeekPlayer id, HideSeekTime now) {
  Advance(now, true);
  auto it = states_.find(id);
  if (it == states_.end()) return HideSeekReject::kUnknownPlayer;
  auto& s = it->second;
  if (phase_ != HideSeekPhase::kHunting && phase_ != HideSeekPhase::kFrenzy)
    return HideSeekReject::kWrongPhase;
  if (s.role != HideSeekRole::kHunter) return HideSeekReject::kWrongRole;
  if (phase_ == HideSeekPhase::kFrenzy) return HideSeekReject::kNone;
  if (s.reload_due || now_ < s.next_attack) return HideSeekReject::kCooldown;
  if (s.magazine == rules_.magazine) return HideSeekReject::kInvalidValue;
  if (!s.reserve) return HideSeekReject::kOutOfStock;
  s.reload_due = now_ + rules_.reload_ms;
  ++revision_;
  return HideSeekReject::kNone;
}

std::optional<std::pair<HideSeekPlayer, float>> HideSeekMatch::Trace(
    const State& shooter, const HideSeekPoint& origin,
    const HideSeekPoint& direction, float range) const {
  std::optional<std::pair<HideSeekPlayer, float>> hit;
  for (const auto& b : scene_.solid)
    if (const auto distance = Ray(b, origin, direction, range))
      range = std::min(range, *distance);
  for (const auto& triangle : scene_.triangles)
    if (const auto distance = HsRay(triangle, origin, direction, range))
      range = *distance;
  for (const auto& [id, s] : states_) {
    if (s.member.player == shooter.member.player ||
        s.role != HideSeekRole::kCat)
      continue;
    auto distance =
        Ray(Box(s, s.prop, s.position, s.turn), origin, direction, range);
    if (distance) {
      if (const auto* model = Prop(s.prop); model && model->geometry) {
        HideSeekPoint local;
        for (unsigned a = 0; a < 3; ++a) local[a] = origin[a] - s.position[a];
        distance = hide_seek_assets::intersect(*model->geometry, local,
                                               direction, s.turn, range);
      }
    }
    if (distance) {
      if (*distance < range) {
        range = *distance;
        hit = std::pair{id, *distance};
      }
    }
  }
  return hit;
}

void HideSeekMatch::Damage(State& attacker, State& target, std::uint16_t amount,
                           HideSeekAttackResult& result) {
  if (target.role != HideSeekRole::kCat) return;
  ++result.hits;
  target.transform_due = 0;
  target.pending_prop = 0;
  target.hp =
      target.hp > amount ? static_cast<std::uint16_t>(target.hp - amount) : 0;
  if (target.hp == 0) {
    target.role = HideSeekRole::kConverting;
    target.stopped_hiding = now_;
    target.conversion_due = now_ + rules_.conversion_ms;
    target.prop = 0;
    ++attacker.captures;
    ++result.captures;
  }
}

HideSeekAttackResult HideSeekMatch::Attack(
    HideSeekPlayer id, std::uint64_t serial, HideSeekWeapon weapon,
    const HideSeekPoint& aim, HideSeekTime now,
    std::optional<HideSeekPoint> native_origin) {
  Advance(now, true);
  HideSeekAttackResult result;
  const auto reject = [&result](HideSeekReject why) {
    result.rejection = why;
    return result;
  };
  auto it = states_.find(id);
  if (it == states_.end()) return reject(HideSeekReject::kUnknownPlayer);
  auto& shooter = it->second;
  if (phase_ != HideSeekPhase::kHunting && phase_ != HideSeekPhase::kFrenzy)
    return reject(HideSeekReject::kWrongPhase);
  if (shooter.role != HideSeekRole::kHunter)
    return reject(HideSeekReject::kWrongRole);
  if ((weapon != HideSeekWeapon::kShotgun &&
       weapon != HideSeekWeapon::kHammer) ||
      !Finite(aim) || Length(aim) < .99F || Length(aim) > 1.01F)
    return reject(HideSeekReject::kInvalidValue);
  if (shooter.has_serial && serial <= shooter.last_serial)
    return reject(HideSeekReject::kReplay);
  if (now_ < shooter.next_attack || shooter.reload_due)
    return reject(HideSeekReject::kCooldown);
  const bool shotgun = weapon == HideSeekWeapon::kShotgun;
  if (shotgun && !shooter.magazine && phase_ != HideSeekPhase::kFrenzy)
    return reject(HideSeekReject::kEmptyMagazine);
  auto eye = shooter.position;
  eye[1] += 1.2F;
  auto origin = eye;
  bool muzzle_obstructed = false;
  if (native_origin) {
    if (!Finite(*native_origin)) return reject(HideSeekReject::kInvalidValue);
    HideSeekPoint delta{};
    for (unsigned i = 0; i < 3; ++i) delta[i] = (*native_origin)[i] - eye[i];
    const float distance = Length(delta);
    // Native muzzle bones and half-float quantisation may displace the ray
    // from the last accepted pose. Never accept a remote origin or allow a
    // muzzle on the far side of map cover to bypass that cover.
    if (distance > 2.25F) return reject(HideSeekReject::kInvalidValue);
    origin = *native_origin;
    if (distance > .001F) {
      const auto segment = Unit(delta);
      for (const auto& b : scene_.solid)
        muzzle_obstructed =
            muzzle_obstructed || Ray(b, eye, segment, distance).has_value();
      for (const auto& triangle : scene_.triangles)
        muzzle_obstructed = muzzle_obstructed ||
                            HsRay(triangle, eye, segment, distance).has_value();
    }
  }
  shooter.last_serial = serial;
  shooter.has_serial = true;
  shooter.next_attack =
      now_ + (shotgun ? rules_.shotgun_interval_ms : rules_.hammer_interval_ms);
  if (shotgun && phase_ != HideSeekPhase::kFrenzy) --shooter.magazine;
  if (phase_ != HideSeekPhase::kFrenzy) {
    const auto cost = shotgun ? rules_.shotgun_cost : rules_.hammer_cost;
    result.health_cost = std::min(cost, shooter.hp);
    shooter.hp = static_cast<std::uint16_t>(shooter.hp - result.health_cost);
  }
  const auto direction = Unit(aim);
  auto right = Cross(direction, {0.F, 1.F, 0.F});
  if (Length(right) < .01F)
    right = {1.F, 0.F, 0.F};
  else
    right = Unit(right);
  const auto up = Unit(Cross(right, direction));
  std::mt19937 random(seed_ ^ static_cast<std::uint32_t>(serial) ^
                      static_cast<std::uint32_t>(serial >> 32U) ^
                      shooter.member.actor);
  for (unsigned pellet = 0; !muzzle_obstructed && pellet < (shotgun ? 7U : 1U);
       ++pellet) {
    auto ray_direction = direction;
    if (shotgun && pellet) {
      // Stable server-only spread, independent of client-provided target
      // reports.
      const float angle = static_cast<float>(random()) /
                          static_cast<float>(std::mt19937::max()) * 6.2831853F;
      const float radius = std::sqrt(static_cast<float>(random()) /
                                     static_cast<float>(std::mt19937::max())) *
                           rules_.shotgun_spread_radians;
      for (unsigned i = 0; i < 3; ++i)
        ray_direction[i] += right[i] * std::cos(angle) * radius +
                            up[i] * std::sin(angle) * radius;
      ray_direction = Unit(ray_direction);
    }
    if (const auto hit =
            Trace(shooter, origin, ray_direction,
                  shotgun ? rules_.shotgun_range : rules_.hammer_range))
      Damage(shooter, states_.at(hit->first),
             shotgun ? rules_.pellet_damage : rules_.hammer_damage, result);
  }
  if (!shooter.hp) shooter.role = HideSeekRole::kEliminated;
  ++revision_;
  Decide();
  return result;
}

void HideSeekMatch::Leave(HideSeekPlayer id, HideSeekTime now) {
  Advance(now, true);
  auto it = states_.find(id);
  if (it == states_.end() || phase_ == HideSeekPhase::kResults ||
      it->second.role == HideSeekRole::kDeparted)
    return;
  auto& s = it->second;
  if (s.role == HideSeekRole::kCat) s.stopped_hiding = now_;
  s.role = HideSeekRole::kDeparted;
  s.prop = 0;
  s.transform_due = s.conversion_due = s.reload_due = 0;
  ++revision_;
  Decide();
}

std::uint32_t HideSeekMatch::Survived(const State& s) const {
  if (s.initial != HideSeekRole::kCat) return 0;
  const auto until = s.stopped_hiding >= 0 ? s.stopped_hiding : now_;
  return Remaining(std::min(until, end_), hunt_);
}

HideSeekSnapshot HideSeekMatch::Snapshot(HideSeekPlayer id) const {
  const auto it = states_.find(id);
  if (it == states_.end())
    throw std::invalid_argument("unknown hide_seek_v1 recipient");
  const auto& s = it->second;
  HideSeekSnapshot result;
  result.round = round_;
  result.revision = revision_;
  result.player = id;
  result.phase = phase_;
  result.initial_role = s.initial;
  result.role = s.role;
  result.winner = winner_;
  result.remaining_ms =
      phase_ == HideSeekPhase::kResults
          ? 0
          : Remaining(phase_ == HideSeekPhase::kHiding ? hunt_ : end_, now_);
  result.conversion_ms = Remaining(s.conversion_due, now_);
  result.transform_ms = Remaining(s.transform_due, now_);
  result.cooldown_ms = Remaining(s.cooldown_due, now_);
  result.hp = s.hp;
  result.magazine = s.magazine;
  result.reserve = s.reserve;
  if (s.role == HideSeekRole::kCat) {
    result.offered = s.offered;
    result.disguise = s.prop;
  }
  for (const auto& [player, other] : states_) {
    if (other.role == HideSeekRole::kCat) ++result.cats;
    if (other.role == HideSeekRole::kHunter) ++result.hunters;
    if (other.role == HideSeekRole::kConverting) ++result.converting;
    if (phase_ == HideSeekPhase::kResults) {
      const bool departed = other.role == HideSeekRole::kDeparted;
      const bool success = winner_ != HideSeekWinner::kAbandoned && !departed &&
                           (other.initial == HideSeekRole::kHunter
                                ? winner_ == HideSeekWinner::kHunters
                                : other.role == HideSeekRole::kCat);
      result.results.push_back({player, other.initial, other.role,
                                Survived(other), other.captures, success,
                                departed});
    }
  }
  return result;
}

std::vector<HideSeekAppearance> HideSeekMatch::Appearances() const {
  std::vector<HideSeekAppearance> result;
  for (const auto& [id, s] : states_)
    result.push_back({id, s.member.actor, s.prop, s.role, s.position, s.turn});
  return result;
}

std::vector<HideSeekHint> HideSeekMatch::TakeHints() {
  auto result = std::move(hints_);
  hints_.clear();
  return result;
}

HideSeekTime HideSeekMatch::NextDeadline() const noexcept {
  if (phase_ == HideSeekPhase::kResults) return 0;
  auto due = phase_ == HideSeekPhase::kHiding    ? hunt_
             : phase_ == HideSeekPhase::kHunting ? frenzy_
                                                 : end_;
  due = std::min(due, next_hint_);
  for (const auto& [id, s] : states_) {
    static_cast<void>(id);
    for (const auto pending : {s.conversion_due, s.transform_due, s.reload_due})
      if (pending) due = std::min(due, pending);
  }
  return due;
}
}  // namespace bf::runtime
