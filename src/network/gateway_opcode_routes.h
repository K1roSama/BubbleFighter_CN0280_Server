#ifndef BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_OPCODE_ROUTES_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_OPCODE_ROUTES_H_

/**
 * @file gateway_opcode_routes.h
 * @brief 定义主网关 opcode
 * 的执行通道分类：世界直投、需数据库快照的世界命令或数据库业务任务。
 */

#include <cstdint>

namespace bf::infrastructure::network {

/** @brief 一个已认证网关包应进入的执行通道。 */
enum class GatewayOpcodeRoute : std::uint8_t {
  kDatabaseTask,  ///< 在数据库工作池执行业务，或先加载快照再投递世界执行器。
  kWorldDirect,   ///< 不读数据库，直接从 I/O strand
                  ///< 投递世界执行器（战斗等高频包）。
};

/** @brief 是否由世界执行器处理的场景、房间、广场或战斗 opcode。 */
[[nodiscard]] bool IsWorldOpcode(std::uint16_t opcode) noexcept;

/** @brief 世界命令是否需要先在数据库线程重新读取完整玩家资料快照。 */
[[nodiscard]] bool WorldOpcodeRequiresMenuSnapshot(
    std::uint16_t opcode) noexcept;

/** @brief 世界命令是否需要先在数据库线程读取聊天屏蔽、处罚和家族权限快照。 */
[[nodiscard]] bool WorldOpcodeRequiresAccessSnapshot(
    std::uint16_t opcode) noexcept;

/**
 * @brief 网关在世界分支之前自行处理（或可能处理）的 opcode。
 * @note 必须与 `GatewaySessionHandler` 数据库任务中世界分支之前的判断保持一致；
 *       命中时始终走数据库通道，保证直投通道不会改变既有业务分支。
 */
[[nodiscard]] bool IsHandledBeforeWorldDispatch(std::uint16_t opcode) noexcept;

/** @brief 选择 opcode 的执行通道；未知或非世界 opcode 一律走数据库通道。 */
[[nodiscard]] GatewayOpcodeRoute ClassifyGatewayOpcode(
    std::uint16_t opcode) noexcept;

}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_OPCODE_ROUTES_H_
