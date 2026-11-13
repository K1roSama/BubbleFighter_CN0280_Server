#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_SCENE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_SCENE_H_

/** @file native_x_scene.h @brief 原X对象层级、引用及有界数值几何读取。 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf::runtime {
/** @brief 原X节点；名称保持原字节，数值不含原DLL进程内指针。 */
struct NativeXNode final {
  std::string type_name;                   ///< 原模板名称。
  std::string type_guid;                   ///< 原模板GUID的规范小写文本。
  std::string name;                        ///< 原节点字节名称。
  std::optional<std::string> object_guid;  ///< 原可选对象GUID。
  std::optional<std::size_t> parent;       ///< 原顺序父节点，根节点为空。
  bool reference{false};                   ///< 原对象引用，不重复展开子节点。
  std::optional<std::size_t>
      reference_target;  ///< 原已发布根对象中首个匹配定义节点。
  std::optional<std::vector<std::uint8_t>>
      data;  ///< 与原Lock相同的数值布局；引用和字符串模板为空。
  std::vector<std::string>
      source_literals;  ///< 原字符串内容；文本转义保持原样，不解释路径。
};

/** @brief 原DLL在引用出现时省略的声明；仅作诊断，不补接后续定义。 */
struct NativeXUnresolvedReference final {
  std::size_t parent;               ///< 当前声明的父节点。
  std::string name;                 ///< 原引用名称，GUID引用时为空。
  std::optional<std::string> guid;  ///< 原可选非零引用GUID。
};

/** @brief 拥有全部原顺序节点；保留帧矩阵和动画，不执行世界坐标变换。 */
struct NativeXScene final {
  std::vector<NativeXNode> nodes;  ///< 原定义及已解析引用的先序层级。
  std::vector<NativeXUnresolvedReference>
      unresolved_references;  ///< 原省略声明的出现顺序。

  /** @brief 精确名称的唯一非引用Mesh；不存在或同类型重名时为空。 */
  [[nodiscard]] const NativeXNode* FindUniqueMesh(
      std::string_view name) const noexcept;
};

/** @brief 从已读取Mesh数值中复制的原顶点和多边形；未三角化或变换。 */
struct NativeXMesh final {
  std::vector<std::array<float, 3>>
      vertices;  ///< 原文件空间的精确float32顶点。
  std::vector<std::vector<std::uint32_t>> faces;  ///< 原面顺序及顶点索引。
};

/**
 * @brief 顺序解析原X头、MSZIP、对象、引用及已支持模板的全部数值。
 * @note 128MiB输入／数值预算、300000节点及声明、128层；浮点数组最多10000000项，
 *       各数组另按剩余输入及总数值预算校验。
 *       支持当前原地图的Header、Mesh及材质／法线／UV／颜色、Frame、FVFData与动画模板。
 *       引用只查找此前已完成的根对象及其子树；未解析声明保留诊断但不补接。
 *       拒绝未知对象模板、截断、非法索引、非有限标量和容量超限。
 *       不加载D3D、纹理、脚本、碰撞或导航；返回值拥有全部内容。
 * @throws protocol::ProtocolError 格式、内容或容量不合法。
 */
[[nodiscard]] NativeXScene ReadNativeX(std::span<const std::uint8_t> bytes);

/** @brief
 * 按节点读取Mesh顶点及面；允许引用Mesh定义，错误类型或内容抛ProtocolError。 */
[[nodiscard]] NativeXMesh GetNativeXMesh(const NativeXScene& scene,
                                         std::size_t node);
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_NATIVE_X_SCENE_H_
