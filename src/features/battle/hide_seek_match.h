#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_MATCH_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_MATCH_H_

/** @file hide_seek_match.h @brief Server-owned hide_seek_v1 rules, geometry and
 * round state. */

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "features/battle/hide_seek_geometry.h"
#include "protocol/battle/hide_seek_assets.h"

namespace bf::runtime {

using HideSeekPlayer = std::uint64_t;
using HideSeekTime =
    std::int64_t;  // Monotonic milliseconds, never wall-clock time.
using HideSeekPoint = std::array<float, 3>;

enum class HideSeekPhase : std::uint8_t {
  kHiding,
  kHunting,
  kFrenzy,
  kResults
};
enum class HideSeekRole : std::uint8_t {
  kCat,
  kHunter,
  kConverting,
  kEliminated,
  kDeparted
};
enum class HideSeekWeapon : std::uint8_t { kShotgun, kHammer };
enum class HideSeekWinner : std::uint8_t { kNone, kCats, kHunters, kAbandoned };
enum class HideSeekReject : std::uint8_t {
  kNone,
  kUnknownPlayer,
  kWrongPhase,
  kWrongRole,
  kInvalidValue,
  kReplay,
  kCooldown,
  kEmptyMagazine,
  kOutOfStock,
  kNotOffered,
  kNoSpace,
  kMovement,
};

struct HideSeekRules final {
  static constexpr std::uint32_t kVersion = 1;
  static constexpr const char* kBusinessKey = "hide_seek_v1";
  HideSeekTime hiding_ms{30'000}, hunting_ms{240'000}, frenzy_ms{60'000};
  HideSeekTime conversion_ms{5'000}, transform_ms{1'000},
      disguise_cooldown_ms{20'000};
  HideSeekTime hint_ms{30'000}, shotgun_interval_ms{1'024},
      hammer_interval_ms{352};
  HideSeekTime reload_ms{4'000};
  std::uint16_t hunter_hp{150}, cat_hp{60}, shotgun_cost{5}, hammer_cost{3};
  std::uint16_t pellet_damage{10}, hammer_damage{20};
  std::uint16_t magazine{5}, reserve{30};
  float hunter_speed{6.0F}, cat_speed_ratio{1.1F};
  float shotgun_range{300.0F}, hammer_range{2.0F},
      shotgun_spread_radians{0.075F};
  [[nodiscard]] bool Valid() const noexcept;
  [[nodiscard]] static std::size_t HunterCount(std::size_t players) noexcept;
};

struct HideSeekBox final {
  HideSeekPoint minimum{}, maximum{};
  [[nodiscard]] bool Valid() const noexcept;
};

/** A reviewed instance from this map. Its model/bounds must share the same
 * asset provenance. */
struct HideSeekProp final {
  std::uint32_t id{};
  std::string name, model;
  HideSeekBox bounds;
  std::shared_ptr<const hide_seek_assets::Prop> geometry{};
};

struct HideSeekScene final {
  std::string map_key;
  std::vector<HideSeekProp> props;
  std::vector<HideSeekBox> solid;
  std::vector<HideSeekTriangle> triangles;
  std::vector<HideSeekPoint> red_spawns, blue_spawns;
  [[nodiscard]] bool Valid() const noexcept;
};

struct HideSeekMember final {
  HideSeekPlayer player{};
  std::uint32_t actor{};
  std::uint8_t seat{};
};

struct HideSeekScore final {
  HideSeekPlayer player{};
  HideSeekRole initial_role{HideSeekRole::kCat}, role{HideSeekRole::kCat};
  std::uint32_t survived_ms{}, captures{};
  bool task_success{false}, departed{false};
};

/** Only the receiving player gets health, candidate IDs and transform/cooldown
 * state. */
struct HideSeekSnapshot final {
  std::uint64_t round{}, revision{};
  HideSeekPlayer player{};
  HideSeekPhase phase{HideSeekPhase::kHiding};
  HideSeekRole initial_role{HideSeekRole::kCat}, role{HideSeekRole::kCat};
  HideSeekWinner winner{HideSeekWinner::kNone};
  std::uint32_t remaining_ms{}, conversion_ms{}, transform_ms{}, cooldown_ms{};
  std::uint16_t hp{}, magazine{}, reserve{};
  std::uint8_t cats{}, hunters{}, converting{};
  std::array<std::uint32_t, 3> offered{};
  std::uint32_t disguise{};
  std::vector<HideSeekScore> results;  // Empty until the round has ended.
};

struct HideSeekAppearance final {
  HideSeekPlayer player{};
  std::uint32_t actor{}, prop{};
  HideSeekRole role{HideSeekRole::kCat};
  HideSeekPoint position{};
  std::uint8_t quarter_turn{};
};

struct HideSeekHint final {
  std::uint64_t serial{};
  std::uint32_t actor{};
  HideSeekPoint position{};  // Audio-only event. Never a map/outline marker.
};

struct HideSeekAttackResult final {
  HideSeekReject rejection{HideSeekReject::kNone};
  std::uint8_t hits{}, captures{};
  std::uint16_t health_cost{};
};

/**
 * A single executor owns this state. Attack rays are resolved here before
 * zero-health victory checks, so a hunter's last shot stays effective. No
 * client HP, damage, targets or arbitrary model paths are accepted. All public
 * mutations advance the same monotonic clock.
 */
class HideSeekMatch final {
 public:
  HideSeekMatch(HideSeekRules rules, HideSeekScene scene,
                std::span<const HideSeekMember> members, std::uint64_t round,
                std::uint32_t seed, HideSeekTime now,
                std::span<const HideSeekPlayer> selected_hunters = {});
  void Tick(HideSeekTime now);
  [[nodiscard]] HideSeekReject Move(HideSeekPlayer player,
                                    const HideSeekPoint& position,
                                    HideSeekTime now);
  [[nodiscard]] HideSeekReject SelectDisguise(HideSeekPlayer player,
                                              std::uint32_t prop,
                                              HideSeekTime now);
  [[nodiscard]] HideSeekReject Rotate(HideSeekPlayer player, HideSeekTime now);
  [[nodiscard]] HideSeekReject Reload(HideSeekPlayer player, HideSeekTime now);
  [[nodiscard]] HideSeekAttackResult Attack(
      HideSeekPlayer player, std::uint64_t serial, HideSeekWeapon weapon,
      const HideSeekPoint& direction, HideSeekTime now,
      std::optional<HideSeekPoint> native_origin = std::nullopt);
  void Leave(HideSeekPlayer player, HideSeekTime now);
  [[nodiscard]] HideSeekSnapshot Snapshot(HideSeekPlayer player) const;
  [[nodiscard]] std::vector<HideSeekAppearance> Appearances() const;
  [[nodiscard]] std::vector<HideSeekHint> TakeHints();

  [[nodiscard]] HideSeekWinner Winner() const noexcept { return winner_; }

  [[nodiscard]] HideSeekPhase Phase() const noexcept { return phase_; }

  [[nodiscard]] HideSeekTime NextDeadline() const noexcept;

  [[nodiscard]] const HideSeekRules& Rules() const noexcept { return rules_; }

  [[nodiscard]] const HideSeekScene& Scene() const noexcept { return scene_; }

 private:
  struct State final {
    HideSeekMember member;
    HideSeekRole initial{HideSeekRole::kCat}, role{HideSeekRole::kCat};
    HideSeekPoint position{};
    float movement_credit{0.05F};
    float vertical_credit{0.05F};
    std::uint16_t hp{}, magazine{}, reserve{};
    std::array<std::uint32_t, 3> offered{};
    std::uint32_t prop{}, pending_prop{}, captures{};
    std::uint8_t turn{};
    std::uint64_t last_serial{};
    bool has_serial{false};
    HideSeekTime last_move{}, next_attack{}, reload_due{}, transform_due{},
        cooldown_due{};
    HideSeekTime conversion_due{}, stopped_hiding{-1};
  };

  void Advance(HideSeekTime now, bool decide);
  void Decide();
  void Finish(HideSeekWinner winner);
  void Damage(State& attacker, State& target, std::uint16_t amount,
              HideSeekAttackResult& result);
  [[nodiscard]] const HideSeekProp* Prop(std::uint32_t id) const noexcept;
  [[nodiscard]] HideSeekBox Box(const State& state, std::uint32_t prop_id,
                                const HideSeekPoint& position,
                                std::uint8_t turn) const;
  [[nodiscard]] bool Space(const State& state, std::uint32_t prop_id,
                           const HideSeekPoint& position,
                           std::uint8_t turn) const;
  [[nodiscard]] std::optional<std::pair<HideSeekPlayer, float>> Trace(
      const State& attacker, const HideSeekPoint& origin,
      const HideSeekPoint& direction, float range) const;
  [[nodiscard]] std::uint32_t Survived(const State& state) const;
  HideSeekRules rules_;
  HideSeekScene scene_;
  std::map<HideSeekPlayer, State> states_;
  HideSeekPhase phase_{HideSeekPhase::kHiding};
  HideSeekWinner winner_{HideSeekWinner::kNone};
  HideSeekTime start_{}, now_{}, hunt_{}, frenzy_{}, end_{}, next_hint_{};
  std::uint64_t round_{}, revision_{1}, hint_serial_{};
  std::uint32_t seed_{};
  std::vector<HideSeekHint> hints_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_MATCH_H_
