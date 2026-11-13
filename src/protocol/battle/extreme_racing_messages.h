#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_EXTREME_RACING_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_EXTREME_RACING_MESSAGES_H_

/**
 * @file extreme_racing_messages.h
 * @brief 定义极限竞速原生请求、计时、对象初始化和成绩补丁；不推测原服计分公式。
 */

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bf::protocol {

/** @brief
 * C2S32934：玩家对象、赛段、该赛段的检查点；身份和地图位置须由业务验证。 */
struct ExtremeRacingCheckpoint final {
  std::uint32_t actor_object{0U};  ///< 7C9A70 第一个 DWORD。
  std::uint32_t stage{0U};         ///< saveArea 编号 / 10。
  std::uint32_t checkpoint{0U};    ///< saveArea 编号 % 10，允许赛段起点 0。
};

/** @brief S2C761 的计时器选择值；与下一字段实际时长分开。 */
enum class ExtremeRacingTimer : std::uint32_t {
  kCountdown = 2500U,  ///< 赛段开始倒计时。
  kRetire = 20000U,    ///< 首位到达后的剩余比赛时间显示。
  kStage = 210000U,    ///< 本赛段比赛计时器。
};

/** @brief S2C764 的 UI 更新路径；不是赛段编号。 */
enum class ExtremeRacingUpdateKind : std::uint32_t {
  kLocal = 0U,        ///< 仅更新匹配本地席位的记录及当前分数。
  kRoundResult = 1U,  ///< 更新全部记录、重排并刷新队伍累计分。
  kStandings = 2U,    ///< 更新全部记录并重排。
};

/** @brief 472850/4728D0 对 PlayRecord
 * 的七个字段标签。两项分数组成未推断业务公式。 */
enum class ExtremeRacingField : std::uint8_t {
  kTotalPrimary = 0U,    ///< PlayRecord+72；累计分数的第一项。
  kRoundPrimary = 1U,    ///< PlayRecord+44；当前分数的第一项。
  kTotalSecondary = 2U,  ///< PlayRecord+76；累计分数的第二项。
  kRoundSecondary = 3U,  ///< PlayRecord+48；当前分数的第二项。
  kInactive = 4U,        ///< 值恰为 1 才置 PlayRecord+92；置位者排序靠后。
  kIndicator = 5U,       ///< PlayRecord+36；有符号 16 位图标选择值。
  kTieBreak = 6U,        ///< PlayRecord+64；累计同分时按有符号整数升序。
};

/** @brief 一项 BYTE 字段标签和有符号 DWORD 值。 */
struct ExtremeRacingFieldValue final {
  ExtremeRacingField field{
      ExtremeRacingField::kTotalPrimary};  ///< 已核实的字段。
  std::int32_t value{0};  ///< 原生字段值，不表示赛段耗时的统一单位。
};

/** @brief 一名玩家的 WORD 席位及七项以内的非重复字段补丁。 */
struct ExtremeRacingRecordUpdate final {
  std::uint16_t seat{0U};                       ///< 原生记录表查找键。
  std::vector<ExtremeRacingFieldValue> fields;  ///< 按给定顺序写入。
};

/** @brief S2C763 中金币类型表、赛段表的合并表示；键由连续对象 ID 隐式提供。 */
struct ExtremeRacingCoin final {
  std::uint32_t type{
      0U};  ///< 原生添加 1 后选择效果资源；资源有效性由业务验证。
  std::uint32_t stage{1U};  ///< 7E8870 要求等于当前赛段。
};

/** @brief 原生 InitGameData；业务须校验对象 ID 区间彼此及与房间其他对象无冲突。
 */
struct ExtremeRacingInitialization final {
  std::uint32_t cannon_base{0U};  ///< 每个地图炮台占用两个连续对象 ID。
  std::uint32_t cannon_count{
      0U};  ///< 原生读取但 5E98B0 不消费的 DWORD，保留调用者值。
  std::uint32_t coin_base{0U};           ///< 金币连续对象 ID 起点。
  std::vector<ExtremeRacingCoin> coins;  ///< 必须完整对应地图金币位置列表。
  std::uint32_t grenade_base{0U};        ///< 投石装置连续对象 ID 起点。
  std::uint32_t grenade_count{0U};       ///< 投石装置数量。
};

/** @brief S2C768：两组地板图案、投石随机种子及每组消失地板的路线索引。 */
struct ExtremeRacingHazards final {
  std::array<std::uint32_t, 2U>
      floor_patterns{};  ///< 521E60 使用的 0..9 图案编号。
  std::uint32_t grenade_seed{
      0U};  ///< 5CA6C0 写入投石管理器，允许全部 DWORD 位模式。
  std::vector<std::uint32_t>
      vanishing_routes;  ///< 数量及各索引上限必须另外匹配地图配置。
};

/** @brief S2C770：命令本地客户端按炮台位置及方向产生炮弹和 C2S32902 上报。 */
struct ExtremeRacingCannonFire final {
  std::uint32_t group{0U};  ///< 0 表示任意组，1/2 匹配炮台 DivideGroup。
  std::uint32_t cannon_object{0U};  ///< 51DF20 创建的炮台对象，不是出口对象。
  std::uint32_t weapon{
      0U};  ///< 网络为 DWORD，后续原生上报使用 WORD；业务须匹配原生武器表。
  float speed_multiplier{1.0F};  ///< 5C6410 经 XMM3 传递的浮点速度倍率。
};

/** @brief C2S32902 的 38 字节炮弹上报；仅解析不代表已有发射授权。 */
struct ExtremeRacingCannonProjectile final {
  std::uint32_t actor_object{0U};     ///< 产生炮弹的本地玩家对象。
  std::uint16_t weapon{0U};           ///< 7EE0B0 唯一的 WORD 字段。
  std::array<float, 3U> position{};   ///< 炮台坐标，须与已授权炮台比对。
  std::array<float, 3U> direction{};  ///< 炮台到出口的单位方向，须由业务比对。
  std::uint32_t cannon_object{0U};    ///< 炮台源对象。
  std::uint32_t sequence{0U};  ///< 原生每玩家计数器；重复和越序由业务拒绝。
};

/** @brief 严格读取 C2S754 的赛段 DWORD，不据此授予到达成绩。 */
[[nodiscard]] std::uint32_t DecodeExtremeRacingFinish(
    std::span<const std::uint8_t> body);
/** @brief 严格读取 C2S755 的金币对象 DWORD，不据此授予金币。 */
[[nodiscard]] std::uint32_t DecodeExtremeRacingCoin(
    std::span<const std::uint8_t> body);
/** @brief 严格读取 C2S32934 的三个 DWORD；拒绝零对象和地图不可能的赛段/检查点。
 */
[[nodiscard]] ExtremeRacingCheckpoint DecodeExtremeRacingCheckpoint(
    std::span<const std::uint8_t> body);
/** @brief 严格解析 C2S32902；拒绝截断、尾部、非法对象、零武器及非有限坐标。 */
[[nodiscard]] ExtremeRacingCannonProjectile DecodeExtremeRacingCannonProjectile(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C761；布尔字段在网络上占 DWORD，停止时仍保留给定时长。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingTimer(
    ExtremeRacingTimer timer, std::uint32_t duration_ms, bool active);
/** @brief 编码 S2C762；首 DWORD 的业务语义未确认，必须由调用者明确传入。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingStage(
    std::int32_t context, std::uint32_t stage);
/** @brief 编码 S2C763；生成两个键集合完全相同的 DWORD 字典。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingInitialization(
    const ExtremeRacingInitialization& initialization);
/** @brief 编码 S2C764；拒绝重复席位、重复字段及窄字段截断。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingRecords(
    ExtremeRacingUpdateKind kind,
    std::span<const ExtremeRacingRecordUpdate> updates);
/** @brief 编码 S2C768；不代替地图路线范围及投石装置完整性检查。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingHazards(
    const ExtremeRacingHazards& hazards);
/** @brief 编码 S2C770；不代替已创建炮台、武器配置、赛段与发射周期检查。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeExtremeRacingCannonFire(
    const ExtremeRacingCannonFire& fire);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_EXTREME_RACING_MESSAGES_H_
