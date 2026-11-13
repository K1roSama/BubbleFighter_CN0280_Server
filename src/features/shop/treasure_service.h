#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SHOP_TREASURE_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SHOP_TREASURE_SERVICE_H_

/**
 * @file treasure_service.h
 * @brief 定义五泡抽取的固定结果、原子消耗、恢复和一次领取用例。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"
#include "application/system_clock.h"
#include "features/configuration/service_rules.h"

namespace bf::application {

/** @brief 某种魔法针在当前有效库存中的可用数量。 */
struct TreasureNeedleCount final {
  domain::ItemId item_id;      ///< 与规则列表对应的针模板。
  std::uint32_t quantity{0U};  ///< 过期项按零返回，协议范围外由存储层拒绝。
};

/** @brief 已提交抽取的不可变二选一结果；断线恢复不得重新随机。 */
struct TreasureDrawSnapshot final {
  std::uint64_t draw_id{0U};                ///< MySQL 稳定抽取编号。
  domain::PlayerId player_id;               ///< 结果所有者。
  domain::ItemId needle_item_id;            ///< 事务实际消耗的魔法针。
  std::uint32_t needle_quantity_after{0U};  ///< 提交后该针剩余数量。
  std::uint32_t bubble_balance_after{0U};   ///< 提交后 treasure_bubble 余额。
  std::array<std::uint32_t, 2U>
      positions{};  ///< 客户端允许选择的两个不同格位。
  std::array<ServiceRewardRule, 5U>
      rewards{};  ///< 固定顺序的五个预览及实际奖励快照。
  std::chrono::seconds selection_duration{0};  ///< 生成时冻结的 UI 选择倒计时。
  std::uint64_t rules_version_id{0U};          ///< 生成本结果的发布规则版本。
};

/** @brief 打开泡泡面板需要的当前钱包、十一种针和可选待领取结果。 */
struct TreasureStateSnapshot final {
  domain::PlayerId player_id;        ///< 状态所属玩家。
  std::uint32_t bubble_balance{0U};  ///< 当前 treasure_bubble 余额。
  std::vector<TreasureNeedleCount>
      needle_counts;  ///< 按 published 规则顺序返回。
  std::optional<TreasureDrawSnapshot>
      pending_draw;  ///< 未领取结果；不存在为空。
};

/** @brief 一次抽取事务的完整固定输入。 */
struct TreasureOpenPersistenceCommand final {
  domain::OperationId operation_id;    ///< 消耗、钱包和抽取审计共用 UUID。
  domain::PlayerId player_id;          ///< 发起抽取的玩家。
  std::uint64_t rules_version_id{0U};  ///< 必须仍指向 published service_rules。
  BubbleNeedleRule needle;             ///< 服务端规则选出的针及消耗数量。
  std::array<std::uint32_t, 2U> positions{};  ///< 两个不同的 0..4 格位。
  std::array<ServiceRewardRule, 5U>
      rewards{};  ///< 事务写入、以后领取复用的奖励快照。
  std::chrono::seconds selection_duration{0};  ///< 写入结果的选择时长。
  std::chrono::system_clock::time_point now;   ///< 统一 UTC 业务时间。
};

/** @brief 一次领取事务的权威选择。 */
struct TreasureClaimPersistenceCommand final {
  domain::OperationId operation_id;  ///< 奖励库存流水的稳定 UUID。
  domain::PlayerId player_id;        ///< 只能领取本人 pending 结果。
  std::uint32_t choice{0U};          ///< 二选一下标，只允许 0 或 1。
  std::uint32_t position{0U};        ///< 必须等于 pending.positions[choice]。
  std::chrono::system_clock::time_point now;  ///< 领取及库存期限统一 UTC 时间。
};

/** @brief 领取成功后返回的固定五项预览和实际发放物品。 */
struct TreasureClaimResult final {
  domain::PlayerId player_id;                ///< 领取玩家。
  std::uint64_t draw_id{0U};                 ///< 被终结的抽取编号。
  std::uint32_t position{0U};                ///< 客户端动画选中的格位。
  std::array<domain::StockId, 5U> stocks{};  ///< 原顺序的五个预览 stock。
  domain::ItemId granted_item_id;            ///< 事务实际发放模板。
};

