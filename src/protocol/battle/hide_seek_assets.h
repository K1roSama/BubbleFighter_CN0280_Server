#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_ASSETS_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_ASSETS_H_

/**
 * @file hide_seek_assets.h
 * @brief 定义客户端与服务端共用的道具几何资源格式，单位为米且禁止客户端缩放。
 */
#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "protocol/battle/hide_seek_wire.h"

namespace bf::hide_seek_assets {
inline constexpr float native_units_per_metre = 15.F;

/** @brief 道具网格顶点，保存位置、法线和纹理坐标。 */
struct Vertex {
  float x{}, y{}, z{}, nx{}, ny{}, nz{}, u{}, v{};
};

/** @brief 使用同一纹理的三角形集合。 */
struct Surface {
  std::string texture;
  std::vector<Vertex> vertices;
};

/** @brief 道具身份、米制包围盒和实际表面几何。 */
struct Prop {
  std::uint32_t id{};
  std::string name;
  std::array<float, 3> minimum{}, maximum{};
  std::vector<Surface> surfaces;
};

/** @brief 客户端与服务端共享的完整道具目录。 */
struct Catalog {
  std::vector<Prop> props;
};

inline void string(hide_seek_wire::Writer& w, const std::string& s) {
  hide_seek_wire::require(!s.empty() && s.size() <= 128);
  w.small(static_cast<std::uint16_t>(s.size()));
  for (unsigned char c : s) w.byte(c);
}

/** @brief 带本地资源大小限制的几何读取器。 */
class Reader {
 public:
  Reader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {
    hide_seek_wire::require(p && n >= 12 && n <= 16 * 1024 * 1024);
  }

  std::uint32_t word() {
    hide_seek_wire::require(n_ - at_ >= 4);
    std::uint32_t v{};
    for (unsigned i = 0; i < 4; ++i) v |= std::uint32_t(p_[at_++]) << (i * 8);
    return v;
  }

  float real() {
    const auto v = word();
    float f;
    std::memcpy(&f, &v, 4);
    hide_seek_wire::require(std::isfinite(f) && std::abs(f) < 10000.F);
    return f;
  }

  std::string string() {
    hide_seek_wire::require(n_ - at_ >= 2);
    const std::size_t len = p_[at_] + (std::size_t(p_[at_ + 1]) << 8);
    at_ += 2;
    hide_seek_wire::require(len && len <= 128 && len <= n_ - at_);
    std::string s(reinterpret_cast<const char*>(p_ + at_), len);
    at_ += len;
    hide_seek_wire::require(
        s.find('\0') == std::string::npos &&
        s.find("..") == std::string::npos && s.find(':') == std::string::npos &&
        s.find('/') == std::string::npos && s.find('\\') == std::string::npos);
    return s;
  }

  void end() const { hide_seek_wire::require(at_ == n_); }

 private:
  const std::uint8_t* p_;
  std::size_t n_, at_{};
};

inline Catalog decode(const std::uint8_t* bytes, std::size_t size) {
  Reader r(bytes, size);
  hide_seek_wire::require(r.word() == 0x31505348 && r.word() == 1);  // HSP1
  const auto count = r.word();
  hide_seek_wire::require(count >= 3 && count <= 128);
  Catalog c;
  std::set<std::uint32_t> ids;
  std::size_t total = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    Prop p;
    p.id = r.word();
    hide_seek_wire::require(p.id && ids.insert(p.id).second);
    p.name = r.string();
    for (auto& f : p.minimum) f = r.real();
    for (auto& f : p.maximum) f = r.real();
    hide_seek_wire::require(std::abs(p.minimum[1]) < 0.001F &&
                            std::abs(p.minimum[0] + p.maximum[0]) < 0.001F &&
                            std::abs(p.minimum[2] + p.maximum[2]) < 0.001F);
    for (unsigned a = 0; a < 3; ++a)
      hide_seek_wire::require(p.maximum[a] - p.minimum[a] >= 0.2F &&
                              p.maximum[a] - p.minimum[a] <= 3.F);
    const auto surfaces = r.word();
    hide_seek_wire::require(surfaces && surfaces <= 16);
    std::array<float, 3> low{1e9F, 1e9F, 1e9F}, high{-1e9F, -1e9F, -1e9F};
    for (std::uint32_t j = 0; j < surfaces; ++j) {
      Surface s;
      s.texture = r.string();
      const auto n = r.word();
      hide_seek_wire::require(n && n % 3 == 0 && n <= 30000 &&
                              total + n <= 300000);
      total += n;
      for (std::uint32_t k = 0; k < n; ++k) {
        Vertex v;
        v.x = r.real();
        v.y = r.real();
        v.z = r.real();
        v.nx = r.real();
        v.ny = r.real();
        v.nz = r.real();
        v.u = r.real();
        v.v = r.real();
        const float pos[] = {v.x, v.y, v.z};
        for (unsigned a = 0; a < 3; ++a) {
          low[a] = std::min(low[a], pos[a]);
          high[a] = std::max(high[a], pos[a]);
          hide_seek_wire::require(pos[a] >= p.minimum[a] - .001F &&
                                  pos[a] <= p.maximum[a] + .001F);
        }
        s.vertices.push_back(v);
      }
      p.surfaces.push_back(std::move(s));
    }
    for (unsigned a = 0; a < 3; ++a)
      hide_seek_wire::require(std::abs(low[a] - p.minimum[a]) < .001F &&
                              std::abs(high[a] - p.maximum[a]) < .001F);
    c.props.push_back(std::move(p));
  }
  r.end();
  return c;
}

