#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_WEAPON_MASTER_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_WEAPON_MASTER_MESSAGES_H_

/**
 * @file weapon_master_messages.h
 * @brief 定义武器大师原生助攻、复活换武器与局内经验字段；不推测升级规则。
 */

#include <cstdint>
#include <span>
#include <vector>

namespace bf::protocol {

/** @brief C2S2026 事件 39；发送者是死亡者，DWORD 是最终攻击者对象，BYTE
 * 是助攻席位。 */
struct WeaponMasterAssistRequest final {
  std::uint32_t finisher_object{0U};  ///< 5ECB40 的 a3，不能误认成发送者对象。
  std::uint8_t assistant_seat{
      0U};  ///< 伤害比例严格大于 1/2 且不是最终攻击者的席位。
};

/** @brief S2C1008 中原生换武器弹窗的三种原因。 */
enum class WeaponMasterChangeReason : std::uint32_t {
  kStart = 1U,    ///< 游戏开始。
  kLevelUp = 2U,  ///< 局内升级。
  kRevival = 3U,  ///< 未得分复活补偿。
};

/** @brief S2C1008 的两组等长 DWORD 向量合并为一条有类型的对应关系。 */
struct WeaponMasterWeapon final {
  std::uint32_t bar{0U};          ///< 角色武器栏 0..4。
  std::uint32_t template_id{0U};  ///< 武器模板，须由调用者查验资源及模式授权。
};

/** @brief S2C729 ScorePlayer+288..304
 * 的局内等级、累计经验和助攻；与账号经验独立。 */
struct WeaponMasterProgress final {
  std::uint32_t level{0U};       ///< 局内等级，1 起始。
  std::uint32_t experience{0U};  ///< 本局累计经验。
  std::uint32_t level_base{0U};  ///< 当前等级累计经验下界，首级原生 UI 使用 0。
  std::uint32_t level_next{0U};  ///< 下一等级累计经验门槛。
  std::uint32_t assists{
      0U};  ///< 原生终局 lbAssistCnt / lbMyAssistCnt 显示次数。
};

/** @brief 严格解析 6 字节事件 39；只校验格式，不给予助攻或奖励授权。 */
[[nodiscard]] WeaponMasterAssistRequest DecodeWeaponMasterAssist(
    std::span<const std::uint8_t> body);

/** @brief C2S760 原生没有负载；验证请求格式，不产生空成功回包。 */
void ValidateWeaponMasterReroll(std::span<const std::uint8_t> body);

/** @brief 校验可显示的原生经验区间；阈值由业务提供，协议层没有默认升级表。 */
void ValidateWeaponMasterProgress(const WeaponMasterProgress& progress);

/** @brief 编码 S2C1008：DWORD 席位、栏位向量、模板向量、DWORD 原因。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeWeaponMasterChange(
    std::uint8_t seat, std::span<const WeaponMasterWeapon> weapons,
    WeaponMasterChangeReason reason);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_WEAPON_MASTER_MESSAGES_H_
