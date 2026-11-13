#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVICE_RULES_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVICE_RULES_H_

/**
 * @file service_rules.h
 * @brief 定义泡泡抽取、神秘商店和工坊共用的不可变发布规则。
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {

/** @brief 一种可在泡泡 UI 使用的魔法针及单次消耗数量。 */
struct BubbleNeedleRule final {
  domain::ItemId item_id;  ///< 客户端已发布的针物品模板。
  std::uint32_t cost{0U};  ///< 每次抽取原子扣除的正整数数量。
};

/** @brief 泡泡预览 stock 对应的固定实际奖励，不依赖后来发布的商城。 */
struct ServiceRewardRule final {
  domain::StockId stock_id;    ///< 原版 UI 显示和回传使用的 stock ID。
  domain::ItemId item_id;      ///< 领取事务实际发放的物品模板。
  std::uint32_t quantity{0U};  ///< 实际发放数量；限时物品固定为一。
  std::optional<std::chrono::seconds>
      duration;  ///< 限时长度；永久或数量合并物品为空。
};

/** @brief build 752 五泡抽取的全部可复现策略。 */
struct BubbleServiceRules final {
  std::uint32_t initial_balance{
      0U};  ///< 首次建立 treasure_bubble 钱包的期初值。
  std::chrono::seconds selection_duration{0};  ///< 客户端二选一倒计时展示值。
  std::vector<BubbleNeedleRule> needles;  ///< 固定客户端顺序的十一种魔法针。
  domain::ItemId super_needle_item_id;    ///< 触发十个消耗分支的特殊魔法针。
  std::vector<ServiceRewardRule>
      reward_pool;  ///< 至少五种、不重复 stock 的奖励快照池。
};

/** @brief 神秘商店的一项固定候选规格。 */
struct MysteryOfferRule final {
  domain::StockId stock_id;  ///< 购买动作引用的旧客户端 stock ID。
  domain::ItemId item_id;    ///< 成交时实际发放的模板。
  std::string item_name;     ///< S2C140 展示的严格 UTF-8 名称。
  std::uint32_t price{0U};   ///< 服务端权威 Lucci 成交价，忽略客户端价格。
  std::chrono::seconds duration{0};  ///< 当前恢复分支只允许一至三十整天。
};

/** @brief 玩家固定选品、刷新和购买使用的神秘商店规则。 */
struct MysteryShopServiceRules final {
  domain::ItemId refresh_item_id;         ///< 刷新券物品模板。
  std::uint32_t refresh_quantity{0U};     ///< 每次刷新原子扣除数量。
  std::uint32_t selection_size{0U};       ///< 每代选择的不同物品数量。
  std::chrono::seconds selection_ttl{0};  ///< 固定选品有效期。
  std::vector<MysteryOfferRule> offers;   ///< 按 stock ID 严格升序的候选池。
};

/** @brief 单个物品允许进入哪些已恢复工坊动作。 */
struct WorkshopItemRule final {
  domain::ItemId item_id;  ///< 客户端物品模板。
  std::uint32_t compound_group{
      0U};  ///< 旧field_68目录分类；v2不把它当作同组限制。
  bool compound_reward_candidate{
      false};  ///< 旧catalog候选字段；v2主结果仅来自注册输入，不用此位。
  bool compound{false};   ///< 可作为合成输入或输出候选。
  bool decompose{false};  ///< 可作为分解输入。
  bool permanent{false};  ///< 可进入限时转永久动作。
  std::uint32_t native_is_gashapon{
      0U};  ///< v2 raw_client_fields.isGashapon原整数。
  std::uint32_t native_is_compound{
      0U};  ///< v2 raw_client_fields.isCompound原整数。
  std::string
      native_duration_type{};  ///< v2 raw_client_fields.durationType原字符串。
};

/**
 * @brief 132
 * 新政策；原客户端确定候选来自注册材料，概率分布和部分参数由本服明确发布。
 * @note 20
 * 日、190110、独立均匀百分比及同模板保护期限合并是本服政策，非原服务器取值。
 */
struct WorkshopCompoundPolicy final {
  std::uint32_t version{0U};
  std::uint32_t policy_epoch{0U};
  std::uint32_t input_count{0U};
  domain::ItemId protection_material_item_id;
  std::uint32_t minimum_remaining_days{0U};
  std::uint32_t maximum_remaining_days{0U};
  std::uint32_t permanent_threshold_days{0U};
  std::uint32_t minimum_percent{0U};
  std::uint32_t maximum_percent{0U};
  std::string probability_policy;
  std::string protected_merge_policy;
  std::string catalog_sha256;
};

/** @brief build 752 已恢复的 132、133、137 工坊策略。 */
struct WorkshopServiceRules final {
  domain::ItemId compound_card_item_id;  ///< 合成卡模板。
  std::uint32_t compound_min{0U};        ///< 合成最少不同输入数。
  std::uint32_t compound_max{0U};        ///< 合成最多不同输入数。
  std::chrono::seconds compound_duration{
      0};  ///< 旧v1固定期限归档字段；v2不用它计算132。
  domain::ItemId decompose_card_item_id;      ///< 分解卡模板。
  std::uint32_t decompose_max{0U};            ///< 单次分解最多不同输入数。
  domain::ItemId decompose_reward_item_id;    ///< 每个输入对应的材料奖励模板。
  domain::ItemId permanent_material_item_id;  ///< 限时转永久消耗的材料模板。
  std::vector<WorkshopItemRule>
      item_rules;  ///< 按 item ID 严格升序的已发布允许矩阵。
  std::optional<WorkshopCompoundPolicy>
      compound_policy{};  ///< 缺失时只允许旧133/137，132不扣料。
};

/** @brief 一次不可变 service_rules 发布版本的完整应用视图。 */
struct PublishedServiceRules final {
  std::uint64_t configuration_version_id{
      0U};                     ///< 数据库外键及历史操作冻结的配置版本。
  BubbleServiceRules bubbles;  ///< 泡泡抽取策略。
  MysteryShopServiceRules mystery_shop;  ///< 神秘商店策略。
  WorkshopServiceRules workshop;         ///< 工坊策略。
};

/** @brief 已发布扩展业务规则的持久化读取端口。 */
class ServiceRulesReader {
 public:
  virtual ~ServiceRulesReader() = default;

  /** @brief 读取唯一 published 规则；没有发布返回空，损坏或存储失败返回错误。
   */
  [[nodiscard]] virtual ServiceResult<std::optional<PublishedServiceRules>>
  LoadPublished() = 0;
};

/** @brief 业务用例共享的只读规则加载接口。 */
class ServiceRulesLoader {
 public:
  virtual ~ServiceRulesLoader() = default;

  /** @brief 返回完成结构、边界、排序、唯一性和 UTF-8 校验的不可变规则。 */
  [[nodiscard]] virtual ServiceResult<PublishedServiceRules> Load() const = 0;
};

/**
 * @brief 在规则进入业务事务前执行与离线发布器独立的 C++ 防御性校验。
 * @note 运行时只读 MySQL published 载荷，不读取 Python 或 research 文件。
 */
class ServiceRulesService final : public ServiceRulesLoader {
 public:
  /** @brief 注入生命周期长于本服务的持久化读取端口。 */
  explicit ServiceRulesService(ServiceRulesReader& reader) noexcept;

  /** @brief 加载唯一版本；缺失配置返回未就绪，畸形配置返回内部错误。 */
  [[nodiscard]] ServiceResult<PublishedServiceRules> Load() const override;

 private:
  ServiceRulesReader& reader_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVICE_RULES_H_
