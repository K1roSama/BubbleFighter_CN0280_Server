#ifndef BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAYER_MENU_SNAPSHOT_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAYER_MENU_SNAPSHOT_H_

/**
 * @file player_menu_snapshot.h
 * @brief 定义登录后菜单资料、钱包、库存与配装的一致性只读用例。
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"
#include "features/characters/character.h"
#include "features/inventory/item_attribute.h"

namespace bf::application {

/** @brief 登录后资料响应需要的等级与经验快照。 */
struct PlayerProgressView final {
  std::int64_t level{0};       ///< 当前显示等级；由已发布奖励规则从经验确定。
  std::int64_t experience{0};  ///< 当前累计经验；协议适配层负责检查 32 位范围。
  std::int64_t level_base{0};  ///< 当前等级起始经验。
  std::int64_t level_next{0};  ///< 下一等级经验阈值。
};

/** @brief 三种不可互换的钱包余额快照。 */
struct PlayerWalletView final {
  std::int64_t lucci{0};            ///< 普通金币余额。
  std::int64_t bubble_cash{0};      ///< 商城点券余额。
  std::int64_t treasure_bubble{0};  ///< 抽取资源余额；不得并入 bubble_cash。
};

/** @brief 持久化宠物属性，内部阶段零对应原版第一阶段。 */
struct PetGrowthView final {
  std::uint32_t level{0U};       ///< 已完成升级的次数，0–2。
  std::uint32_t experience{0U};  ///< 当前阶段经验，0–100。
  std::optional<std::chrono::system_clock::time_point>
      fed_until;  ///< 普通粮下次可喂 UTC 时间。
};

/** @brief 一项未过期库存及当前有效配装的状态。 */
struct PlayerItemView final {
  domain::ItemInstanceId item_instance_id;              ///< 持有实例主键。
  domain::ItemId item_id;                               ///< 客户端物品模板 ID。
  std::int64_t quantity{0};                             ///< 正整数持有数量。
  std::chrono::system_clock::time_point acquired_at{};  ///< 资产取得 UTC 时间。
  std::optional<std::chrono::system_clock::time_point>
      expires_at;         ///< 永久物品为空。
  std::string slot_code;  ///< 模板装备槽；不可装备时可为空。
  bool equipped{false};   ///< 是否在读取的配装页生效。
  std::optional<PetGrowthView>
      pet_growth;  ///< 已发生喂食的宠物属性；普通物品为空。
  std::vector<domain::NativeItemAttribute>
      attributes;  ///< 绑定本实例的原生动态属性；未升级时为空。
};

/** @brief 原生 835 中的一页主副武器摘要；四个武器槽独立，完整装备由 806 同步。
 */
struct WeaponPresetView final {
  bool enabled{false};       ///< 是否已通过有效扩张卡开放。
  domain::ItemId primary;    ///< 主武器；空标识表示未装备。
  domain::ItemId secondary;  ///< 副武器；空标识表示未装备。
};

/** @brief 已声明通关的单人教程；时间是本服观察值，不是原服战斗最佳成绩。 */
struct SinglePlayClearView final {
  std::uint32_t stage_key{0U};  ///< 原mission_id + 10 * difficulty_ui_index。
  std::uint32_t observed_seconds{
      0U};  ///< 最短服务器观察Playing耗时，向下取整为秒。
};

/** @brief 已提交的每模式战绩；与菜单其他字段共用一次数据库快照。 */
struct PlayerModeStatisticsView final {
  std::uint32_t mode_id{0U};  ///< 原生模式号。
  std::uint64_t matches{0U};  ///< 已结算场数。
  std::uint64_t wins{0U};     ///< 胜场。
  std::uint64_t losses{0U};   ///< 负场。
  std::uint64_t draws{0U};    ///< 平局。
  std::uint64_t bubbles{0U};  ///< 泡封次数。
  std::uint64_t pangs{0U};    ///< 击爆次数。
  std::uint64_t helps{0U};    ///< 帮助次数。
};

/** @brief
 * 原生可用勋章余额；由真实授勋收入减称号支出，不由总战绩或称号库存推导。 */
struct PlayerMedalProgressView final {
  std::uint32_t medal_key{0U};  ///< 原版 mode * 10000 + medalid 键。
  std::uint64_t count{
      0U};  ///< 同一事务中真实可用余额，编码前检查有符号 DWORD 上限。
};

/** @brief 持久情侣关系的伴侣身份、正式戒指与成立时间。 */
struct CoupleProfileView final {
  domain::PlayerId partner_id;
  std::string partner_nickname;
  std::uint32_t ring_item_id{0U};
  std::chrono::system_clock::time_point since{};
};

