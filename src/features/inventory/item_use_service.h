#ifndef BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_USE_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_USE_SERVICE_H_

/**
 * @file item_use_service.h
 * @brief 定义原生仓库道具使用、角色染色与宠物成长事务边界。
 */

#include <cstdint>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"
#include "features/battle/match_id.h"
#include "features/characters/nickname.h"
#include "features/chat/megaphone.h"
#include "features/inventory/inventory_discard.h"
#include "features/rooms/room_ownership.h"

namespace bf::application {
/** @brief 原生通用道具使用与独立宠物升阶段请求。 */
enum class ItemUseOperation : std::uint8_t {
  kUse,
  kPetLevelUp,
  kInstantRespawn,
};

/** @brief 已认证玩家发送的通用道具使用参数。 */
struct UseItemCommand final {
  domain::PlayerId player_id;  ///< 连接身份，不能由载荷伪造。
  domain::ItemId item_id;      ///< 使用的本人库存模板。
  std::uint32_t quantity{1U};  ///< 请求使用数量。
  std::string
      target;  ///< 染色为空；喂粮为宠物编号；改名为新昵称；扩张券固定为10。
  ItemUseOperation operation{
      ItemUseOperation::kUse};  ///< C2S143 为独立升阶段。
  std::optional<domain::MatchId>
      match_id;                       ///< 世界线程冻结的即时复活对局身份。
  std::uint64_t life_generation{0U};  ///< 同一条生命重复请求不得再次扣费。
  std::optional<RoomOwnership>
      room_owner;  ///< 独立 DS 的战斗扣费必须携带当前所有权。
};

/** @brief 道具使用提交结果。 */
struct UseItemResult final {
  std::uint16_t color_code{0U};  ///< 已持久化的原版颜色编码。
  bool consumed{false};  ///< 是否实际消耗道具；同色重试和独立升阶段不扣道具。
  bool equipment_restored{
      false};  ///< 零Life恢复了已保存配装引用，网关需要同步有效装备。
};

/** @brief 已通过入口校验的宠物成长事务参数。 */
struct PetGrowthCommand final {
  domain::PlayerId player_id;  ///< 经认证的主人。
  domain::ItemId pet_id;       ///< 原版目标宠物模板，必须为本人有效库存。
  domain::ItemId food_id;      ///< 喂食粮食；升阶段使用无效标识零。
  std::uint32_t maximum_level{
      2U};  ///< 目录允许的内部阶段上限，显示阶段为其加一。
  std::chrono::seconds hunger_seconds_per_point{
      72};  ///< 初始冷却政策，可由用例注入。
};

/** @brief 道具资产事务端口，状态修改与消耗必须原子提交。 */
class ItemUseRepository {
 public:
  virtual ~ItemUseRepository() = default;

  /** @brief
   * 原子清除本人已过期非宠物实例及失效配装，保留不可变审计；无可用资产扣款。 */
  [[nodiscard]] virtual ServiceResult<ExpiredItemDiscardResult> DiscardExpired(
      const ExpiredItemDiscardCommand&, std::chrono::system_clock::time_point) {
    return ServiceError{.code = ErrorCode::kInvalidState,
                        .safe_message = "过期删弃事务不可用"};
  }

