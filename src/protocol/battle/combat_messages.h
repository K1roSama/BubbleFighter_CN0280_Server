#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_COMBAT_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_COMBAT_MESSAGES_H_

/**
 * @file combat_messages.h
 * @brief 定义现有客户端角色动作、压缩开火和命中报告的严格字段视图。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf::protocol {

/** @brief 暴风创建、本人受击效果与本人到期清理的原生字段；cookie 不是
 * GameObject ID。 */
struct NativeStormRequest final {
  std::uint32_t actor_object_id{0U};
  std::uint16_t weapon{0U};
  std::uint8_t team{0U};
  std::uint32_t target{0xffffffffU};
  std::uint32_t type{0U};
  std::uint32_t cookie{0U};
};

/** @brief 判断 32899/32910/32911/32916 暴风专用消息。 */
[[nodiscard]] bool IsNativeStormOpcode(std::uint16_t opcode) noexcept;
/** @brief 两种创建均为 19 字节，但末两个 DWORD 的含义不同。 */
[[nodiscard]] NativeStormRequest DecodeNativeStorm(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 已恢复布局的原生动作及仅允许忽略的不透明报告种类。 */
enum class NativeActionKind : std::uint8_t {
  kTransform,
  kActionFlag,
  kOpaqueTenBytes,
  kJump,
  kStopShoot,
  kHoldGrenade,
  kWallAction,
  kWallPose,
  kUnrecoveredObjectPair,
  kOpaqueReport,
  kOpaqueOwnedReport,
  kBubbleAction,
  kWeaponSwitch,
  kOpaqueEightBytes,
  kInfectionEffect,
  kOpaqueFourBytes,
  kCompressedTransform,
  kCompressedFire,
  kHitReaction,
  kCharacterControl,
  kEffectEnd,
  kSlip,
  kHumiliation,
  kItemEffect,
  kTextEmote,
  kAreaHitReaction,
  kHealthReport,
  kRespawnDurationReport,
  kSurvivalAlliancePose,
  kSurvivalTreatment,
  kSurvivalMelee,
  kSurvivalTrap,
  kAreaCast,
  kExplicitFire,
  kSceneObjectControl,
};

/** @brief 32898的显式模板开火；位置不是玩家移动，cookie不是场景对象号。 */
struct NativeExplicitFire final {
  std::uint16_t weapon{0U};
  std::int8_t variant{0};
  std::array<float, 3U> position{};
  std::array<float, 3U> direction{};
  std::uint32_t target{0xffffffffU};
  std::uint8_t flags{0U};
  std::uint32_t cookie{0U};
  std::uint32_t token{0U};
};

/** @brief 32930的效果种类和投掷物当前位置；不含投掷物编号，也不更新角色位置。
 */
struct NativeAreaCast final {
  std::uint32_t type{0U};
  std::array<float, 3U> position{};
};

/** @brief 生存近战握手；请求携带攻击者位置，确认携带受害者归一化朝向。 */
struct NativeSurvivalMelee final {
  std::uint32_t attacker{0U};
  std::uint32_t victim{0U};
  std::array<float, 3U> vector{};
  bool confirmation{false};
};

/** @brief Area32 的效果编号和阶段；不是客户端指定的治疗量。 */
struct NativeSurvivalTreatment final {
  std::uint16_t effect_id{0U};
  std::uint8_t stage{0U};
  std::vector<std::uint16_t> expired_effects;
};

/** @brief C2S32810 中由客户端选择的装备模板和武器栏。 */
struct NativeWeaponSwitch final {
  std::uint32_t template_id{0U};  ///< 客户端原生武器模板，不是物品 item_id。
  std::uint32_t bar_index{0U};    ///< 已恢复范围 0～4。
  bool select{false};             ///< 1 表示切换当前栏，0 只同步动作。
};

/** @brief C2S32925 中用于去重和弹丸登记的已知字段。 */
struct NativeCompressedFire final {
  std::uint8_t bar_index{0U};  ///< 发射使用的装备栏。
  std::uint32_t serial{0U};    ///< +22 原生弹丸 cookie，也是命中报告索引。
  std::uint32_t token{0U};     ///< +26 时序令牌；去重只使用低 24 位。
};

/** @brief 原生踢爆动画、攻击者请求与受害者确认的已验证字段。 */
struct NativeBubbleAction final {
  std::uint32_t attacker{0U};  ///< 攻击者对象；动画结束消息只有此字段。
  std::uint32_t victim{0U};    ///< 泡封受害者对象。
  std::uint32_t variant{0U};   ///< 动画参数原值，不推测布尔语义。
  bool success{false};         ///< 仅 32795 成功确认允许计分。
  bool shot_burst{
      false};  ///< 32795 的末 DWORD；823C70 射爆路径为 1，无踢击前置包。
};

/** @brief 一条原生角色动作的认证对象及已知可校验字段。 */
struct NativeActionRequest final {
  std::optional<NativeExplicitFire> explicit_fire;
  std::optional<std::uint32_t> scene_value;  ///< 32895状态或32778相关场景对象。
  std::optional<NativeAreaCast> area_cast;
  std::optional<NativeSurvivalTreatment> survival_treatment;
  std::optional<NativeSurvivalMelee> survival_melee;
  NativeActionKind kind{
      NativeActionKind::kStopShoot};  ///< opcode 映射后的稳定种类。
  std::uint32_t actor_object_id{
      0U};  ///< Character 对象编号；不透明报告不使用。
  std::optional<std::array<float, 3U>> position;    ///< 两种移动消息中的 XYZ。
  std::optional<NativeWeaponSwitch> weapon_switch;  ///< 仅 C2S32810 存在。
  std::optional<NativeCompressedFire> compressed_fire;  ///< 仅 C2S32925 存在。
  std::optional<NativeBubbleAction> bubble_action;      ///< 仅踢爆动作链存在。
  std::optional<std::uint32_t>
      reaction_source;  ///< 32788 受击动画的来源角色，允许世界哨兵。
  std::optional<std::uint32_t>
      consumed_object;       ///< 32833 滑倒时删除的陷阱对象，-1 表示环境。
  bool slip_bubbled{false};  ///< 32833 原生泡封滑倒分支。
  std::optional<std::uint32_t>
      humiliation_target;  ///< 32815/32816 羞辱目标，非击爆结果。
  std::optional<float>
      reported_health;  ///< 32933 本人生命显示值，不作为胜负或结算依据。
  std::optional<std::uint32_t>
      respawn_duration_ms;  ///< 32905 本次死亡的初始时长，不是剩余时间。
};

/** @brief 原生近战/范围伤害对象的创建、手雷爆炸、命中和销毁种类。 */
enum class NativeWeaponObjectKind : std::uint8_t {
  kMelee,
  kBlast,
  kDetonate,
  kHit,
  kDestroy,
  kAttachGrenade,
  kDestroyProjectile
};

/**
 * @brief C2S33281/32772/32900/33282/32907 的已恢复伤害对象字段。
 * @note 依据
 * local_server/research/native_weapons_results_2026-09-15.md；攻击参数是有符号
 *       字节原值，不是武器栏。向量只作有限性检查；原始负载由运行时验证后转发。
 */
struct NativeWeaponObjectRequest final {
  NativeWeaponObjectKind kind{
      NativeWeaponObjectKind::kMelee};  ///< 消息动作类别。
  std::uint32_t actor_object_id{0U};    ///< 直接创建时的所属角色。
  std::uint16_t template_id{0U};        ///< 创建对象所用原生装备模板。
  std::int8_t attack_variant{0};        ///< 客户端攻击参数，保持原值。
  std::uint32_t cookie{0U};             ///< 伤害对象编号，区别于源弹丸编号。
  std::uint32_t source_cookie{0U};      ///< 手雷爆炸时消耗的原生弹丸编号。
  std::uint32_t target_object_id{0U};   ///< 命中的同场角色编号。
};

/** @brief 判断是否为已恢复的原生伤害对象消息。 */
[[nodiscard]] bool IsNativeWeaponObjectOpcode(std::uint16_t opcode) noexcept;

/** @brief 严格解析伤害对象消息，拒绝错误长度及非有限/越界向量。 */
[[nodiscard]] NativeWeaponObjectRequest DecodeNativeWeaponObject(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief C2S32776/32924 的弹丸命中字段。 */
struct NativeHitRequest final {
  std::uint32_t projectile_cookie{0U};  ///< 必须引用当前比赛内已接受的开火。
  std::uint32_t target_object_id{
      0U};                           ///< 玩家对象、0 世界或 UINT32_MAX 无目标。
  std::array<float, 3U> position{};  ///< 未压缩 float 或压缩 half 的命中位置。
  std::uint8_t kind{0U};             ///< 保留的原生命中类型字节。
  bool keep_projectile{false};       ///< 是否允许同一弹丸继续命中不同目标。
};

/** @brief 已恢复的受击者自报泡封、击爆、救援及死亡动画种类。 */
enum class NativeLifecycleKind : std::uint8_t {
  kDefeat,
  kBubbleSettled,
  kRecovery,
  kProtection,
  kDeathAnimationStarted,
  kDeathAnimationFinished,
};

/** @brief 一条受击者自有生命周期报告中的可认证字段。 */
struct NativeLifecycleRequest final {
  NativeLifecycleKind kind{
      NativeLifecycleKind::kProtection};  ///< opcode 映射后的稳定种类。
  std::uint32_t actor_object_id{0U};      ///< 必须属于发送 Session。
  std::optional<std::uint32_t>
      related_object_id;      ///< 攻击者或救援者；世界值允许 0/-1。
  bool direct_defeat{false};  ///< C2S32787 的 pang 字段；false 表示进入泡封。
};

/** @brief C2S704 或 C2S32926 的复活请求/完成报告。 */
struct NativeRespawnRequest final {
  std::uint32_t actor_object_id{0U};  ///< 必须属于发送 Session。
  std::uint32_t respawn_point{0U};    ///< 两阶段必须完全相同的原生出生点标识。
  bool flag{false};        ///< 已验证布尔；语义尚未恢复，不据此判定结果。
  bool completion{false};  ///< false 为请求，true 为完成报告。
};

/** @brief 判断 opcode 是否属于当前已恢复的角色动作集合。 */
[[nodiscard]] bool IsNativeActionOpcode(std::uint16_t opcode) noexcept;

/** @brief 判断 opcode 是否是已恢复的压缩或未压缩命中消息。 */
[[nodiscard]] bool IsNativeHitOpcode(std::uint16_t opcode) noexcept;

/** @brief 判断 opcode 是否是已恢复的受击者生命周期报告。 */
[[nodiscard]] bool IsNativeLifecycleOpcode(std::uint16_t opcode) noexcept;

/** @brief 判断 opcode 是否是普通模式复活请求或完成报告。 */
[[nodiscard]] bool IsNativeRespawnOpcode(std::uint16_t opcode) noexcept;

/**
 * @brief 严格校验一条已恢复的原生战斗消息，但不应用对象所有权或战斗状态变化。
 * @details 用于 Loading、Countdown、Results
 * 或刚返回广场时丢弃客户端竞态产生的早到/迟到 消息；未知 opcode
 * 或字段非法时仍抛出 ProtocolError，不能把任意数据当成兼容空操作。
 */
void ValidateNativeCombatMessage(std::uint16_t opcode,
                                 std::span<const std::uint8_t> body);

/**
 * @brief 严格解析固定长度角色动作并校验有限浮点、布尔和栏位范围。
 * @note 不校验对象所有权或装备内容；它们属于房间运行时的认证状态。
 */
[[nodiscard]] NativeActionRequest DecodeNativeAction(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 严格解析命中字段；长度、half/float 非有限值和 keep 布尔不合法时抛
 * ProtocolError。 */
[[nodiscard]] NativeHitRequest DecodeNativeHit(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 严格解析生命周期报告；不在协议层判断当前角色状态。 */
[[nodiscard]] NativeLifecycleRequest DecodeNativeLifecycle(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 严格解析普通复活的请求/完成两种固定布局。 */
[[nodiscard]] NativeRespawnRequest DecodeNativeRespawn(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

/** @brief 编码 C2S704 成功后只回给请求者的 16 字节 S2C707 授权体。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativeRespawnAuthorization(
    std::uint32_t actor_object_id, std::uint32_t respawn_point);

/** @brief 原生营救握手；33284 在线格式中将 helper 放在 victim 前。 */
struct NativeRescueRequest final {
  std::uint32_t victim{0U};  ///< 泡封者对象。
  std::uint32_t helper{0U};  ///< 营救者对象。
  std::uint8_t kind{0U};     ///< 原生营救类型 0/1/3；恢复血量消息中为空。
};

/** @brief 通用玩家及海盗 NPC 的营救消息。 */
[[nodiscard]] bool IsNativeRescueOpcode(std::uint16_t opcode) noexcept;
/** @brief 严格校验营救请求、恢复血量及完成效果的布局。 */
[[nodiscard]] NativeRescueRequest DecodeNativeRescue(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_COMBAT_MESSAGES_H_
