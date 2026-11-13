#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NPC_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NPC_MESSAGES_H_

/** @file npc_messages.h @brief 原版 NpcGameContext
 * 缺口的逐字段解码；不授予转发或对象创建权限。 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace bf::protocol {

/** @brief 32842 / 9DA4B0 -> 9DA620。没有客户端指定对象
 * ID，不可凭合法负载建立演员。 */
struct NativeNpcCreate final {
  std::u16string type_name;
  std::array<float, 3U> position;
  float yaw;
  std::uint32_t word;
  std::u16string settings;
};

/** @brief 32843 / 9E1E30 -> 7EFC80 的 41 字节，不是 C++
 * 结构体大小；未知字段保持中性。 */
struct NativeNpcDamage final {
  std::uint32_t object;
  std::uint32_t word_4;
  std::int8_t signed_byte;
  std::array<float, 3U> vector_9;
  std::array<float, 3U> vector_21;
  std::uint32_t word_33;
  float scalar_37;
};

/** @brief 32844 武器模板、32848 攻击者、32852 救援者、32853
 * 相关对象；不可当任意状态枚举。 */
struct NativeNpcPair final {
  std::uint32_t object;
  std::uint32_t word_4;
};

/** @brief 32845 / 9E1F50 -> 9E2330，末字段为 DWORD 布尔；word_20 不能推断为
 * HP。 */
struct NativeNpcTransition final {
  std::uint32_t object;
  std::uint32_t word_4;
  std::array<float, 3U> vector_8;
  std::uint32_t word_20;
  bool flag;
};

/** @brief 32846/32847，MobBase 虚函数296/300的对象号和原始向量。 */
struct NativeNpcVector final {
  std::uint32_t object;
  std::array<float, 3U> vector;
};

/** @brief 32849 / 9E2010 -> 9E22C0，带击败/计分效果，不是无害显示请求。 */
struct NativeNpcKicked final {
  std::uint32_t object;
  bool flag;
  std::uint32_t attacker;
  std::array<float, 3U> position;
};

/** @brief 32859/32863 是上下文触发表 key；不是获得 MobBase 权限的 actor ID。 */
struct NativeNpcTrigger final {
  std::uint32_t trigger;
};

/** @brief 32860 通过 9D88C0 创建；名称/组/位置合法不等于获得创建权限。 */
struct NativeNpcNamedSpawn final {
  std::u16string name;
  std::uint32_t word;
  std::array<float, 3U> position;
  float yaw;
};

/** @brief 32862 通过 9DCB00/9D8980 创建 MobLua，必须另证计划、原生类型和 ID
 * 分配。 */
struct NativeNpcGroupedSpawn final {
  std::uint32_t word_0;
  std::uint32_t word_4;
  std::u16string type_name;
  std::array<float, 3U> position;
  float yaw;
};

/** @brief 32861 命名组操作、32864 A9F690 创建命令；解码不执行脚本或创建物体。
 */
struct NativeNpcText final {
  std::u16string text;
};

using NativeNpcPayload =
    std::variant<NativeNpcCreate, NativeNpcDamage, NativeNpcPair,
                 NativeNpcTransition, NativeNpcVector, NativeNpcKicked,
                 NativeNpcTrigger, NativeNpcNamedSpawn, NativeNpcGroupedSpawn,
                 NativeNpcText>;

/** @brief 只标识已核实的16个收发布局，不是网关路由/支持玩法白名单。 */
[[nodiscard]] bool IsObservedNpcSyncOpcode(std::uint16_t opcode) noexcept;
/** @brief 严格有限数值、DWORD 布尔、UTF-16
 * 数量、截断和尾随字节；不改变生命/HP/统计。
 * @note 原版 .Dao 等七类在 AddMob 后登记的是同 ID 的 ChrLogic/Character
 * 代理，不可由本解码 推断为 MobBase。当前这些核心 opcode
 * 仍无网关路由，未知创建和不兼容目标仍阻断。
 */
[[nodiscard]] NativeNpcPayload DecodeObservedNpcSync(
    std::uint16_t opcode, std::span<const std::uint8_t> body);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_NPC_MESSAGES_H_
