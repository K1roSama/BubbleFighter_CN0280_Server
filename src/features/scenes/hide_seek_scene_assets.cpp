#include "features/scenes/hide_seek_scene_assets.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "features/scenes/native_wb_scene.h"
#include "features/scenes/native_x_scene.h"

namespace bf::runtime {
namespace {
std::vector<std::uint8_t> Read(const std::filesystem::path& path,
                               std::size_t maximum) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() <= 0 || std::size_t(in.tellg()) > maximum)
    throw std::invalid_argument("hide_seek_v1 missing/bounded asset file");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size())))
    throw std::invalid_argument("hide_seek_v1 asset read failed");
  return bytes;
}

std::uint32_t Integer(const std::vector<std::uint8_t>& b, std::size_t offset) {
  if (offset > b.size() || b.size() - offset < 4)
    throw std::invalid_argument("hide_seek_v1 invalid material map");
  std::uint32_t n;
  std::memcpy(&n, b.data() + offset, 4);
  return n;
}
}  // namespace

std::shared_ptr<const HideSeekScene> LoadHideSeekSceneAssets(
    const std::filesystem::path& directory, bool development) {
  // A plain marker cannot approve unknown assets. Production stays closed until
  // the graphical acceptance receipt format and its content binding are
  // implemented.
  if (!development)
    throw std::invalid_argument(
        "hide_seek_v1 graphical acceptance pending; production profile disabled");
  const auto data = Read(directory / "byway.hsp", 16 * 1024 * 1024);
  auto catalog = hide_seek_assets::decode(data.data(), data.size());
  const auto x = ReadNativeX(Read(directory / "track.x", 128 * 1024 * 1024));
  const auto wb = ReadNativeWb(Read(directory / "track.wb", 64 * 1024 * 1024));
  auto scene = std::make_shared<HideSeekScene>();
  scene->map_key = "rel_Village01_Byway";
  for (auto& p : catalog.props) {
    auto shared = std::make_shared<const hide_seek_assets::Prop>(std::move(p));
    scene->props.push_back({shared->id,
                            shared->name,
                            "byway.hsp",
                            {shared->minimum, shared->maximum},
                            shared});
  }
  for (unsigned i = 1; i <= 8; ++i) {
    for (unsigned team = 1; team <= 2; ++team) {
      const auto* spawn =
          wb.FindDummy("#spawn_T" + std::to_string(team) + std::to_string(i));
      if (!spawn) continue;
      const HideSeekPoint point{spawn->pose[0] / 15.F, spawn->pose[1] / 15.F,
                                spawn->pose[2] / 15.F};
      (team == 1 ? scene->red_spawns : scene->blue_spawns).push_back(point);
    }
  }
  std::map<std::string, const NativeWbMesh*> records;
  for (const auto& r : wb.records)
    if (const auto* m = std::get_if<NativeWbMesh>(&r.value))
      records[m->name] = m;
  for (std::size_t index = 0; index < x.nodes.size(); ++index) {
    const auto& node = x.nodes[index];
    if (node.reference || node.type_name != "Mesh" ||
        node.name.starts_with("ani_") || !records.contains(node.name))
      continue;
    if (node.parent)
      throw std::invalid_argument(
          "hide_seek_v1 world mesh has an unhandled frame transform");
    const auto& wm = *records.at(node.name);
    std::set<std::uint32_t> enabled;
    for (std::size_t i = 0; i < wm.subsets.size(); ++i) {
      if (i >= wm.materials.size())
        throw std::invalid_argument("hide_seek_v1 world material mismatch");
      auto texture = wm.materials[i].texture;
      std::transform(
          texture.begin(), texture.end(), texture.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (texture.starts_with("alpha_") || texture == "no texture") continue;
      enabled.insert(wm.subsets[i]);
    }
    const std::vector<std::uint8_t>* material = nullptr;
    for (const auto& child : x.nodes)
      if (child.parent == index && child.type_name == "MeshMaterialList" &&
          child.data)
        material = &*child.data;
    if (!material) continue;
    const auto mesh = GetNativeXMesh(x, index);
    if (Integer(*material, 4) != mesh.faces.size())
      throw std::invalid_argument(
          "hide_seek_v1 world material face count mismatch");
    for (std::size_t f = 0; f < mesh.faces.size(); ++f) {
      if (!enabled.contains(Integer(*material, 8 + f * 4))) continue;
      const auto& face = mesh.faces[f];
      for (std::size_t j = 1; j + 1 < face.size(); ++j) {
        HideSeekTriangle triangle{mesh.vertices[face[0]],
                                  mesh.vertices[face[j]],
                                  mesh.vertices[face[j + 1]]};
        for (auto* point : {&triangle.a, &triangle.b, &triangle.c})
          for (float& v : *point) v /= 15.F;
        if (HsDot(HsCross(HsSub(triangle.b, triangle.a),
                          HsSub(triangle.c, triangle.a)),
                  HsCross(HsSub(triangle.b, triangle.a),
                          HsSub(triangle.c, triangle.a))) < 1e-12F)
          continue;
        scene->triangles.push_back(triangle);
        if (scene->triangles.size() > 500000)
          throw std::invalid_argument(
              "hide_seek_v1 world triangle budget exceeded");
      }
    }
  }
  if (!scene->Valid() || scene->triangles.empty())
    throw std::invalid_argument("hide_seek_v1 incomplete native map profile");
  return scene;
}
}  // namespace bf::runtime
