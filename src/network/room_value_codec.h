#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_VALUE_CODEC_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_VALUE_CODEC_H_

/**
 * @file room_value_codec.h
 * @brief 定义内部房间值消息的结构化编码，不向客户端暴露 JSON 协议。
 */
#include <nlohmann/json.hpp>

#include "features/rooms/dedicated_room.h"

namespace bf::infrastructure::network {
/** @brief 解析有界 JSON；过深嵌套、重复键、非对象及不合法 UTF-8 会失败。 */
[[nodiscard]] nlohmann::json ParseRoomPayload(
    std::span<const std::uint8_t> payload);
/** @brief 从可信网关值消息读取完整资料快照，保留所有现有资产字段。 */
[[nodiscard]] application::PlayerMenuSnapshot ReadRoomPlayer(
    const nlohmann::json& value);
/** @brief 编码完整拥有型资料；可选字段与读取端使用同一契约。 */
[[nodiscard]] nlohmann::json WriteRoomPlayer(
    const application::PlayerMenuSnapshot& value);
/** @brief 从可信持久化回执读取战斗安装值，开始时刻使用本进程单调时钟。 */
[[nodiscard]] runtime::RoomMatchInstallation ReadRoomInstallation(
    const nlohmann::json& value);
/** @brief 编码已确认开局安装值，不包含任何本机单调时间。 */
[[nodiscard]] nlohmann::json WriteRoomInstallation(
    const runtime::RoomMatchInstallation& value);
/** @brief 读取数据库已提交的结算结果。 */
[[nodiscard]] application::MatchSettlementResult ReadRoomSettlement(
    const nlohmann::json& value);
/** @brief 编码已提交奖励快照，供 DS 按对局身份展示。 */
[[nodiscard]] nlohmann::json WriteRoomSettlement(
    const application::MatchSettlementResult& value);
/** @brief 读取终局命令并校验业务不变量，调用方另外校验实例归属。 */
[[nodiscard]] application::MatchSettlementCommand ReadRoomTerminal(
    const nlohmann::json& value);
/** @brief 读取原子消息批次和会话阶段，禁止跨进程传递计时器。 */
[[nodiscard]] runtime::RoomCoordinatorResult ReadRoomResult(
    const nlohmann::json& value);
/** @brief 将房间结果转成拥有型消息，不序列化进程内 timer 截止点。 */
[[nodiscard]] nlohmann::json WriteRoomResult(
    const runtime::RoomCoordinatorResult& result);
/** @brief 将错误转换成内部稳定值。 */
[[nodiscard]] nlohmann::json WriteRoomError(
    const application::ServiceError& error);
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_VALUE_CODEC_H_
