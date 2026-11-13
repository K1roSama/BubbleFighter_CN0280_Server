#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_

/**
 * @file character_creation.h
 * @brief 定义首次昵称检查与角色创建用例的事务端口和公开结果。
 */

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "application/error.h"
#include "application/identifier.h"
#include "features/characters/character.h"
#include "features/characters/nickname.h"

namespace bf::application {

/** @brief 事务内锁定的玩家建角状态快照。 */
struct CharacterCreationProfileSnapshot final {
  domain::PlayerId player_id;        ///< 被锁定的玩家。
  bool is_character_created{false};  ///< 是否已经原子完成建角。
  std::string nickname;              ///< 当前展示昵称。
  std::optional<domain::InitialFighter>
      initial_fighter;  ///< 已完成时的初始角色。
};

/** @brief 事务内读取的已启用初始物品模板。 */
struct StarterCatalogItem final {
  domain::ItemId item_id;                ///< 物品模板编号。
  std::optional<std::string> slot_code;  ///< 可装备模板的槽位；不可装备时为空。
};

/**
 * @brief 一次角色创建数据库事务所需的最小仓储端口。
 * @note 实现必须独占一个连接和事务；除 `commit` 外所有方法禁止自行提交。
 * @note 未成功提交时析构必须回滚。任何 row、事务或连接对象都不得越过此接口。
 */
class CharacterCreationTransaction {
 public:
  virtual ~CharacterCreationTransaction() = default;

  /** @brief 锁定玩家资料父记录，使同一玩家的并发创建严格串行。 */
  [[nodiscard]] virtual ServiceResult<CharacterCreationProfileSnapshot>
  LockProfile(domain::PlayerId player_id) = 0;

  /** @brief 在当前事务中判断规范化昵称是否已被另一位完成建角的玩家占用。 */
  [[nodiscard]] virtual ServiceResult<bool> NicknameKeyInUse(
      std::string_view nickname_key, domain::PlayerId excluding_player_id) = 0;

  /** @brief 锁定并返回已发布、已启用的物品模板；缺失或停用应返回明确错误。 */
  [[nodiscard]] virtual ServiceResult<StarterCatalogItem>
  LockEnabledStarterItem(domain::ItemId item_id) = 0;

  /**
   * @brief 保证玩家持有一件永久初始物品并返回实例 ID。
   * @note 已存在时复用实例；新发放时必须在同一事务追加 `starter` 库存流水。
   */
  [[nodiscard]] virtual ServiceResult<domain::ItemInstanceId> EnsureStarterItem(
      domain::PlayerId player_id, domain::ItemId item_id) = 0;

  /** @brief 在零号配装页用同一玩家的库存实例替换指定槽位。 */
  [[nodiscard]] virtual std::optional<ServiceError> EquipItem(
      domain::PlayerId player_id, std::string_view slot_code,
      domain::ItemInstanceId item_instance_id) = 0;

  /**
   * @brief 在全部初始资产和装备成功后写入昵称、唯一键、初始角色及完成标志。
   * @note 唯一约束竞争必须映射为 `kConflict`，不能向上泄漏 SQL 文本。
   */
  [[nodiscard]] virtual std::optional<ServiceError> CompleteProfile(
      domain::PlayerId player_id, const domain::Nickname& nickname,
      domain::InitialFighter fighter) = 0;

  /** @brief 提交完整事务；返回后不能再使用该事务执行仓储操作。 */
  [[nodiscard]] virtual std::optional<ServiceError> Commit() = 0;
};

/** @brief 创建角色事务的工厂端口，由数据库工作线程上的基础设施实现。 */
class CharacterCreationUnitOfWork {
 public:
  virtual ~CharacterCreationUnitOfWork() = default;

  /** @brief 开始一个独占连接的事务；队列/连接超限必须返回可识别错误。 */
  [[nodiscard]] virtual ServiceResult<
      std::unique_ptr<CharacterCreationTransaction>>
  Begin() = 0;
};

/** @brief C2S31 名称预检用例输入。 */
struct CheckNicknameCommand final {
  domain::PlayerId player_id;  ///< 当前建角玩家，用于排除自己的已提交昵称。
  std::string nickname;        ///< 客户端提供、尚未信任的 UTF-8 昵称。
};

/** @brief 名称预检成功后返回的原文与稳定唯一键。 */
struct NicknameCheckResult final {
  std::string nickname;      ///< 保留大小写的展示昵称。
  std::string nickname_key;  ///< 数据库唯一键。
};

/** @brief C2S32 首次角色创建用例输入。 */
struct CreateCharacterCommand final {
  domain::PlayerId player_id;   ///< 已认证且处于建角阶段的玩家。
  std::string nickname;         ///< 客户端发送的展示昵称。
  std::uint32_t fighter_id{0};  ///< 不可信协议角色 DWORD；仅允许 1、2、11、12。
};

/** @brief 角色创建成功的稳定业务结果。 */
struct CharacterCreationResult final {
  domain::PlayerId player_id;          ///< 完成建角的玩家。
  std::string nickname;                ///< 最终展示昵称。
  domain::InitialFighter fighter;      ///< 最终初始角色。
  std::size_t starter_item_count{0U};  ///< 本次规则要求存在的初始物品数。
  bool already_completed{false};       ///< 同参数重试是否命中已提交结果。
};

/** @brief 网关运行时依赖的昵称检查与首次角色创建用例接口。 */
class CharacterCreationUseCases {
 public:
  virtual ~CharacterCreationUseCases() = default;

  /** @brief 校验候选昵称并确认当前数据库快照中可用。 */
  [[nodiscard]] virtual ServiceResult<NicknameCheckResult> CheckNickname(
      const CheckNicknameCommand& command) const = 0;

  /** @brief 在一个事务中完成首次角色、资产和装备创建。 */
  [[nodiscard]] virtual ServiceResult<CharacterCreationResult> CreateCharacter(
      const CreateCharacterCommand& command) const = 0;
};

/**
 * @brief
 * 协调昵称规则、资料锁、目录复核、初始资产、装备和提交的角色创建应用服务。
 * @note 服务本身不持有连接；调用方应在数据库工作池上执行一次完整方法调用。
 */
class CharacterCreationService final : public CharacterCreationUseCases {
 public:
  /** @brief 注入事务工厂；其生命周期必须覆盖本服务。 */
  explicit CharacterCreationService(
      CharacterCreationUnitOfWork& unit_of_work) noexcept;

  /** @brief 校验并查询昵称是否可用，供原版 C2S31/S2C15 交互使用。 */
  [[nodiscard]] ServiceResult<NicknameCheckResult> CheckNickname(
      const CheckNicknameCommand& command) const override;

  /**
   * @brief 原子完成角色创建；相同参数重试成功，不同参数重复创建返回冲突。
   * @note 目录预检先于任何发放；任一步失败都由事务 RAII
   * 回滚，完成标志最后写入。
   */
  [[nodiscard]] ServiceResult<CharacterCreationResult> CreateCharacter(
      const CreateCharacterCommand& command) const override;

 private:
  CharacterCreationUnitOfWork& unit_of_work_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CHARACTERS_CHARACTER_CREATION_H_
