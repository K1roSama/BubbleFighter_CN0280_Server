#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_SERVICE_H_

/**
 * @file equipment_service.h
 * @brief 定义玩家装备与卸下本人有效库存物品的事务用例。
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"

namespace bf::application {

/** @brief 仓库装备请求的动作类型。 */
enum class EquipmentAction : std::uint8_t {
  kEquip,
  kUnequip,
};

/** @brief 运行时已确认允许改装后提交给应用层的命令。 */
struct ChangeEquipmentCommand final {
  domain::PlayerId player_id;  ///< 装备所属玩家；不得使用账号 ID。
  domain::ItemId item_id;      ///< C2S804/805 提供的物品模板 ID。
  EquipmentAction action{EquipmentAction::kEquip};  ///< 装备或卸下。
  std::uint32_t loadout_id{0U};  ///< 当前原版流程使用零号配装页。
};

/** @brief 事务内锁定并确认未过期、可装备的本人库存项。 */
struct ActiveEquipableItem final {
  domain::ItemInstanceId item_instance_id;  ///< 玩家当前库存实例。
  domain::ItemId item_id;                   ///< 必须与请求模板相同。
  std::string slot_code;                    ///< 已发布模板定义的非空装备槽。
};

/** @brief 成功改装后返回给运行时刷新仓库、模型和准备状态的结果。 */
struct ChangeEquipmentResult final {
  domain::PlayerId player_id;                       ///< 改装玩家。
  domain::ItemId item_id;                           ///< 请求物品模板。
  domain::ItemInstanceId item_instance_id;          ///< 被装备或卸下的实例。
  std::string slot_code;                            ///< 受影响槽位。
  EquipmentAction action{EquipmentAction::kEquip};  ///< 已提交动作。
};

/**
 * @brief 换装数据库事务端口。
 * @note 实现必须锁定本人库存并在同一事务复核 expires_at；装备使用槽位
 * upsert，卸下只删除 指向目标实例的映射，不能误删该槽位随后换上的其他物品。
 */
class ChangeEquipmentTransaction {
 public:
  virtual ~ChangeEquipmentTransaction() = default;

  /** @brief 锁定玩家并选择有效武器页；战斗模式约束必须在同一事务复核。 */
  [[nodiscard]] virtual std::optional<ServiceError> SelectPreset(
      domain::PlayerId player_id, std::uint8_t preset,
      std::chrono::system_clock::time_point now, std::uint32_t combat_mode) = 0;

  /** @brief 锁定本人未过期库存及模板槽位；不存在、过期或不可装备返回冲突。 */
  [[nodiscard]] virtual ServiceResult<ActiveEquipableItem> LockActiveItem(
      domain::PlayerId player_id, domain::ItemId item_id,
      std::chrono::system_clock::time_point now) = 0;

  /** @brief 原子替换同配装页同槽位的旧映射为目标实例。 */
  [[nodiscard]] virtual std::optional<ServiceError> Equip(
      domain::PlayerId player_id, std::uint32_t loadout_id,
      std::string_view slot_code, domain::ItemInstanceId item_instance_id) = 0;

  /** @brief 仅当槽位仍指向目标实例时删除映射；未装备也按兼容行为视为成功。 */
  [[nodiscard]] virtual std::optional<ServiceError> UnequipIfCurrent(
      domain::PlayerId player_id, std::uint32_t loadout_id,
      std::string_view slot_code, domain::ItemInstanceId item_instance_id) = 0;

  /** @brief 提交一次换装；提交前返回的结果不得向会话广播。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 换装事务工厂，由数据库工作线程和独占连接实现。 */
class ChangeEquipmentUnitOfWork {
 public:
  virtual ~ChangeEquipmentUnitOfWork() = default;

  /** @brief 开始短事务；未提交对象析构时必须回滚。 */
  [[nodiscard]] virtual ServiceResult<
      std::unique_ptr<ChangeEquipmentTransaction>>
  Begin() = 0;
};

/** @brief 运行时换装路由依赖的业务用例接口。 */
class EquipmentChanger {
 public:
  virtual ~EquipmentChanger() = default;

  /** @brief 提交武器页选择，扩展页必须持有未过期扩张卡。 */
  [[nodiscard]] virtual std::optional<ServiceError> SelectPreset(
      domain::PlayerId player_id, std::uint8_t preset,
      std::uint32_t combat_mode = 0U) const = 0;

  /** @brief 执行并提交一次装备或卸下操作。 */
  [[nodiscard]] virtual ServiceResult<ChangeEquipmentResult> Change(
      const ChangeEquipmentCommand& command) const = 0;
};

/**
 * @brief 将一项有效库存装备到唯一槽位，或安全卸下当前映射。
 * @note
 * 对局阶段禁改装属于会话/房间顺序规则，运行时必须在调用本用例前拒绝；应用层仍复核
 *       所有权、模板槽位和有效期，不能信任运行时缓存。
 */
class ChangeEquipmentService final : public EquipmentChanger {
 public:
  /** @brief 注入 UTC 时钟和事务工厂；生命周期必须覆盖本服务。 */
  ChangeEquipmentService(const SystemClock& clock,
                         ChangeEquipmentUnitOfWork& unit_of_work) noexcept;

  /** @brief 校验页号后在单一事务中切换并提交，失败不改动当前页。 */
  [[nodiscard]] std::optional<ServiceError> SelectPreset(
      domain::PlayerId player_id, std::uint8_t preset,
      std::uint32_t combat_mode = 0U) const override;

  /** @brief 执行并提交换装；失败时不返回可广播的成功状态。 */
  [[nodiscard]] ServiceResult<ChangeEquipmentResult> Change(
      const ChangeEquipmentCommand& command) const override;

 private:
  const SystemClock& clock_;
  ChangeEquipmentUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_EQUIPMENT_SERVICE_H_