  /** @brief 一支生命注射器为本人全部未满 Life
   * 道具各补一点，扣除及审计原子提交。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> ReplenishLife(
      domain::PlayerId, std::chrono::system_clock::time_point) {
    return ServiceError{.code = ErrorCode::kStorageUnavailable,
                        .safe_message = "Life事务不可用"};
  }

  /** @brief 原子更新昵称唯一键并扣除一张有效改名卡；相同昵称重试不扣卡。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> Rename(
      domain::PlayerId player, const domain::Nickname& nickname,
      std::chrono::system_clock::time_point now) = 0;
  /** @brief 锁定玩家与有效库存并提交颜色，失败回滚。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> Dye(
      domain::PlayerId player, domain::ItemId item, std::uint16_t color,
      std::chrono::system_clock::time_point now) = 0;
  /** @brief 锁定主人与有效库存，原子提交喂粮或升阶段；失败不得扣粮。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> GrowPet(
      const PetGrowthCommand& command,
      std::chrono::system_clock::time_point now) = 0;
  /** @brief 会长使用原版扩张券增加十名容量，上限一百；扣券、扩容及流水同事务。
   */
  [[nodiscard]] virtual ServiceResult<UseItemResult> ExpandClan(
      domain::PlayerId player, std::chrono::system_clock::time_point now) = 0;
  /** @brief 按对局、玩家和生命幂等扣除即时复活道具并保留审计。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> InstantRespawn(
      const UseItemCommand& command,
      std::chrono::system_clock::time_point now) = 0;
  /** @brief 原子扣除喇叭、记账并保存待广播正文；内部重试和短时重复不再扣费。 */
  [[nodiscard]] virtual ServiceResult<MegaphoneReceipt> Megaphone(
      const MegaphoneCommand& command,
      std::chrono::system_clock::time_point now) = 0;
  /** @brief 有界加载尚未完成投递的喇叭，支持进程重启恢复。 */
  [[nodiscard]] virtual ServiceResult<std::vector<MegaphoneReceipt>>
  PendingMegaphones() = 0;
  /** @brief 只在世界线程已尝试在线广播之后提交投递回执。 */
  [[nodiscard]] virtual std::optional<ServiceError> AcknowledgeMegaphone(
      std::string_view id) = 0;
};

/** @brief 网关使用的道具业务接口。 */
class ItemUseExecutor {
 public:
  virtual ~ItemUseExecutor() = default;

  /** @brief 原808仅删除已过期本人物品，活跃与永久物品不能用模板键销毁。 */
  [[nodiscard]] virtual ServiceResult<ExpiredItemDiscardResult> DiscardExpired(
      const ExpiredItemDiscardCommand&) const {
    return ServiceError{.code = ErrorCode::kInvalidState,
                        .safe_message = "过期删弃事务不可用"};
  }

  /** @brief 校验原版请求并提交已实现的道具用途。 */
  [[nodiscard]] virtual ServiceResult<UseItemResult> Use(
      const UseItemCommand& command) const = 0;
  [[nodiscard]] virtual ServiceResult<MegaphoneReceipt> Megaphone(
      const MegaphoneCommand& command) const = 0;
  [[nodiscard]] virtual ServiceResult<std::vector<MegaphoneReceipt>>
  PendingMegaphones() const = 0;
  [[nodiscard]] virtual std::optional<ServiceError> AcknowledgeMegaphone(
      std::string_view id) const = 0;
};

/** @brief 使用已验证喷剂/宠物目录执行染色、喂粮和升阶段。 */
class ItemUseService final : public ItemUseExecutor {
 public:
  /** @brief 借用时钟与事务仓储。 */
  ItemUseService(const SystemClock& clock,
                 ItemUseRepository& repository) noexcept;
  [[nodiscard]] ServiceResult<ExpiredItemDiscardResult> DiscardExpired(
      const ExpiredItemDiscardCommand& command) const override;
  /** @copydoc ItemUseExecutor::use */
  [[nodiscard]] ServiceResult<UseItemResult> Use(
      const UseItemCommand& command) const override;
  [[nodiscard]] ServiceResult<MegaphoneReceipt> Megaphone(
      const MegaphoneCommand& command) const override;
  [[nodiscard]] ServiceResult<std::vector<MegaphoneReceipt>> PendingMegaphones()
      const override;
  [[nodiscard]] std::optional<ServiceError> AcknowledgeMegaphone(
      std::string_view id) const override;

 private:
  const SystemClock& clock_;
  ItemUseRepository& repository_;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_INVENTORY_ITEM_USE_SERVICE_H_