/** @brief 泡泡抽取 MySQL 端口；每个写方法必须独立完成一个短事务。 */
class TreasureRepository {
 public:
  virtual ~TreasureRepository() = default;

  /** @brief 一致性读取钱包、有效针库存和未领取抽取。 */
  [[nodiscard]] virtual ServiceResult<TreasureStateSnapshot> LoadState(
      domain::PlayerId player_id, const BubbleServiceRules& rules,
      std::chrono::system_clock::time_point now) = 0;

  /**
   * @brief 原子锁玩家、扣针、扣一个泡泡、写资产流水和固定抽取；已有 pending
   * 时直接恢复。
   * @return pair 的 bool 仅在本次事务真正消耗并创建时为 true。
   */
  [[nodiscard]] virtual ServiceResult<std::pair<TreasureDrawSnapshot, bool>>
  Open(const TreasureOpenPersistenceCommand& command) = 0;

  /** @brief 锁定 pending，复核选择，发放冻结奖励并只将该结果终结一次。 */
  [[nodiscard]] virtual ServiceResult<TreasureClaimResult> Claim(
      const TreasureClaimPersistenceCommand& command) = 0;
};

/** @brief 为抽取提供无偏、不重复索引的密码学随机端口。 */
class TreasureRandomSource {
 public:
  virtual ~TreasureRandomSource() = default;

  /** @brief 从 `[0, upper_bound)` 无放回抽取 count
   * 个索引；边界错误或熵源失败返回错误。 */
  [[nodiscard]] virtual ServiceResult<std::vector<std::size_t>>
  SampleWithoutReplacement(std::size_t upper_bound, std::size_t count) = 0;
};

/** @brief 网关泡泡路由依赖的业务接口。 */
class TreasureExecutor {
 public:
  virtual ~TreasureExecutor() = default;

  /** @brief 读取面板状态及可恢复结果。 */
  [[nodiscard]] virtual ServiceResult<TreasureStateSnapshot> State(
      domain::PlayerId player_id) const = 0;

  /** @brief
   * 校验格位、随机冻结五项奖励并执行抽取；空格位列表兼容生成相邻二选一。 */
  [[nodiscard]] virtual ServiceResult<std::pair<TreasureDrawSnapshot, bool>>
  Open(domain::OperationId operation_id, domain::PlayerId player_id,
       domain::ItemId needle_item_id, std::uint32_t primary_position,
       std::span<const std::uint32_t> positions) const = 0;

  /** @brief 领取本人的 pending 结果；错误选择不改变库存或结果状态。 */
  [[nodiscard]] virtual ServiceResult<TreasureClaimResult> Claim(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint32_t choice, std::uint32_t position) const = 0;
};

/**
 * @brief 使用 published service_rules 驱动五泡抽取，不接受客户端奖励或成本。
 * @note
 * 随机发生在写事务前；事务保存完整奖励快照，所以断线、配置退役和领取重试都不会重抽。
 */
class TreasureService final : public TreasureExecutor {
 public:
  /** @brief
   * 注入时钟、规则加载器、随机源和事务仓储；依赖生命周期必须覆盖本服务。 */
  TreasureService(const SystemClock& clock, ServiceRulesLoader& rules,
                  TreasureRandomSource& random,
                  TreasureRepository& repository) noexcept;

  /** @brief 读取并复核状态顺序、玩家和 pending 归属。 */
  [[nodiscard]] ServiceResult<TreasureStateSnapshot> State(
      domain::PlayerId player_id) const override;

  /** @brief 只允许发布针及两个不同格位，并从固定池无放回选出五项奖励。 */
  [[nodiscard]] ServiceResult<std::pair<TreasureDrawSnapshot, bool>> Open(
      domain::OperationId operation_id, domain::PlayerId player_id,
      domain::ItemId needle_item_id, std::uint32_t primary_position,
      std::span<const std::uint32_t> positions) const override;

  /** @brief 只允许 choice 0/1，最终格位匹配由锁定 pending 的事务再次验证。 */
  [[nodiscard]] ServiceResult<TreasureClaimResult> Claim(
      domain::OperationId operation_id, domain::PlayerId player_id,
      std::uint32_t choice, std::uint32_t position) const override;

 private:
  const SystemClock& clock_;
  ServiceRulesLoader& rules_;
  TreasureRandomSource& random_;
  TreasureRepository& repository_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SHOP_TREASURE_SERVICE_H_
