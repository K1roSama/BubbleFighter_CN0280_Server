#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_PREPARATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_PREPARATION_H_

/**
 * @file match_preparation.h
 * @brief 定义正式开局前读取发布版本、成员配装并形成运行时冻结快照的只读用例。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/match_result.h"
#include "features/inventory/item_attribute.h"

namespace bf::application {

inline constexpr std::size_t kPreparedWeaponBarCount =
    5U;  ///< 原生切枪协议固定栏位数。

/** @brief world executor 从串行房间状态冻结的一名开局成员。 */
struct MatchPreparationParticipantInput final {
  domain::PlayerId player_id;         ///< 已认证且仍属于房间的玩家。
  std::uint8_t seat_index{0U};        ///< 零基稳定席位。
  std::uint8_t team_code{0U};         ///< 普通模式队伍 1 或 2。
  std::uint32_t actor_object_id{0U};  ///< 经 C2S709 验证的客户端对象 ID。
};

/** @brief 一次开局准备读取的完整不可变输入。 */
struct MatchPreparationRequest final {
  domain::RoomId room_id;     ///< 来源房间标识。
  std::uint32_t mode_id{0U};  ///< 房间当前模式。
  std::vector<MatchPreparationParticipantInput>
      participants;  ///< 完整开局名单。
  std::vector<std::uint32_t>
      restricted_items;  ///< 等待房间冻结的禁用模板列表。
};

/** @brief 数据库快照中一名成员的原生武器栏结果。 */
struct PreparedMatchParticipant final {
  MatchPreparationParticipantInput
      identity;  ///< 房间冻结的身份、席位、队伍与对象。
  std::array<std::optional<std::uint32_t>, kPreparedWeaponBarCount>
      weapon_templates;  ///< 由已发布目录、当前预设及共享装备解析的原生模板。
  bool lucci_pouch{false};    ///< 生效的 100256 福袋授权击爆金币掉落。
  bool chocolate_box{false};  ///< 生效的 100257 巧克力盒授权击爆恢复道具。
  std::uint32_t fighter{1U};  ///< 同事务读取的角色装备或初始角色。
  std::vector<std::uint32_t>
      equipment_items;  ///< 同事务有效配装，用于原生服饰属性。
  std::vector<domain::EquippedItemAttributes>
      equipment_attributes;  ///< 同事务有效实例属性；不读取客户端自报值。
};

/** @brief 开局事务及战斗安装共同使用的已发布版本和全员配装快照。 */
struct PreparedMatch final {
  domain::RoomId room_id;                    ///< 来源房间标识。
  std::uint32_t mode_id{0U};                 ///< 已明确支持的普通模式。
  std::int64_t configuration_version_id{0};  ///< 配装所用已发布目录版本。
  std::int64_t reward_rules_version_id{0};   ///< 终局计算使用的奖励规则版本。
  domain::MatchScoreModel score_model{
      domain::MatchScoreModel::kTeamPangs};  ///< 计分模型。
  bool ordinary_respawn_allowed{true};       ///< 是否允许普通 C2S704 复活链。
  std::vector<PreparedMatchParticipant>
      participants;  ///< 按 player_id 严格升序。
};

/** @brief 在一个只读一致性事务中读取开局发布版本与全员配装的仓储端口。 */
class MatchPreparationReader {
 public:
  virtual ~MatchPreparationReader() = default;

  /** @brief 读取并解析全部输入玩家；任一玩家或发布版本缺失都返回显式错误。 */
  [[nodiscard]] virtual ServiceResult<PreparedMatch> Load(
      const MatchPreparationRequest& request) = 0;
};

/** @brief world executor 依赖的开局准备业务接口。 */
class MatchPreparationExecutor {
 public:
  virtual ~MatchPreparationExecutor() = default;

  /** @brief 校验房间输入、调用仓储并复核不可变返回契约。 */
  [[nodiscard]] virtual ServiceResult<PreparedMatch> Prepare(
      MatchPreparationRequest request) const = 0;
};

/**
 * @brief 对普通击爆模式建立明确计分语义并保护数据库适配器边界。
 * @note 专用目标玩法不会被猜成普通 pang 结算；未恢复模式返回 kInvalidState。
 */
class MatchPreparationService final : public MatchPreparationExecutor {
 public:
  /** @brief 注入生命周期覆盖本服务的只读仓储。 */
  explicit MatchPreparationService(MatchPreparationReader& reader) noexcept;

  /** @brief 规范化参与者、确定普通模式计分模型并复核仓储结果。 */
  [[nodiscard]] ServiceResult<PreparedMatch> Prepare(
      MatchPreparationRequest request) const override;

 private:
  MatchPreparationReader& reader_;  ///< MySQL 一致性读取端口，不由本对象拥有。
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_PREPARATION_H_
