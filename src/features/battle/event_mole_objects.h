#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_OBJECTS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_OBJECTS_H_

/** @file event_mole_objects.h @brief EventMole 的有序原生对象与已授权金币账本。
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "application/identifier.h"
#include "features/battle/event_mole_profile.h"
#include "protocol/battle/special_mode_messages.h"

namespace bf::runtime {

/** @brief 冻结玩家身份及独立确认的场景初始计数；不得从2018自报或actor加一推导。
 */
struct EventMoleOwner final {
  domain::PlayerId player;
  std::uint32_t seat;
  std::uint32_t actor;
  std::uint32_t confirmed_next_object{
      0U};  ///< 完整初始化已消费后的首个空闲ID；0表示缺失。
};

/** @brief 已登记的 ChrMole；slot 和 object 始终分开保存。 */
struct EventMoleObject final {
  std::uint32_t object;
  std::uint32_t slot;
  std::uint32_t spawner;
  std::uint32_t property;
  domain::PlayerId owner;
  std::array<float, 3U> position;
  bool visible{false};
};

/** @brief
 * 原地图计划及所有接收端顺序独立确认的构造事实；原32892自身不产生该事实。 */
struct EventMoleConstruction final {
  domain::PlayerId owner;  ///< 已确认的原生authority，不能从移动/命中包反推。
  std::uint64_t
      authority_generation;        ///< 房主选任代次；旧代次不继承新房主授权。
  std::uint32_t expected_counter;  ///< 所有同owner构造事件消费后的可靠计数器。
  protocol::NativeMoleSpawn
      spawn;  ///< 已确认计划实例；opaque_8仅保留字节，不能认证身份。
};

/** @brief 来自独立已确认命中的有限凭据；隐藏动作不产生凭据。 */
struct EventMoleHit final {
  domain::PlayerId attacker;
  std::uint32_t target;
  std::uint64_t source;
  std::uint64_t attacker_life;
};

/**
 * @brief
 * 保存接收端同owner的严格对象创建顺序，而非从移动/显隐/命中首包认领对象。
 * @note
 * 必须由房间executor按实际接收端顺序调用。advance_creation仅供已独立验证、实际
 *       发给所有接收者的其他对象构造事件。缺失任何构造事件须mark_order_gap；2018自报
 *       快照不能修复该gap或推进counter。本组件尚未安装到生产17，完整构造生产者和原
 *       on-hit参数/终局链缺失时生产开局继续保护。
 */
class EventMoleObjects final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit EventMoleObjects(std::span<const EventMoleOwner> owners,
                            domain::PlayerId authority);

  /** @brief
   * 为实际已认证的共用counter构造事件分配一段ID，必须先核对预期counter。 */
  [[nodiscard]] std::optional<std::uint32_t> AdvanceCreation(
      domain::PlayerId owner, std::uint32_t expected_next, std::uint32_t count);
  /** @brief 发现漏掉或未知构造后使owner顺序失效；不允许借2018报告任意跳号。 */
  void MarkOrderGap(domain::PlayerId owner) noexcept;
  /** @brief 返回有序且未离场owner的下一个原生对象ID；gap返回空值。 */
  [[nodiscard]] std::optional<std::uint32_t> NextObject(
      domain::PlayerId owner) const noexcept;
  /** @brief
   * 2018仅核对当前已确认counter和本人席位；不复制客户端HP/道具、不修复顺序gap。
   */
  [[nodiscard]] bool CheckpointMatches(
      domain::PlayerId owner,
      const protocol::NativeObjectCheckpointRequest& request) const noexcept;
  /** @brief 换房主不迁移原owner的counter，也不改变已经登记对象的owner。 */
  [[nodiscard]] bool ChangeAuthority(domain::PlayerId owner) noexcept;

  /** @brief 消费独立确认的构造事实；原始32892不得直接调用本入口授权地鼠。 */
  [[nodiscard]] std::optional<EventMoleObject> CreateMole(
      const EventMoleConstruction& fact);
  /** @brief 当前原生authority选任代次，供已经独立确认的构造事实绑定。 */
  [[nodiscard]] std::uint64_t AuthorityGeneration() const noexcept;
  /** @brief 只查询已登记的真实ChrMole，查询绝不建立对象或授予权限。 */
  [[nodiscard]] const EventMoleObject* Object(std::uint32_t id) const noexcept;
  /** @brief 已登记对象显隐；不移除对象、不产生击败/金币或消耗slot。 */
  [[nodiscard]] bool SetState(domain::PlayerId owner,
                              const protocol::NativeMoleState& state);
  /** @brief 仅独立认证的原对象销毁事件释放slot；32918(state12)不得调用此入口。
   */
  [[nodiscard]] bool DestroyMole(domain::PlayerId owner, std::uint32_t object);

  /** @brief 检查已认证命中凭据，只建立一次后续掉落资格；不猜伤害值/HP或概率。
   */
  [[nodiscard]] std::optional<std::uint64_t> ConfirmHit(
      const EventMoleHit& hit);
  /** @brief 消耗真实命中资格登记服务器已授权的kind1/2币。原权重生产者未安装。
   */
  [[nodiscard]] bool RegisterAuthorizedCoin(
      std::uint64_t hit, const protocol::NativeMoleCoinSpawn& coin,
      Clock::time_point now);
  /** @brief 只消费已经存在且激活未过期的金币；返回1/100，本组件不操作账户资产。
   */
  [[nodiscard]] std::optional<std::uint32_t> Collect(
      domain::PlayerId player, const protocol::NativeMoleCoinPickup& request,
      Clock::time_point now);
  /** @brief 返回已消费原生金币的本局累计；不代表账户钱包、评级或胜负。 */
  [[nodiscard]] std::uint64_t Collected(domain::PlayerId player) const noexcept;
  /** @brief 2026只消费真实拾取后尚未镜像的10/13次数，永不再次加金币。 */
  [[nodiscard]] bool ObserveCollectionStat(
      domain::PlayerId player, const protocol::NativeMoleCoinStat& report);
  /** @brief 断线撤销该owner权限、对象、未消费hit；既得本局计数保留供终局冻结。
   */
  void Leave(domain::PlayerId player) noexcept;

 private:
  /** @brief 单一冻结owner的counter、连接有效性和已收币事实。 */
  struct Owner final {
    EventMoleOwner identity;
    std::uint32_t next;
    bool ordered{true};
    bool active{true};
    std::uint64_t collected{0U};
    std::array<std::uint64_t, 2U> pending_stats{};
  };

  /** @brief 原生金币激活、消失期限和永久单次消费墓碑。 */
  struct Coin final {
    std::uint32_t amount;
    Clock::time_point activate;
    Clock::time_point expire;
    bool consumed{false};
  };

  std::map<domain::PlayerId, Owner> owners_;
  domain::PlayerId authority_;
  std::uint64_t authority_generation_{1U};
  std::map<std::uint32_t, EventMoleObject> objects_;
  std::array<std::optional<std::uint32_t>, 64U> slots_{};
  std::set<std::uint32_t> used_objects_;
  std::map<std::uint64_t, EventMoleHit> hits_;
  std::set<std::array<std::uint64_t, 4U>> hit_keys_;
  std::map<std::uint32_t, Coin> coins_;
  std::uint64_t hit_sequence_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_EVENT_MOLE_OBJECTS_H_
