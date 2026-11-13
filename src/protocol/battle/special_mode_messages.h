#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SPECIAL_MODE_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SPECIAL_MODE_MESSAGES_H_

/**
 * @file special_mode_messages.h
 * @brief 定义原版感染、僵尸 PVE 和队长模式已经确认的独立方向报文。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf::protocol {

/** @brief 7E5460/7E5580 的 C2S/peer32918；仅恢复了 ChrMole
 * 显隐，不是击杀或计分。 */
struct NativeMoleState final {
  std::uint32_t
      object;           ///< 已存在的 ChrMole 对象编号；不授予任何来源或所有权。
  std::uint32_t state;  ///< 已证实 0/12；完整枚举尚未恢复。
};

/** @brief 严格解码已证实的显隐子集；无可靠登记的 ChrMole
 * 来源时不得路由或泛转发。 */
[[nodiscard]] NativeMoleState DecodeObservedMoleState(
    std::span<const std::uint8_t> body);

/** @brief 7D3050/7F0C70 已证的 32892 wire32；没有可认证的完整对象 ID。 */
struct NativeMoleSpawn final {
  std::uint32_t spawner;
  std::uint32_t property;
  std::uint64_t opaque_8;  ///< producer 源自 DFC8F0
                           ///< double计时；保留位模式，绝不是身份cookie。
  std::uint32_t
      slot;  ///< 独立的原生槽位 0..63，不是人类席位或 ChrMole 对象号。
  std::array<float, 3U> position;  ///< wire+20..31，不能错读成 wire+8 的向量。
};

/** @brief 严格读取宽度、槽位和有限坐标；仅为只读解码，不授予 spawn 权限。 */
[[nodiscard]] NativeMoleSpawn DecodeObservedMoleSpawn(
    std::span<const std::uint8_t> body);

/** @brief 823C70 -> 7CD0B0 已证的 C2S737：kind1/2、variant6 和不完整来源索引。
 */
struct NativeMoleCoinDrop final {
  std::array<float, 3U> position;
  std::array<float, 3U> velocity;
  std::uint32_t kind;
  std::uint32_t variant;
  std::uint32_t
      source_index;  ///< 不是完整目标 ID、弹丸 cookie 或可验证的 hit 收据。
};

/** @brief 解码已证金币掉落子集；运行时仍缺 source/cookie/生命绑定，不能发钱。
 */
[[nodiscard]] NativeMoleCoinDrop DecodeObservedMoleCoinDrop(
    std::span<const std::uint8_t> body);
/** @brief 解码 selector_return2 的 C2S735
 * BOX；字段24/28均为1，不把该分支当金币。 */
[[nodiscard]] NativeMoleCoinDrop DecodeObservedMoleAlternateDrop(
    std::span<const std::uint8_t> body);

/** @brief 720 -> 7D4150 的已证 kind1/2 对象；最后 DWORD 仍是原始表现参数。 */
struct NativeMoleCoinSpawn final {
  std::uint32_t object;
  std::uint32_t kind;
  std::array<float, 3U> position;
  std::array<float, 3U> velocity;
  std::uint32_t visual_parameter;
};

/** @brief 只读解码 S2C720 的36字节；不得把对象存在推断成可拾取授权。 */
[[nodiscard]] NativeMoleCoinSpawn DecodeObservedMoleCoinSpawn(
    std::span<const std::uint8_t> body);
/** @brief 返回原生 kind1/2 的1/100金额；其他种类没有此证据。 */
[[nodiscard]] std::optional<std::uint32_t> ObservedMoleCoinAmount(
    std::uint32_t kind) noexcept;

/** @brief C2S736/S2C721 共用的对象对；必须另验本人和未消费金币账本。 */
struct NativeMoleCoinPickup final {
  std::uint32_t collector;
  std::uint32_t coin;
};

/** @brief 严格解码8字节，仅解析而不消费或登记任何对象。 */
[[nodiscard]] NativeMoleCoinPickup DecodeObservedMoleCoinPickup(
    std::span<const std::uint8_t> body);

/** @brief C2S2026 event10/13 只镜像已收币表现，不是第二个奖励来源。 */
struct NativeMoleCoinStat final {
  std::uint8_t event;
  std::uint32_t collector;
  std::uint8_t seat;
};

/** @brief 严格读取6字节，仅接受已证10/13；身份授权属于运行时，解码不计分。 */
[[nodiscard]] NativeMoleCoinStat DecodeObservedMoleCoinStat(
    std::span<const std::uint8_t> body);

/** @brief 客户端掉落请求 C2S729 (7ED7B0)，与 S2C729 计分板无关。 */
struct PveItemDrop final {
  std::array<float, 3U> position{};
  std::array<float, 3U> velocity{};
  std::uint32_t type{0U};
  std::uint8_t team{0U};
  std::uint32_t lifetime_ms{0U};
};

/** @brief Recovered PVE client reports and pursuit/drop requests. */
[[nodiscard]] bool IsPveEventOpcode(std::uint16_t opcode) noexcept;
/** @brief Check wire structure only; runtime rejects unmatched rules without
 * disconnecting. */
void ValidatePveEvent(std::uint16_t opcode, std::span<const std::uint8_t> body);
[[nodiscard]] PveItemDrop DecodePveItemDrop(std::span<const std::uint8_t> body);

/** @brief 原版 C2S725 的客户端终局预测；数值语义由模式决定，不能直接作为胜方。
 */
struct SpecialModeFinishClaim final {
  std::uint32_t value{
      0U};  ///< 7CA9E0 写入的一个 DWORD；已确认调用点只发送 0 或 1。
};

