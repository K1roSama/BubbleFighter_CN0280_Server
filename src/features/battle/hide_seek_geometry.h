#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_GEOMETRY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_GEOMETRY_H_

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace bf::runtime {
using HideSeekVector = std::array<float, 3>;

struct HideSeekTriangle {
  HideSeekVector a{}, b{}, c{};
};

inline HideSeekVector HsSub(const HideSeekVector& a, const HideSeekVector& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

inline HideSeekVector HsCross(const HideSeekVector& a,
                              const HideSeekVector& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

inline float HsDot(const HideSeekVector& a, const HideSeekVector& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline std::optional<float> HsRay(const HideSeekTriangle& t,
                                  const HideSeekVector& o,
                                  const HideSeekVector& d, float limit) {
  const auto ab = HsSub(t.b, t.a), ac = HsSub(t.c, t.a), h = HsCross(d, ac),
             v = HsSub(o, t.a);
  const float det = HsDot(ab, h);
  if (std::abs(det) < 1e-7F) return {};
  const float inv = 1.F / det, u = HsDot(v, h) * inv;
  if (u < 0 || u > 1) return {};
  const auto q = HsCross(v, ab);
  const float w = HsDot(d, q) * inv;
  if (w < 0 || u + w > 1) return {};
  const float hit = HsDot(ac, q) * inv;
  return hit >= 0 && hit < limit ? std::optional<float>{hit} : std::nullopt;
}

// Continuous separating-axis test for a translated AABB against a fixed
// triangle. The same test with zero motion checks whether a larger disguise
// fits a space.
inline bool HsSweep(const HideSeekTriangle& t, const HideSeekVector& lo,
                    const HideSeekVector& hi,
                    const HideSeekVector& velocity = {}) {
  for (unsigned i = 0; i < 3; ++i) {
    const float low = std::min(lo[i], lo[i] + velocity[i]),
                high = std::max(hi[i], hi[i] + velocity[i]);
    if (high < std::min({t.a[i], t.b[i], t.c[i]}) ||
        low > std::max({t.a[i], t.b[i], t.c[i]}))
      return false;
  }
  HideSeekVector centre{}, half{};
  for (unsigned i = 0; i < 3; ++i) {
    centre[i] = (lo[i] + hi[i]) * .5F;
    half[i] = std::max(0.F, (hi[i] - lo[i]) * .5F - .001F);
  }
  const std::array<HideSeekVector, 3> basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  const std::array<HideSeekVector, 3> edges{HsSub(t.b, t.a), HsSub(t.c, t.b),
                                            HsSub(t.a, t.c)};
  std::array<HideSeekVector, 13> axes{};
  unsigned count = 0;
  for (const auto& b : basis) axes[count++] = b;
  axes[count++] = HsCross(edges[0], edges[1]);
  for (const auto& e : edges)
    for (const auto& b : basis) axes[count++] = HsCross(e, b);
  float enter = 0.F, exit = 1.F;
  for (const auto& axis : axes) {
    if (HsDot(axis, axis) < 1e-12F) continue;
    const float x = HsDot(t.a, axis), y = HsDot(t.b, axis),
                z = HsDot(t.c, axis);
    const float minimum = std::min({x, y, z}), maximum = std::max({x, y, z});
    const float radius = std::abs(axis[0]) * half[0] +
                         std::abs(axis[1]) * half[1] +
                         std::abs(axis[2]) * half[2];
    const float start = HsDot(centre, axis), speed = HsDot(velocity, axis);
    if (std::abs(speed) < 1e-9F) {
      if (start + radius < minimum || start - radius > maximum) return false;
      continue;
    }
    float first = (minimum - radius - start) / speed,
          last = (maximum + radius - start) / speed;
    if (first > last) std::swap(first, last);
    enter = std::max(enter, first);
    exit = std::min(exit, last);
    if (enter > exit) return false;
  }
  return enter <= 1.F && exit >= 0.F;
}
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_HIDE_SEEK_GEOMETRY_H_