inline std::vector<std::uint8_t> encode(const Catalog& c) {
  hide_seek_wire::Writer w;
  w.word(0x31505348);
  w.word(1);
  w.word(static_cast<std::uint32_t>(c.props.size()));
  for (const auto& p : c.props) {
    w.word(p.id);
    string(w, p.name);
    for (float f : p.minimum) w.real(f);
    for (float f : p.maximum) w.real(f);
    w.word(static_cast<std::uint32_t>(p.surfaces.size()));
    for (const auto& s : p.surfaces) {
      string(w, s.texture);
      w.word(static_cast<std::uint32_t>(s.vertices.size()));
      for (const auto& v : s.vertices)
        for (float f : {v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v}) w.real(f);
    }
  }
  static_cast<void>(decode(w.data.data(), w.data.size()));
  return w.data;
}

inline const Prop* find(const Catalog& c, std::uint32_t id) {
  const auto it = std::find_if(c.props.begin(), c.props.end(),
                               [id](const auto& p) { return p.id == id; });
  return it == c.props.end() ? nullptr : &*it;
}

inline std::array<float, 3> rotate(std::array<float, 3> p, unsigned turns) {
  for (unsigned i = 0; i < (turns % 4); ++i) {
    const auto x = p[0];
    p[0] = -p[2];
    p[2] = x;
  }
  return p;
}

// Two-sided triangle intersection: rendered surfaces and server hits use
// identical vertices.
inline std::optional<float> intersect(const Prop& p,
                                      std::array<float, 3> origin,
                                      std::array<float, 3> direction,
                                      unsigned turn, float limit) {
  origin = rotate(origin, 4 - turn % 4);
  direction = rotate(direction, 4 - turn % 4);
  auto cross = [](const auto& a, const auto& b) {
    return std::array<float, 3>{a[1] * b[2] - a[2] * b[1],
                                a[2] * b[0] - a[0] * b[2],
                                a[0] * b[1] - a[1] * b[0]};
  };
  auto dot = [](const auto& a, const auto& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  std::optional<float> hit;
  for (const auto& s : p.surfaces)
    for (std::size_t i = 0; i < s.vertices.size(); i += 3) {
      const auto &a = s.vertices[i], &b = s.vertices[i + 1],
                 &c = s.vertices[i + 2];
      const std::array<float, 3> e1{b.x - a.x, b.y - a.y, b.z - a.z},
          e2{c.x - a.x, c.y - a.y, c.z - a.z},
          t{origin[0] - a.x, origin[1] - a.y, origin[2] - a.z};
      const auto h = cross(direction, e2);
      const float det = dot(e1, h);
      if (std::abs(det) < 1e-7F) continue;
      const float inv = 1.F / det, u = dot(t, h) * inv;
      if (u < 0.F || u > 1.F) continue;
      const auto q = cross(t, e1);
      const float v = dot(direction, q) * inv;
      if (v < 0.F || u + v > 1.F) continue;
      const float d = dot(e2, q) * inv;
      if (d >= 0.F && d < limit) {
        hit = d;
        limit = d;
      }
    }
  return hit;
}
}  // namespace bf::hide_seek_assets

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_HIDE_SEEK_ASSETS_H_