/** @brief 7EF6D0 按字段读取的 S2C982；布局不能使用内存结构体尺寸代替。 */
struct SpecialModeRespawn final {
  std::uint32_t seat{0U};  ///< 玩家席位，不是 Character 对象编号。
  std::uint32_t movement_state{
      0U};  ///< 虚函数 +632 -> 8B00F0 的动作/生命周期状态。
  std::uint32_t hp_bits{
      0U};  ///< +76 -> 81BE00 的有限 float HP 位模式；不授予血量权限。
  std::uint32_t field_12{0U};  ///< 写入 Character +3352；不擅自解释为生命值。
  std::array<float, 3U> position{};  ///< 角色同步位置。
  std::uint32_t next_object{0U};     ///< 本席位后续创建对象的计数器。
  std::uint32_t selected_item_slot{
      0U};  ///< 0/1立即索引道具槽并调用8375A0；其他值跳过选择。
  std::vector<std::uint32_t>
      item_slots;              ///< 最多两个ItemSpec道具编号；不是角色枪械背包。
  std::uint32_t field_48{0U};  ///< 解码器读取但该接收函数未使用。
  std::uint32_t field_52{0U};  ///< 解码器读取但该接收函数未使用。

  /** @brief 用于回放和编码往返的逐字段比较。 */
  bool operator==(const SpecialModeRespawn&) const = default;
};

/** @brief 7E6A40 的C2S2018：首字段是收件席位，后续counter和状态属于认证发送者。
 */
struct NativeObjectCheckpointRequest final {
  std::uint32_t
      recipient_seat;  ///< B44710传入新激活的远端席位；不是counter的owner。
  SpecialModeRespawn
      reported;  ///< seat取外部认证发送者；HP/生命/道具仍是声明而非授权。
};

/** @brief
 * 逐字段校验2018，使用认证sender校验号段；不可原样当982广播或重置counter。 */
[[nodiscard]] NativeObjectCheckpointRequest DecodeNativeObjectCheckpointRequest(
    std::span<const std::uint8_t> body,
    std::uint32_t authenticated_sender_seat);

/** @brief 判断这里恢复的 C2S725/726；515
 * 与房间控制复用，必须由调用者按阶段分派。 */
[[nodiscard]] bool IsSpecialModeClaimOpcode(std::uint16_t opcode) noexcept;
/** @brief 严格读取 C2S725，拒绝截断、尾随数据和未确认的预测值。 */
[[nodiscard]] SpecialModeFinishClaim DecodeSpecialModeFinishClaim(
    std::span<const std::uint8_t> body);
/** @brief 校验 C2S726 的空负载；它是目标状态通知，不等同于服务端确认胜利。 */
void ValidateSpecialModeObjectiveClaim(std::span<const std::uint8_t> body);
/** @brief 校验僵尸对局 C2S515 的空负载；只能消费服务端已批准的阵营转换。 */
void ValidateInfectionTransitionRequest(std::span<const std::uint8_t> body);
/** @brief 无副作用校验阶段外迟到的 C2S725/726。 */
void ValidateSpecialModeClaim(std::uint16_t opcode,
                              std::span<const std::uint8_t> body);
/** @brief 编码 S2C504 的 DWORD 席位和有符号 BYTE 阵营，限定已核实的感染阵营。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeInfectionTeamResponse(
    std::uint32_t mode, std::uint32_t seat, std::uint8_t team);
/** @brief 解码 S2C982 回放样本，并检查计数器、有限坐标与道具槽下标。 */
[[nodiscard]] SpecialModeRespawn DecodeSpecialModeRespawn(
    std::span<const std::uint8_t> body);
/** @brief 编码 S2C982，保留尚未确认含义的字段并拒绝会导致原版越界的组合。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeSpecialModeRespawn(
    const SpecialModeRespawn& response);
/** @brief 编码 S2C988 的队长技能席位；B3FA70 转为事件 315。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCaptainSkillResponse(
    std::uint32_t seat);
/** @brief 569370 发送 C2S2021 的 DWORD 席位；运行时还须校验本人和队长状态。 */
[[nodiscard]] std::uint32_t DecodeCaptainRestoreRequest(
    std::span<const std::uint8_t> body);
/** @brief 编码 S2C1000 的 DWORD 开关；B3FAC0 写入 CaptainModeMgr +7300。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCaptainOptionResponse(
    bool enabled);

/** @brief ItemSpecDefault 与 55DFA0/84FAF0 确认的队长临时形态。 */
struct CaptainForm final {
  std::uint32_t item;
  std::uint8_t team;
  std::uint32_t character;
  std::uint32_t
      weapon;  ///< 临时第4栏；704/705 使用代码常量7220，覆盖表内7211。
};

[[nodiscard]] std::optional<CaptainForm> GetCaptainForm(
    std::uint32_t item) noexcept;

/** @brief 7E09E0 的两个DWORD；此解码器仅用于队长变身分支。 */
struct CaptainTransformRequest final {
  std::uint32_t object;
  CaptainForm form;
};

[[nodiscard]] CaptainTransformRequest DecodeCaptainTransformRequest(
    std::span<const std::uint8_t> body);

/** @brief 55EF10 -> 7E9D00 的C2S710/27，不能按同号S2C710广播或直接加分。 */
struct CaptainDefeatReport final {
  std::uint32_t victim;
  std::uint32_t attacker;
  std::uint8_t victim_seat;
  std::uint8_t attacker_seat;
  std::uint8_t weapon_kind;
};

[[nodiscard]] CaptainDefeatReport DecodeCaptainDefeatReport(
    std::span<const std::uint8_t> body);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_SPECIAL_MODE_MESSAGES_H_
