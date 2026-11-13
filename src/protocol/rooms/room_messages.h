#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_MESSAGES_H_

/**
 * @file room_messages.h
 * @brief 定义等待房间控制请求与增量广播的严格编解码接口。
 */

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "protocol/scenes/scene_messages.h"

namespace bf::protocol {

/** @brief 校验已知房间控制请求，返回是否属于该集合；阶段竞态可在验证后忽略。 */
[[nodiscard]] bool ValidateRoomControlMessage(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief C2S141 本地生存排队请求；旧服务仅恢复示范公寓地图。 */
struct SurvivalQueueRequest final {
  bool cancel{false};      ///< 取消时 mode 必须为零且地图为空。
  std::uint32_t mode{0U};  ///< 客户端请求模式。
  std::u16string map_key;  ///< 有界原生地图资源键。
};

/** @brief 严格解析 C2S141 的动作、模式和 UTF-16 地图键。 */
[[nodiscard]] SurvivalQueueRequest DecodeSurvivalQueueRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C1011；状态 0/1/2 带细节 DWORD，取消状态 4 不带。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSurvivalQueueResponse(
    std::uint32_t status, std::uint32_t value = 0U);

/** @brief C2S404 离开当前房间请求；原版负载必须为空。 */
struct LeaveRoomRequest final {};

/** @brief C2S505 玩家等待房间状态。 */
struct RoomStatusRequest final {
  bool ready{false};      ///< READY/CANCEL 按钮状态；房主由领域层强制为 false。
  bool shop_open{false};  ///< 客户端是否打开房间商店界面。
  bool inventory_open{false};  ///< 客户端是否打开房间背包界面。
};

/** @brief C2S508 主动移交房主；原版 B4EC40 只写目标昵称 UTF-16 字符串。 */
[[nodiscard]] std::u16string DecodeTransferHostRequest(
    std::span<const std::uint8_t> body);

/** @brief C2S509 房主设置空席位锁的请求。 */
struct SeatLockRequest final {
  std::uint32_t slot{0U};  ///< 线协议 DWORD 席位；领域层按房间容量再次校验。
  bool locked{false};      ///< true 锁定，false 解锁；只接受 0/1 DWORD。
};

/** @brief C2S515 切换红蓝队请求；原版负载必须为空。 */
struct SwitchTeamRequest final {};

/** @brief C2S517 请求房间成员响应一次延迟探测。 */
struct RoomLatencyProbeRequest final {
  std::uint32_t slot{0U};  ///< 发起者本地席位，同时作为本次探测 token。
};

/**
 * @brief C2S/S2C32840 战斗 RTT 探测；两个 little-endian DWORD 必须原样往返。
 * @note build 752 的 96C410 发送，7DF920/964ED0 校验席位和 timeGetTime token，
 *       收到匹配回包后约 2500ms 再次探测；不能静默吞包或改写客户端时钟。
 */
struct MatchLatencyProbe final {
  std::uint32_t slot{0U};      ///< 房间零基席位，由运行时核对已认证玩家身份。
  std::uint32_t token_ms{0U};  ///< 客户端 32 位毫秒时钟原值，允许高位及回绕。
};

/** @brief 严格解析 C2S32840 的两个 DWORD，拒绝截断和尾随字段。 */
[[nodiscard]] MatchLatencyProbe DecodeMatchLatencyProbe(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C32840；仅向席位已验证的请求者回显，不广播到其他成员。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMatchLatencyProbe(
    const MatchLatencyProbe& probe);

/** @brief C2S518 对某次房间延迟探测的成员报告。 */
struct RoomLatencyReportRequest final {
  std::uint32_t slot{0U};   ///< 报告者席位。
  std::uint32_t token{0U};  ///< 原 C2S517 发起者席位 token。
  std::uint32_t fps{0U};    ///< 客户端报告帧率原值。
  std::uint32_t stage{0U};  ///< 客户端报告阶段原值。
};

/**
 * @brief C2S2019 原版客户端在房间内周期发送的单 DWORD 上下文信号。
 * @details 当前样本出现过 1 和 4；客户端发送函数只证明它是一个
 * DWORD，具体业务语义尚未
 *          恢复。服务端仅用该类型做长度校验并静默兼容，不据此改变房间或对局状态。
 */
struct RoomContextSignalRequest final {
  std::uint32_t value{0U};  ///< 原始上下文值；语义未恢复，必须保持不解释。
};

/**
 * @brief C2S952 原版客户端进入战斗场景后上报的资源场景名。
 * @details 历史抓包确认负载是长度前缀 UTF-16LE，例如
 * `rel_Village01_Byway`；旧服务端
 *          静默接收且不回包。该通知不代表服务端加载完成、开局或结算，不得据此推进权威
 *          房间状态；保留文本只用于完成严格线格式校验。
 */
struct SceneResourceReportRequest final {
  std::u16string
      resource_name;  ///< 客户端当前资源场景名；不写日志、不作为权威地图选择。
};

/** @brief S2C504 单个席位的队伍变更。 */
struct RoomTeamChangedResponse final {
  std::uint32_t slot{0U};  ///< 变更的零基席位。
  std::uint8_t team{1U};   ///< 团队1/2；13/23个人模式使用seat+1（1..16）。
};

/** @brief S2C512 单个空席位的锁状态变更。 */
struct RoomSeatLockChangedResponse final {
  std::uint32_t slot{0U};  ///< 变更的零基席位。
  bool locked{false};      ///< true 锁定，false 解锁。
};

/** @brief S2C518 交付给探测发起者的成员延迟显示数据。 */
struct RoomLatencyReportResponse final {
  std::uint32_t slot{0U};   ///< 响应成员席位。
  std::uint32_t fps{0U};    ///< 响应成员报告帧率。
  std::uint32_t stage{0U};  ///< 响应成员报告阶段。
};

/** @brief 严格解析空负载 C2S404。 */
[[nodiscard]] LeaveRoomRequest DecodeLeaveRoomRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S505 的三个 DWORD 布尔值，拒绝其他数值和尾随字节。 */
[[nodiscard]] RoomStatusRequest DecodeRoomStatusRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S509 的席位和 0/1 锁定 DWORD。 */
[[nodiscard]] SeatLockRequest DecodeSeatLockRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析空负载 C2S515。 */
[[nodiscard]] SwitchTeamRequest DecodeSwitchTeamRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S517 的单个本地席位 DWORD。 */
[[nodiscard]] RoomLatencyProbeRequest DecodeRoomLatencyProbeRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S518 的席位、token、帧率和阶段四个 DWORD。 */
[[nodiscard]] RoomLatencyReportRequest DecodeRoomLatencyReportRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S2019 的单个 DWORD，并拒绝截断或尾随字段。 */
[[nodiscard]] RoomContextSignalRequest DecodeRoomContextSignalRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief 严格解析 C2S952 的长度前缀 UTF-16LE 场景名。
 * @note 名称上限是 128 个 UTF-16 代码单元；拒绝截断、非法代理项和尾随字节。
 */
[[nodiscard]] SceneResourceReportRequest DecodeSceneResourceReportRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C102 建房结果；当前兼容约定 0 成功、非零失败。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCreateRoomResponse(
    std::uint32_t result);

/** @brief 编码 S2C103 加房结果；当前兼容约定 0 成功、非零失败。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeJoinRoomResponse(
    std::uint32_t result);

/** @brief 编码 S2C119 空负载，通知客户端清理当前房间对象。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLeaveRoomResponse();

/** @brief 编码 S2C501 完整 SceneInfo 更新。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomSceneUpdateResponse(
    const SceneInfo& scene);

/** @brief 编码 S2C502 当前房主席位。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomHostChangedResponse(
    std::uint32_t slot);

/** @brief 编码 S2C504 席位 DWORD 与单字节队伍。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomTeamChangedResponse(
    const RoomTeamChangedResponse& response);

/** @brief 编码 S2C512 席位和锁定 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomSeatLockChangedResponse(
    const RoomSeatLockChangedResponse& response);

/** @brief 编码 S2C517 的发起席位 token，供每个房间成员生成 C2S518。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomLatencyProbeResponse(
    std::uint32_t token);

/** @brief 编码 S2C518 的响应席位、帧率和阶段。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomLatencyReportResponse(
    const RoomLatencyReportResponse& response);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_ROOM_MESSAGES_H_
