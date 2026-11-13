#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_WORKSHOP_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_WORKSHOP_SERVICE_H_

/**
 * @file workshop_service.h
 * @brief 定义科学工坊合成、分解和限时转永久的规则与原子用例。
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"
#include "application/system_clock.h"
#include "features/configuration/service_rules.h"

namespace bf::application {

/** @brief build 752 已验证的工坊动作代码。 */
enum class WorkshopAction : std::uint8_t {
  kCompound = 132U,
  kDecompose = 133U,
  kPermanent = 137U,
};

/** @brief 经过应用层规则校验后交给单事务仓储的完整工坊命令。 */
struct WorkshopCommand final {
  domain::OperationId operation_id;  ///< 操作、流水和历史共用 UUID。
  domain::PlayerId player_id;        ///< 资产所属玩家。
  WorkshopAction action{WorkshopAction::kCompound};  ///< 已验证动作。
  std::vector<domain::ItemId> inputs;  ///< 不重复且已按发布允许矩阵校验的输入。
  std::uint32_t option{0U};  ///< 合成0普通、1..5保护对应注册槽；其余动作零。
  std::uint64_t rules_version_id{0U};  ///< 当前已发布 service_rules 版本。
  domain::ItemId reward_item_id;       ///< 合成随机结果或分解/永久化固定结果。
  std::chrono::seconds reward_duration{
      0};  ///< 旧固定期限字段；新132在事务内算真实期限，填零。
  domain::ItemId required_material_id;  ///< 本动作额外消耗的卡片或材料。
  std::uint32_t required_material_quantity{1U};  ///< 当前原版动作固定消耗一份。
  std::chrono::system_clock::time_point now;     ///< 全事务统一 UTC 时间。
  std::optional<WorkshopCompoundPolicy>
      compound_policy{};  ///< 冻结新政策；旧固定期限不用于132。
  std::uint32_t compound_percent{
      0U};  ///< 本服50..100整数百分比，事务按实际期限计算。
  std::uint32_t protection_percent{0U};  ///< 有保护时独立50..100整数百分比。
};

/** @brief 工坊成功后的原版结果字段。 */
struct WorkshopResult final {
  WorkshopAction action{
      WorkshopAction::kCompound};       ///< 响应 opcode 的动作来源。
  std::vector<domain::ItemId> rewards;  ///< 原版允许重复的奖励模板序列。
  std::vector<domain::ItemId> consumed_inputs;  ///< 被消耗的装备；永久化为空。
  double reward_days{0.0};  ///< 合成天数或永久兼容值 36500。
};

/** @brief 工坊事务仓储端口；实现必须把全部资产变化、流水和操作历史一次提交。 */
class WorkshopRepository {
 public:
  virtual ~WorkshopRepository() = default;

  /** @brief 锁定玩家和全部资产，执行命令并返回提交后的固定结果。 */
  [[nodiscard]] virtual ServiceResult<WorkshopResult> Execute(
      const WorkshopCommand& command) = 0;
};

/** @brief 合成奖励候选选择所用的无偏随机端口。 */
class WorkshopRandomSource {
 public:
  virtual ~WorkshopRandomSource() = default;

  /** @brief 返回 `[0, upper_bound)` 内一个无偏索引。 */
  [[nodiscard]] virtual ServiceResult<std::size_t> Choose(
      std::size_t upper_bound) = 0;
};

/** @brief 151与1500科学配置前3个DWORD使用的已发布本服值，不是材料数量上下限。
 */
struct WorkshopCompoundConfiguration final {
  domain::ItemId compound_card_item_id;
  domain::ItemId protection_material_item_id;
  std::uint32_t minimum_remaining_days{0U};
};

/** @brief 网关 132～137 路由依赖的工坊业务接口。 */
class WorkshopExecutor {
 public:
  virtual ~WorkshopExecutor() = default;

  /** @brief 只读公布132当前政策；未恢复实现和旧v1不能把固定旧值伪装成可用配置。
   */
  [[nodiscard]] virtual ServiceResult<WorkshopCompoundConfiguration>
  CompoundConfiguration() const {
    return ServiceError{
        .code = ErrorCode::kInvalidState,
        .safe_message = "尚未发布按原版材料和期限规则恢复的合成政策"};
  }

  /** @brief 校验动作、输入、选项及发布规则后执行一个原子工坊操作。 */
  [[nodiscard]] virtual ServiceResult<WorkshopResult> Execute(
      domain::OperationId operation_id, domain::PlayerId player_id,
      WorkshopAction action, std::span<const std::uint32_t> input_item_ids,
      std::uint32_t option) const = 0;
};

/** @brief 使用冻结 service_rules 选择奖励并把资产写入委托给仓储的应用服务。 */
class WorkshopService final : public WorkshopExecutor {
 public:
  /** @brief 注入 UTC 时钟、规则、随机源和事务仓储；生命周期必须覆盖本服务。 */
  WorkshopService(const SystemClock& clock, ServiceRulesLoader& rules,
                  WorkshopRandomSource& random,
                  WorkshopRepository& repository) noexcept;

  [[nodiscard]] ServiceResult<WorkshopCompoundConfiguration>
  CompoundConfiguration() const override;

  [[nodiscard]] ServiceResult<WorkshopResult> Execute(
      domain::OperationId operation_id, domain::PlayerId player_id,
      WorkshopAction action, std::span<const std::uint32_t> input_item_ids,
      std::uint32_t option) const override;

 private:
  const SystemClock& clock_;
  ServiceRulesLoader& rules_;
  WorkshopRandomSource& random_;
  WorkshopRepository& repository_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_WORKSHOP_SERVICE_H_
