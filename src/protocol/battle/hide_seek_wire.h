#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_WIRE_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_WIRE_H_

/** @file hide_seek_wire.h @brief Bounded C++17 wire contract shared by the
 * server and 32-bit bridge. */
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

namespace bf::hide_seek_wire {
inline constexpr std::uint16_t request_opcode = 65082, state_opcode = 65083;
inline constexpr std::uint16_t create_opcode = 65087;
inline constexpr std::uint16_t native_create_opcode = 65088;
inline constexpr std::uint16_t appearance_opcode = 65084, hint_opcode = 65085,
                               capability_opcode = 65086;
inline constexpr std::uint32_t version = 1;
inline constexpr std::uint32_t render_props = 1, prop_collision = 2,
                               authoritative_health = 4, input_control = 8,
                               equipment_override = 16, private_hud = 32,
                               spatial_audio = 64, spectator_control = 128,
                               native_attack_ray = 256;
inline constexpr std::uint32_t required_capabilities = 511;
enum class Command : std::uint8_t {
  Hello,
  Poll,
  Select,
  Rotate,
  Reload,
  Attack,
  Move,
  AttackRay
};

struct Request {
  Command command{Command::Hello};
  std::uint64_t round{}, serial{};
  std::uint32_t value{};
  std::uint8_t weapon{};
  std::array<float, 3> point{};
  std::array<float, 3> origin{};  // AttackRay only, metres in map coordinates.
};

struct Score {
  std::uint64_t player{};
  std::uint8_t initial{}, role{};
  std::uint32_t survived_ms{}, captures{};
  bool success{}, departed{};
};

struct State {
  std::uint64_t round{}, revision{}, player{};
  std::uint8_t phase{}, initial{}, role{}, winner{}, cats{}, hunters{},
      converting{}, rejection{};
  std::uint32_t remaining_ms{}, conversion_ms{}, transform_ms{}, cooldown_ms{};
  std::uint16_t hp{}, magazine{}, reserve{};
  std::array<std::uint32_t, 3> offered{};
  std::uint32_t disguise{};
  std::vector<Score> scores;
};

struct Appearance {
  std::uint64_t player{};
  std::uint32_t actor{}, prop{};
  std::uint8_t role{}, turn{};
  std::array<float, 3> point{};
};

struct Appearances {
  std::uint64_t round{}, revision{};
  std::vector<Appearance> actors;
};

struct Hint {
  std::uint64_t serial{};
  std::uint32_t actor{};
  std::array<float, 3> point{};
};

struct Hints {
  std::uint64_t round{};
  std::vector<Hint> events;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {
    if ((!data && size) || size > 4096)
      throw std::invalid_argument("hide_seek_v1 payload limit");
  }

  std::uint64_t integer(unsigned width) {
    if (width > 8 || width > size_ - position_)
      throw std::invalid_argument("hide_seek_v1 truncated payload");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < width; ++i)
      v |= std::uint64_t(data_[position_++]) << (8 * i);
    return v;
  }

  std::uint8_t byte() { return static_cast<std::uint8_t>(integer(1)); }

  std::uint16_t small() { return static_cast<std::uint16_t>(integer(2)); }

  std::uint32_t word() { return static_cast<std::uint32_t>(integer(4)); }

  std::uint64_t wide() { return integer(8); }

