#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_PLAZA_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_PLAZA_MESSAGES_H_

/**
 * @file plaza_messages.h
 * @brief 定义频道选择、广场进入/返回和场景移动消息的严格编解码接口。
 */

#include <cstdint>
#include <span>
#include <vector>

#include "application/identifier.h"

namespace bf::protocol {

/** @brief C2S53 选择客户端频道；60 为自由广场，80 为家族频道。 */
struct ChannelSelectionRequest final {
  std::uint32_t channel{0U};  ///< 客户端请求的频道编号。
};

/** @brief C2S105 从广场或房间返回主菜单；负载必须为空。 */
struct ReturnToMenuRequest final {};

/** @brief C2S106 从主菜单进入自由广场；负载必须为空。 */
struct EnterPlazaRequest final {};

/**
 * @brief C2S511 的 22 字节场景移动值。
 * @note 方向轴只允许 -1、0、1；坐标和朝向必须有限且绝对值不超过 1e7。
 */
struct SceneMovementRequest final {
  std::uint32_t sequence{0U};  ///< 客户端单调移动序号；服务端当前只透传。
  float x{0.0F};               ///< 世界 X 坐标。
  float y{0.0F};               ///< 世界 Y 坐标。
  float z{0.0F};               ///< 世界 Z 坐标。
  std::int8_t forward{0};      ///< 前后方向轴。
  std::int8_t sideways{0};     ///< 左右方向轴。
  float yaw{0.0F};             ///< 角色朝向。

  /** @brief 按全部已恢复移动字段比较。 */
  bool operator==(const SceneMovementRequest&) const = default;
};

/** @brief 严格解析 C2S53 的单个频道 DWORD。 */
[[nodiscard]] ChannelSelectionRequest DecodeChannelSelectionRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析空负载 C2S105。 */
[[nodiscard]] ReturnToMenuRequest DecodeReturnToMenuRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析空负载 C2S106。 */
[[nodiscard]] EnterPlazaRequest DecodeEnterPlazaRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析、校验 C2S511 的固定 22 字节移动值。 */
[[nodiscard]] SceneMovementRequest DecodeSceneMovementRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C13 的成功标志、频道编号和频道内编号三个 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeChannelSelectionResponse(
    bool success, std::uint32_t channel, std::uint32_t channel_id);

/**
 * @brief 编码 S2C514：认证玩家 DWORD 后接规范化的 22 字节移动值。
 * @note 玩家标识超过旧协议 uint32 范围时抛出
 * ProtocolError，不能截断冒充其他玩家。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSceneMovementResponse(
    domain::PlayerId player_id, const SceneMovementRequest& movement);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_PLAZA_MESSAGES_H_
