#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_WB_SCENE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_WB_SCENE_H_

/** @file native_wb_scene.h @brief
 * 顺序读取原WB2的dummy、网格实例、材质与标量动画。 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bf::runtime {
/** @brief 原FloatKey的时间、值与可选Bezier控制量；不推定游戏逻辑含义。 */
struct NativeWbFloatKey final {
  float time;   ///< 原32位时间字段。
  float value;  ///< 原32位值字段。
  std::optional<std::array<float, 2>>
      bezier;  ///< 插值字段为0时存在的两个控制量。
};

/** @brief 原六路标量轨道中的一路；插值字段非零时每关键帧只有两个float。 */
struct NativeWbFloatTrack final {
  std::optional<std::uint32_t> interpolation;  ///< 零关键帧不消费插值字段。
  std::vector<NativeWbFloatKey> keys;          ///< 原顺序关键帧，不重排或插值。
};

/** @brief 原材质有贴图时的完整附加字段；未知参数保持原位。 */
struct NativeWbMaterialData final {
  std::uint32_t subset;                 ///< 原子集编号。
  std::string name;                     ///< 原单字节编码名称，未经有损转换。
  std::array<float, 9> parameters;      ///< 原九个标量参数。
  std::string properties;               ///< 原属性文本字节，不执行XML或脚本。
  std::uint32_t resource_flag;          ///< 仅值1表示随后带资源字符串。
  std::optional<std::string> resource;  ///< 原可选资源字符串。
  std::array<std::uint32_t, 6> flags;   ///< 保留原六个DWORD，不误读为float。
  std::array<std::uint32_t, 2>
      extra_values;     ///< 原独立材质数组的两个DWORD，不按float解释。
  float initial_value;  ///< 原动画控制对象+28的初值。
  std::array<NativeWbFloatTrack, 6> tracks;  ///< 原六路标量动画。
};

/** @brief 原No Texture占位只消费名称，不消费材质附加字段。 */
struct NativeWbMaterial final {
  std::string texture;                       ///< 原贴图字节名；比较区分大小写。
  std::optional<NativeWbMaterialData> data;  ///< 精确No Texture时为空。
};

/** @brief 一条原WB网格实例记录；不包含另存于X资源的顶点、三角形或碰撞。 */
struct NativeWbMesh final {
  std::string name;                         ///< 原单字节编码名称。
  std::string properties;                   ///< 原对象属性字节。
  std::array<float, 3> parameters;          ///< 原对象三个标量。
  std::vector<std::uint32_t> subsets;       ///< 原启用子集编号，不重排。
  std::vector<NativeWbMaterial> materials;  ///< 与子集数相同的原顺序材质。
};

/** @brief 一条原dummy记录；位置和姿态保存六个精确float32。 */
struct NativeWbDummy final {
  std::string name;           ///< 原单字节编码名称；ASCII玩法标记可直接查找。
  std::string properties;     ///< 原属性文本字节。
  std::array<float, 6> pose;  ///< 前三项位置，后三项原姿态。
  std::size_t pose_offset;    ///< 当前WB内六个float的起始偏移。
};

/** @brief 当前WB中一条完整记录及其精确字节边界。 */
struct NativeWbRecord final {
  std::size_t offset;  ///< 名称长度字节的偏移。
  std::size_t end;     ///< 下一记录或文件结尾偏移。
  std::variant<NativeWbMesh, NativeWbDummy>
      value;  ///< 原kind0网格或kind1 dummy。
};

/** @brief 完整WB2记录文件；名称保留原字节，未执行系统代码页或图形资源加载。 */
struct NativeWbScene final {
  float header_value;  ///< 版本之后的原32位标量，保留资源含义边界。
  std::vector<NativeWbRecord> records;  ///< 原顺序完整记录，包括同名覆盖历史。

  /** @brief 精确字节名查找最后一条同名dummy，对齐原E907F0覆盖语义。 */
  [[nodiscard]] const NativeWbDummy* FindDummy(
      std::string_view name) const noexcept;
};

/**
 * @brief
 * 从头顺序读取完整WB2文件；拒绝未知kind、截断、非有限标量和资源容量超限。
 * @note
 * 抛protocol::ProtocolError；最大64MiB、131072条记录及总计1048576个关键帧。
 *       不扫描标记、解析属性XML、执行脚本或加载网格碰撞；返回值拥有全部记录。
 */
[[nodiscard]] NativeWbScene ReadNativeWb(std::span<const std::uint8_t> bytes);
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_WB_SCENE_H_
