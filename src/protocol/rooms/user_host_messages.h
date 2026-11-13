#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_USER_HOST_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_USER_HOST_MESSAGES_H_

/**
 * @file user_host_messages.h
 * @brief 定义原生玩家主办比赛的奖金设置、范围同步和开赛授权线格式。
 */

#include <cstdint>
#include <span>
#include <vector>

namespace bf::protocol {

/** @brief B4CB50 的设置动作；2/3 仅查询当前状态，不代表领取或退款。 */
enum class UserHostSettingAction : std::uint32_t {
  kBeginEditing = 0U,
  kSetAmount = 1U,
  kQueryAsGuest = 2U,
  kQueryAsHost = 3U,
};

/** @brief C2S2029 恰好两个 DWORD；非设置动作的第二个字段必须为零。 */
struct UserHostSettingRequest final {
  UserHostSettingAction action{
      UserHostSettingAction::kBeginEditing};  ///< 原生 0～3 动作。
  std::uint32_t amount{
      0U};  ///< 本次设置的金币总额；钱包与范围检查由业务层执行。
};

/** @brief S2C998 固定四 DWORD；全部动作均携带金额与服务器设置的上下限。 */
struct UserHostSettingResponse final {
  UserHostSettingAction action{
      UserHostSettingAction::kBeginEditing};  ///< 通知或查询种类。
  std::uint32_t amount{0U};   ///< 当前房间保存的奖金总额，零表示尚未设置。
  std::uint32_t minimum{0U};  ///< 金额下限；客户端仅在动作 2/3 时复制此值。
  std::uint32_t maximum{0U};  ///< 金额上限；不是原生客户端内置的常量。
};

/** @brief B85890 处理的 S2C997 有符号状态；-4 是缺券，-5 是奖金信息无效。 */
enum class UserHostStartStatus : std::int32_t {
  kAccepted = 0,
  kNotTeamMode = -1,
  kUnequalTeams = -2,
  kCannotStart = -3,
  kMissingTicket = -4,
  kInvalidPrize = -5,
};

/** @brief 严格解析 B4F4A0 的八字节请求，不对收到的金额作静默夹取。 */
[[nodiscard]] UserHostSettingRequest DecodeUserHostSettingRequest(
    std::span<const std::uint8_t> body);

/** @brief 校验 8A4FA0 的 C2S2028；此消息不携带队伍人数或客户端金额。 */
void ValidateUserHostStartRequest(std::span<const std::uint8_t> body);

/** @brief 按 B85F60 固定顺序编码完整 S2C998，不省略暂未使用的末两个 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeUserHostSettingResponse(
    const UserHostSettingResponse& response);

/** @brief 编码 S2C999 的两个 DWORD；范围必须非负、有序且适合原生有符号整数。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeUserHostDefaultSetting(
    std::uint32_t minimum, std::uint32_t maximum);

/** @brief 编码 S2C997 的单个补码 DWORD；此函数不表示业务已经授权开赛。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeUserHostStartResponse(
    UserHostStartStatus status);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_ROOMS_USER_HOST_MESSAGES_H_
