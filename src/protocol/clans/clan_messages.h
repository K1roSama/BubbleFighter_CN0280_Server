#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_

/**
 * @file clan_messages.h
 * @brief 定义 0.2.80.1 客户端 Nx 战队子连接的帧、字符串和响应布局。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "application/identifier.h"
#include "features/clans/clan_service.h"
#include "protocol/codec/packet_io.h"

namespace bf::protocol {

inline constexpr std::size_t kClanFrameHeaderBytes =
    4U;  ///< uint16 body size + uint16 opcode。
inline constexpr std::size_t kMaximumClanBodyBytes =
    65'531U;  ///< 保证整帧不超过 uint16。

/** @brief 一条已解密的 Nx 战队消息。 */
struct ClanPacket final {
  std::uint16_t opcode{0U};        ///< C2S/S2C 操作码。
  std::vector<std::uint8_t> body;  ///< 不含帧头的明文负载。
};

/**
 * @brief 就地应用客户端可逆 Nx 负载编码。
 * @note 密钥同时依赖 opcode 和负载长度；该历史编码不是密码学保护。
 */
void CryptClanPayload(std::uint16_t opcode,
                      std::span<std::uint8_t> body) noexcept;

/** @brief 将明文战队包编码为 `<size:u16, opcode:u16, encrypted-body>`。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanFrame(
    const ClanPacket& packet);

/** @brief 校验完整帧长度并解密；截断、尾随或超限时抛 ProtocolError。 */
[[nodiscard]] ClanPacket DecodeClanFrame(std::span<const std::uint8_t> frame);

/** @brief 顺序读取 Nx 小端字段与 NUL 结尾 UTF-16LE 文本。 */
class ClanPacketReader final {
 public:
  /** @brief 借用一段完整 Nx 消息体，并从首字节开始读取。 */
  explicit ClanPacketReader(std::span<const std::uint8_t> body) noexcept;
  /** @brief 读取一个无符号 8 位整数；截断时抛出 ProtocolError。 */
  [[nodiscard]] std::uint8_t ReadU8();
  /** @brief 按小端读取一个无符号 16 位整数；截断时抛出 ProtocolError。 */
  [[nodiscard]] std::uint16_t ReadU16();
  /** @brief 按小端读取一个无符号 32 位整数；截断时抛出 ProtocolError。 */
  [[nodiscard]] std::uint32_t ReadU32();
  /** @brief 按小端读取一个有符号 32 位整数；截断时抛出 ProtocolError。 */
  [[nodiscard]] std::int32_t ReadI32();
  /** @brief 读取原生 OLE 月首日期；拒绝非有限、小数天和非月首。 */
  [[nodiscard]] std::chrono::sys_days ReadMonth();
  /** @brief 借用后续 count 字节；范围越界时抛出 ProtocolError。 */
  [[nodiscard]] std::span<const std::uint8_t> ReadBytes(std::size_t count);
  /** @brief 读取最多 maximum_code_units 个 UTF-16 代码单元，并严格转为 UTF-8。
   */
  [[nodiscard]] std::string ReadText(std::size_t maximum_code_units);
  /** @brief 要求消息体恰好读完；存在未知尾字段时抛出 ProtocolError。 */
  void RequireEnd() const;

 private:
  PacketReader reader_;
};

/** @brief 构造 Nx 小端字段、NUL 结尾 UTF-16LE 文本和 OLE 日期。 */
class ClanPacketWriter final {
 public:
  /** @brief 追加一个无符号 8 位整数。 */
  void WriteU8(std::uint8_t value);
  /** @brief 按小端追加一个无符号 16 位整数。 */
  void WriteU16(std::uint16_t value);
  /** @brief 按小端追加一个无符号 32 位整数。 */
  void WriteU32(std::uint32_t value);
  /** @brief 按小端追加一个有符号 32 位整数。 */
  void WriteI32(std::int32_t value);
  /** @brief 按小端追加一个无符号 64 位整数。 */
  void WriteU64(std::uint64_t value);
  /** @brief 原样追加一段字节。 */
  void WriteBytes(std::span<const std::uint8_t> value);
  /** @brief 将严格 UTF-8 文本转为以 NUL 结尾的 UTF-16LE 字符串。 */
  void WriteText(std::string_view utf8);
  /** @brief 写入客户端直接交给 COleDateTime 的天数 double。 */
  void WriteDate(std::chrono::system_clock::time_point value);
  /** @brief 转移已经编码的消息体，并清空 writer 的所有权。 */
  [[nodiscard]] std::vector<std::uint8_t> Release() noexcept;

 private:
  PacketWriter writer_;
};

/** @brief 主网关 S2C23 的长度 UTF-16 昵称与 Nx C2S1005 单次票据布局。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeGatewayClanTicket(
    domain::PlayerId player_id, std::string_view nickname,
    std::span<const std::uint8_t> secret);

/** @brief 常规/扩展成员列表编码所需的场景快照。 */
struct ClanRosterWireEntry final {
  application::ClanMemberView member;  ///< 数据库权威成员。
  std::uint32_t scene{0U};             ///< 0 离线，在线默认 3。
};

/** @brief 编码战队主页、当前成员角色或待审批申请状态。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanHome(
    const application::ClanHomeState& state);
/** @brief 编码战队资料和用于客户端头像预览的当前成员列表。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanProfile(
    const std::optional<application::ClanSummary>& clan,
    std::span<const application::ClanMemberView> roster);
/** @brief 编码成员列表；extended 控制是否携带场景和扩展资料字段。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanRoster(
    std::span<const ClanRosterWireEntry> entries, bool extended);
/** @brief 编码搜索页、全部匹配数、页码和服务端确认后的搜索词。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanSearch(
    std::span<const application::ClanSummary> entries,
    std::uint32_t total_count, std::uint32_t page, std::string_view query);
/** @brief 编码仅管理角色可见的待审批玩家列表。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanApplicants(
    std::span<const application::ClanApplicantView> entries);
/** @brief 编码按稳定游标排序的战队留言板记录。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanPosts(
    std::span<const application::ClanPostView> entries);

/** @brief 编码1077成员或1079对手战绩的原生字段顺序。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanRecords(
    std::span<const application::ClanRecordEntry> entries, bool opponents);

/** @brief 编码1083最近比赛及点击详情；日期按中国墙上时间写入OLE。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanRecentMatches(
    std::span<const application::ClanRecentMatch> entries);

/** @brief 编码1074活动周期和积分；仅有显示证据的两个积分字段取真实数据。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanActivity(
    const application::ClanActivityView& value);
/** @brief 编码1085/1087榜单，或1089/1091完整本队排名。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanRankings(
    std::span<const application::ClanRankingEntry> entries, bool single);

/** @brief 主连接S2C25：未参与UI展示的身份、贡献、原因、活动图标状态。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeClanPointsNotice(
    std::uint32_t clan_id, std::uint32_t points, std::uint32_t reason,
    bool event_icon);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_CLANS_CLAN_MESSAGES_H_
