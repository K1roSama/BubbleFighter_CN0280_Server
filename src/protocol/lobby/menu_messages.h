#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_MENU_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_MENU_MESSAGES_H_

/**
 * @file menu_messages.h
 * @brief 定义登录后单人队伍与大厅菜单场景的已验证响应编码。
 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "features/lobby/player_menu_snapshot.h"

namespace bf::protocol {

/** @brief 原S2C924的count +
 * (stage_key,count_metric,seconds_metric)；未知击杀数固定零。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSinglePlayClearsResponse(
    std::span<const application::SinglePlayClearView> records);

/**
 * @brief C2S2004 原版客户端在大厅界面切换时发送的单字节上下文标志。
 * @details 当前 EXE 的唯一发送函数固定写入一个字节，五个调用路径只传入 0 或
 * 1，且 发送后不等待响应。字段业务语义尚未恢复，服务端只做严格线格式兼容，
 *          不得据此改变玩家、广场或房间的权威状态。
 */
struct WorldContextFlagRequest final {
  bool value{false};  ///< 客户端内部上下文标志；已确认线格式取值仅为 0 或 1。
};

/**
 * @brief 严格解析 C2S2004 的单字节 0/1 标志。
 * @note 拒绝空载荷、尾随字段和未在当前客户端调用路径中出现的数值。
 */
[[nodiscard]] WorldContextFlagRequest DecodeWorldContextFlagRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief C2S2009 原版客户端在大厅或广场发送的单 DWORD 世界上下文信号。
 * @details 当前 EXE 的唯一发送函数写入一个 DWORD，两个调用路径只传入 0 或
 * 1；历史抓包
 *          也确认该消息不要求服务端响应。字段业务语义尚未恢复，服务端只做严格兼容，
 *          不得据此改变玩家、广场或房间状态。
 */
struct WorldContextSignalRequest final {
  std::uint32_t mode{0U};  ///< 客户端内部上下文模式；已确认取值仅为 0 或 1。
};

/**
 * @brief 严格解析 C2S2009 的单 DWORD 0/1 模式。
 * @note 拒绝截断、尾随字段和未在当前客户端发送路径中出现的数值。
 */
[[nodiscard]] WorldContextSignalRequest DecodeWorldContextSignalRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief 编码 S2C201 单人队伍状态。
 * @note 原版客户端会无条件读取首位队员，不能以空 vector 代替尚未组队状态。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSoloPartyResponse(
    const application::PlayerMenuSnapshot& snapshot);

/**
 * @brief 编码 S2C112 scene_type=1 的本地大厅菜单 GameSession。
 * @note 未恢复的 SceneConfig
 * 字段保持已验证零值；本函数不编码房间或自由广场场景。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMenuStateResponse(
    const application::PlayerMenuSnapshot& snapshot);

/** @brief 校验当前原生客户端的 2006/2024 旁路通知；没有已恢复的业务响应。 */
void ValidateNativeAuxiliaryReport(std::uint16_t opcode,
                                   std::span<const std::uint8_t> body);

/**
 * @brief S2C152 `gspRepEventZoneConfig`（B81860 -> 46F4F0，44 字节）。
 * @note 日期为 OLE DATE（1899-12-30 起的天数）；`day` 早于客户端当天时 5C5670
 * 会重新发送 C2S140。 `open` 为 0 时 5C5310 返回 3，客户端显示“이벤트존 게임
 * 점검 중 입니다.”。
 */
struct EventZoneConfig final {
  bool open{false};
  std::uint32_t wait_ms{
      3000U};  ///< +8：500480 等待框的首段计时，客户端缺省 3000。
  std::uint32_t reserved{0U};     ///< +12：已恢复消费者未读取。
  double day{0.0};                ///< +16：配置所属日期。
  double start{0.0};              ///< +28：开放期起点（5C5430）。
  double end{0.0};                ///< +40：开放期终点。
  std::uint32_t daily_open{0U};   ///< +52：每日开放 HHMM（5C5510 -> 46F330）。
  std::uint32_t daily_close{0U};  ///< +56：每日关闭 HHMM。
};

[[nodiscard]] std::vector<std::uint8_t> EncodeEventZoneConfig(
    const EventZoneConfig& config);

/** @brief S2C126 `gspRepAwardInfo` 中的一枚勋章（B95320 读取
 * sMedalInfo：编号、数量）。 */
struct MedalCount final {
  std::uint32_t medal_id{0U};
  std::uint32_t count{0U};
};

/**
 * @brief S2C126：`u32 目标玩家, u32 结果, u32 数量, 数量 × {u32 勋章, u32
 * 数量}`（B953B0； C2S120 查看他人时同样以目标玩家开头，见
 * encode_fighter_title_failure）。
 * @note 结果 1 时 B78640
 * 刷新勋章表并打开/刷新勋章窗口（9B9BD0）；数量增加的勋章进入新获得列表。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeAwardInfo(
    std::uint32_t target, std::uint32_t result,
    std::span<const MedalCount> medals);

/**
 * @brief S2C935：`i32 结果, u32 称号/物品, wstring`；失败也必须保留全部字段。
 * @note C2S931 才是领取请求，C2S935 是独立的排名查询。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeTitleClaimResponse(
    std::uint32_t title, std::int32_t result, std::u16string_view text = {});

/** @brief S2C816 `gspRepExchangeBubbleCash`（B732C0）：`u32 成功, u32 金额`。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeBubbleCashExchangeResponse(
    bool success, std::uint32_t amount);

/** @brief C2S140 为空包（5C5380）。 */
void ValidateEventZoneConfigRequest(std::span<const std::uint8_t> body);

/**
 * @brief C2S115 原活动区申请（6537A0 -> 6540B0 -> 654420）。
 * @note 两个缓存 DWORD 和 tick 仅保留原线格式，不作为身份、队列或资产授权来源。
 */
struct EventZoneRequest final {
  std::uint32_t mode{0U};
  std::uint32_t cached_field_4{0U};
  std::uint32_t cached_field_8{0U};
  std::uint32_t client_tick{0U};
  std::uint8_t selection_byte{
      0U};  ///< 654420 从原结构 byte+16 零扩展为 DWORD。
  std::u16string map_key;
};

[[nodiscard]] EventZoneRequest DecodeEventZoneRequest(
    std::span<const std::uint8_t> body);

/** @brief C2S116 原动作：0 请求选场，1 取消（653770/6538B0 -> 654000）。 */
[[nodiscard]] bool DecodeEventZoneCancelRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief 编码完整 S2C976 关闭/取消终态，不生成匹配成功或等待状态。
 * @note B908A0 无条件读取五 DWORD、wstring 和末 DWORD；B836D0 的 22 为维护、1
 * 为取消。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeEventZoneUnavailableResponse(
    bool cancel);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_MENU_MESSAGES_H_
