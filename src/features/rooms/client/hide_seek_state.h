#ifndef BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_CLIENT_HIDE_SEEK_STATE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_CLIENT_HIDE_SEEK_STATE_H_

/** @file hide_seek_state.h @brief Portable C++17 state and input policy for the
 * native bridge. */
#include <algorithm>
#include <optional>
#include <string>

#include "protocol/battle/hide_seek_wire.h"

namespace bf::hide_seek_client {
struct Controls {
  bool move{}, attack{}, reload{}, choose{}, rotate{}, blind{},
      hunter_spectator{}, infinite_ammo{};
};

/** Presentation state only. The bridge cannot award health, damage, captures or
 * victory. */
class State final {
 public:
  void reset() {
    snapshot_.reset();
    actors_.clear();
    last_hint_ = 0;
    received_ = 0;
    visual_revision_ = 0;
  }

  bool receive(const hide_seek_wire::State& value, std::uint64_t now) {
    if (snapshot_ &&
        (snapshot_->round != value.round || snapshot_->player != value.player ||
         value.revision < snapshot_->revision))
      return false;
    snapshot_ = value;
    received_ = now;
    return true;
  }

  bool receive(const hide_seek_wire::Appearances& value) {
    if (!snapshot_ || value.round != snapshot_->round ||
        value.revision < snapshot_->revision ||
        value.revision < visual_revision_)
      return false;
    actors_ = value.actors;
    visual_revision_ = value.revision;
    return true;
  }

  std::vector<hide_seek_wire::Hint> receive(
      const hide_seek_wire::Hints& value) {
    std::vector<hide_seek_wire::Hint> result;
    if (!snapshot_ || value.round != snapshot_->round ||
        snapshot_->phase == 0 || snapshot_->phase == 3)
      return result;
    for (const auto& h : value.events)
      if (h.serial > last_hint_) {
        result.push_back(h);
        last_hint_ = h.serial;
      }
    return result;
  }

  const std::optional<hide_seek_wire::State>& snapshot() const {
    return snapshot_;
  }

  const std::vector<hide_seek_wire::Appearance>& actors() const {
    return actors_;
  }

  Controls controls() const {
    Controls c;
    if (!snapshot_) return c;
    const auto& s = *snapshot_;
    const bool active = s.phase != 3;
    c.move = active && (s.role == 0 || (s.role == 1 && s.phase != 0));
    c.attack = active && s.role == 1 && s.phase != 0 && s.hp > 0;
    c.reload = c.attack && s.phase != 2;
    c.choose = active && s.role == 0 && !s.cooldown_ms && !s.transform_ms;
    c.rotate = active && s.role == 0;
    c.blind = active && ((s.role == 1 && s.phase == 0) || s.role == 2);
    c.hunter_spectator = active && (s.role == 3 || s.role == 4);
    c.infinite_ammo = active && s.role == 1 && s.phase == 2;
    return c;
  }

  std::vector<std::uint32_t> spectator_targets() const {
    std::vector<std::uint32_t> result;
    if (!controls().hunter_spectator) return result;
    for (const auto& a : actors_)
      if (a.role == 1) result.push_back(a.actor);
    return result;
  }

  std::uint32_t remaining_ms(std::uint64_t now) const {
    if (!snapshot_) return 0;
    const auto elapsed = now >= received_ ? now - received_ : 0;
    return elapsed >= snapshot_->remaining_ms
               ? 0
               : static_cast<std::uint32_t>(snapshot_->remaining_ms - elapsed);
  }

  std::wstring title() const {
    if (!snapshot_) return L"等待躲猫猫状态";
    const auto& s = *snapshot_;
    if (s.phase == 3)
      return s.winner == 1   ? L"红方获胜"
             : s.winner == 2 ? L"蓝方获胜"
                             : L"回合结束";
    if (s.role == 2) return L"正在转化为猎人";
    if (s.role == 3) return L"本回合已淘汰";
    if (s.role == 4) return L"本回合观战";
    if (s.phase == 0) return s.role == 0 ? L"选择伪装并躲藏" : L"等待猫躲藏";
    if (s.phase == 2)
      return s.role == 0 ? L"最后60秒：躲避疯狂猎人！"
                         : L"疯狂时刻：无限弹药，无需换弹";
    return s.role == 0 ? L"躲避猎人，存活到回合结束"
                       : L"寻找伪装物件，攻击会消耗血量";
  }

 private:
  std::optional<hide_seek_wire::State> snapshot_;
  std::vector<hide_seek_wire::Appearance> actors_;
  std::uint64_t last_hint_{}, received_{}, visual_revision_{};
};
}  // namespace bf::hide_seek_client

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_ROOMS_CLIENT_HIDE_SEEK_STATE_H_