/** @brief 单次一致性读取产生的登录后菜单快照。 */
struct PlayerMenuSnapshot final {
  domain::PlayerId player_id;  ///< 快照所属玩家。
  std::string nickname;        ///< 数据库展示昵称，编码为 UTF-8。
  domain::InitialFighter initial_fighter{
      domain::InitialFighter::kDao};  ///< HUD 回退角色。
  PlayerProgressView progress;        ///< 同一数据库快照中的经验等级。
  PlayerWalletView wallets;           ///< 同一数据库快照中的三钱包。
  std::vector<PlayerItemView>
      items;  ///< 仅含有效库存，按 item_instance_id 严格升序。
  std::chrono::system_clock::time_point
      observed_at{};             ///< 过滤期限和计算剩余天数的同一时刻。
  std::uint32_t clan_mark{0U};   ///< 当前家族徽章，未加入家族为零。
  std::uint16_t color_code{0U};  ///< 原版角色颜色，零为默认。
  std::uint8_t active_weapon_preset{
      0U};  ///< 当前有效武器页，扩张卡过期回退基本页。
  std::array<WeaponPresetView, 5U>
      weapon_presets{};    ///< 原版固定读取五条，未显示页保持禁用。
  std::string login_name;  ///< 原生资料窗口判断本人使用的登录名；不含凭据。
  std::string clan_name;   ///< 当前正式战队名称，无战队为空。
  std::uint32_t rating{1'000U};  ///< 已提交的匹配评级。
  std::uint32_t level_icon_type{
      0U};  ///< 原生 UserSlot +184；失去家族身份时回退普通等级。
  std::uint32_t clan_id{
      0U};  ///< 原生 UserSlot +256；只来自当前正式家族成员关系。
  std::vector<PlayerModeStatisticsView>
      mode_statistics;  ///< 按模式严格升序的真实累计战绩。
  std::optional<CoupleProfileView> couple;
  std::optional<std::vector<PlayerMedalProgressView>>
      medal_progress;  ///< 空值为未知；空列表为已读取的零余额，不代表授勋已实现。
  std::vector<PlayerItemView>
      expired_items;  ///< 仅供原仓库expired位显示与808删弃；绝不作为有效配装或战斗资产。
  std::vector<SinglePlayClearView>
      single_play_clears;  ///< 已提交且按stage_key升序；只用于原教程可用性。
};

/**
 * @brief 菜单快照数据库读取端口。
 * @note
 * 实现必须在一个只读一致性事务/快照中取得资料、规则版本、三钱包、库存和当前有效武器页及共享配装，
 *       并排除 expires_at <= now 的物品；不得逐查询借用不同连接拼接可撕裂结果。
 */
class PlayerMenuSnapshotReader {
 public:
  virtual ~PlayerMenuSnapshotReader() = default;

  /** @brief 读取已完成建角玩家；不存在返回空值，存储失败返回显式错误。 */
  [[nodiscard]] virtual ServiceResult<std::optional<PlayerMenuSnapshot>> Load(
      domain::PlayerId player_id,
      std::chrono::system_clock::time_point now) = 0;
};

/** @brief 网关运行时依赖的菜单快照读取用例接口。 */
class PlayerMenuSnapshotLoader {
 public:
  virtual ~PlayerMenuSnapshotLoader() = default;

  /** @brief 读取并验证一个可编码的完整菜单快照。 */
  [[nodiscard]] virtual ServiceResult<PlayerMenuSnapshot> Load(
      domain::PlayerId player_id) const = 0;
};

/**
 * @brief 为 C2S102/803 等登录后入口取得可安全编码的完整菜单数据。
 * @note
 * 本用例不修改过期装备；数据库查询通过有效期条件使过期项不可见，后续清理由独立作业
 *       或写事务负责，避免一个看似只读的刷新请求产生隐式提交。
 */
class PlayerMenuSnapshotService final : public PlayerMenuSnapshotLoader {
 public:
  /** @brief 注入 UTC 时钟和只读仓储；二者生命周期必须覆盖本服务。 */
  PlayerMenuSnapshotService(const SystemClock& clock,
                            PlayerMenuSnapshotReader& reader) noexcept;

  /** @brief 一次读取并复核跨表契约；畸形数据库结果不会进入协议层。 */
  [[nodiscard]] ServiceResult<PlayerMenuSnapshot> Load(
      domain::PlayerId player_id) const override;

 private:
  const SystemClock& clock_;
  PlayerMenuSnapshotReader& reader_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_LOBBY_PLAYER_MENU_SNAPSHOT_H_