  float real() {
    auto bits = word();
    float f;
    std::memcpy(&f, &bits, 4);
    if (!std::isfinite(f) || std::abs(f) > 100'000.F)
      throw std::invalid_argument("hide_seek_v1 nonfinite point");
    return f;
  }

  void header() {
    if (word() != version)
      throw std::invalid_argument("hide_seek_v1 version mismatch");
  }

  void end() {
    if (position_ != size_)
      throw std::invalid_argument("hide_seek_v1 trailing payload");
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_, position_{};
};

class Writer {
 public:
  void integer(std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
      data.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }

  void byte(std::uint8_t v) { integer(v, 1); }

  void small(std::uint16_t v) { integer(v, 2); }

  void word(std::uint32_t v) { integer(v, 4); }

  void wide(std::uint64_t v) { integer(v, 8); }

  void real(float v) {
    if (!std::isfinite(v) || std::abs(v) > 100'000.F)
      throw std::invalid_argument("hide_seek_v1 invalid point");
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    word(bits);
  }

  std::vector<std::uint8_t> data;
};

inline void require(bool condition) {
  if (!condition) throw std::invalid_argument("hide_seek_v1 invalid payload");
}

inline Request decode_request(const std::uint8_t* bytes, std::size_t size) {
  Reader r(bytes, size);
  r.header();
  Request v;
  v.command = static_cast<Command>(r.byte());
  require(v.command <= Command::AttackRay);
  v.round = r.wide();
  v.serial = r.wide();
  switch (v.command) {
    case Command::Hello:
      v.value = r.word();
      require(v.round == 0);
      break;
    case Command::Select:
      v.value = r.word();
      require(v.value != 0);
      break;
    case Command::Attack:
    case Command::AttackRay:
      v.weapon = r.byte();
      require(v.weapon < 2);
      for (auto& f : v.point) f = r.real();
      if (v.command == Command::AttackRay)
        for (auto& f : v.origin) f = r.real();
      break;
    case Command::Move:
      for (auto& f : v.point) f = r.real();
      break;
    default:
      break;
  }
  if (v.command != Command::Hello) require(v.round != 0);
  r.end();
  return v;
}

inline std::vector<std::uint8_t> encode(const Request& v) {
  Writer w;
  w.word(version);
  w.byte(static_cast<std::uint8_t>(v.command));
  w.wide(v.round);
  w.wide(v.serial);
  switch (v.command) {
    case Command::Hello:
    case Command::Select:
      w.word(v.value);
      break;
    case Command::Attack:
    case Command::AttackRay:
      w.byte(v.weapon);
      for (float f : v.point) w.real(f);
      if (v.command == Command::AttackRay)
        for (float f : v.origin) w.real(f);
      break;
    case Command::Move:
      for (float f : v.point) w.real(f);
      break;
    default:
      break;
  }
  static_cast<void>(decode_request(w.data.data(), w.data.size()));
  return w.data;
}

inline State decode_state(const std::uint8_t* bytes, std::size_t size) {
  Reader r(bytes, size);
  r.header();
  State v;
  v.round = r.wide();
  v.revision = r.wide();
  v.player = r.wide();
  v.phase = r.byte();
  v.initial = r.byte();
  v.role = r.byte();
  v.winner = r.byte();
  v.cats = r.byte();
  v.hunters = r.byte();
  v.converting = r.byte();
  v.rejection = r.byte();
  require(v.round && v.revision && v.player && v.phase <= 3 && v.initial <= 1 &&
          v.role <= 4 && v.winner <= 3 && v.rejection <= 11);
  require(v.cats + v.hunters + v.converting <= 8);
  v.remaining_ms = r.word();
  v.conversion_ms = r.word();
  v.transform_ms = r.word();
  v.cooldown_ms = r.word();
  require(v.remaining_ms <= 720'000 && v.conversion_ms <= 30'000 &&
          v.transform_ms <= 5'000 && v.cooldown_ms <= 120'000);
  v.hp = r.small();
  v.magazine = r.small();
  v.reserve = r.small();
  for (auto& p : v.offered) p = r.word();
  v.disguise = r.word();
  const auto count = r.byte();
  require(count <= 8 && (v.phase == 3 || count == 0));
  require(v.role == 0 ||
          (v.offered == std::array<std::uint32_t, 3>{} && v.disguise == 0));
  std::set<std::uint64_t> seen;
  for (unsigned i = 0; i < count; ++i) {
    Score s;
    s.player = r.wide();
    s.initial = r.byte();
    s.role = r.byte();
    s.survived_ms = r.word();
    s.captures = r.word();
    const auto success = r.byte(), departed = r.byte();
    require(s.player && seen.insert(s.player).second && s.initial <= 1 &&
            s.role <= 4 && success <= 1 && departed <= 1 && s.captures <= 8 &&
            s.survived_ms <= 600'000);
    s.success = success != 0;
    s.departed = departed != 0;
    v.scores.push_back(s);
  }
  r.end();
  return v;
}

inline std::vector<std::uint8_t> encode(const State& v) {
  require(v.scores.size() <= 8);
  Writer w;
  w.word(version);
  w.wide(v.round);
  w.wide(v.revision);
  w.wide(v.player);
  for (auto b : {v.phase, v.initial, v.role, v.winner, v.cats, v.hunters,
                 v.converting, v.rejection})
    w.byte(b);
  for (auto n :
       {v.remaining_ms, v.conversion_ms, v.transform_ms, v.cooldown_ms})
    w.word(n);
  for (auto n : {v.hp, v.magazine, v.reserve}) w.small(n);
  for (auto n : v.offered) w.word(n);
  w.word(v.disguise);
  w.byte(static_cast<std::uint8_t>(v.scores.size()));
  for (const auto& s : v.scores) {
    w.wide(s.player);
    w.byte(s.initial);
    w.byte(s.role);
    w.word(s.survived_ms);
    w.word(s.captures);
    w.byte(s.success ? 1 : 0);
    w.byte(s.departed ? 1 : 0);
  }
  static_cast<void>(decode_state(w.data.data(), w.data.size()));
  return w.data;
}

inline Appearances decode_appearances(const std::uint8_t* bytes,
                                      std::size_t size) {
  Reader r(bytes, size);
  r.header();
  Appearances v;
  v.round = r.wide();
  v.revision = r.wide();
  auto count = r.byte();
  require(v.round && v.revision && count <= 8);
  std::set<std::uint64_t> seen;
  std::set<std::uint32_t> actors;
  for (unsigned i = 0; i < count; ++i) {
    Appearance a;
    a.player = r.wide();
    a.actor = r.word();
    a.prop = r.word();
    a.role = r.byte();
    a.turn = r.byte();
    for (auto& f : a.point) f = r.real();
    require(a.player && seen.insert(a.player).second &&
            actors.insert(a.actor).second && a.actor >= 1'000'000 &&
            a.actor <= 1'700'000 && (a.actor - 1'000'000) % 100'000 == 0 &&
            a.role <= 4 && a.turn < 4);
    require(a.role == 0 ? a.prop != 0 : a.prop == 0);
    v.actors.push_back(a);
  }
  r.end();
  return v;
}

inline std::vector<std::uint8_t> encode(const Appearances& v) {
  require(v.actors.size() <= 8);
  Writer w;
  w.word(version);
  w.wide(v.round);
  w.wide(v.revision);
  w.byte(static_cast<std::uint8_t>(v.actors.size()));
  for (const auto& a : v.actors) {
    w.wide(a.player);
    w.word(a.actor);
    w.word(a.prop);
    w.byte(a.role);
    w.byte(a.turn);
    for (float f : a.point) w.real(f);
  }
  static_cast<void>(decode_appearances(w.data.data(), w.data.size()));
  return w.data;
}

inline Hints decode_hints(const std::uint8_t* bytes, std::size_t size) {
  Reader r(bytes, size);
  r.header();
  Hints v;
  v.round = r.wide();
  const auto count = r.byte();
  require(v.round && count <= 8);
  std::uint64_t previous = 0;
  for (unsigned i = 0; i < count; ++i) {
    Hint h;
    h.serial = r.wide();
    h.actor = r.word();
    for (auto& f : h.point) f = r.real();
    require(h.serial > previous && h.actor >= 1'000'000 &&
            h.actor <= 1'700'000 && (h.actor - 1'000'000) % 100'000 == 0);
    previous = h.serial;
    v.events.push_back(h);
  }
  r.end();
  return v;
}

inline std::vector<std::uint8_t> encode(const Hints& v) {
  require(v.events.size() <= 8);
  Writer w;
  w.word(version);
  w.wide(v.round);
  w.byte(static_cast<std::uint8_t>(v.events.size()));
  for (const auto& h : v.events) {
    w.wide(h.serial);
    w.word(h.actor);
    for (float f : h.point) w.real(f);
  }
  static_cast<void>(decode_hints(w.data.data(), w.data.size()));
  return w.data;
}
}  // namespace bf::hide_seek_wire

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_WIRE_H_
