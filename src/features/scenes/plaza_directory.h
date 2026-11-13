#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_DIRECTORY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_DIRECTORY_H_

/**
 * @file plaza_directory.h
 * @brief 按频道分配有空位的广场，并按玩家路由独立场景广播。
 */

#include <memory>

#include "features/scenes/plaza_coordinator.h"

namespace bf::runtime {
/** @brief world executor 独占的广场实例目录；满员创建新实例，空实例回收。 */
class PlazaDirectory final {
 public:
  /** @brief 设置每个实例的真人席位上限，符合原生广场 16 席边界。 */
  explicit PlazaDirectory(std::uint16_t capacity = 16U);
  /** @brief 同频道优先保留原广场，再找空位，全部满员时创建新广场。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> Enter(
      application::PlayerMenuSnapshot snapshot, std::uint32_t channel = 0U,
      protocol::ScenePartyState party = {.leader = true});
  [[nodiscard]] PlazaCoordinatorResult UpdateParty(
      domain::PlayerId player, protocol::ScenePartyState party);
  [[nodiscard]] PlazaCoordinatorResult RecruitmentText(
      domain::PlayerId player, std::u16string text,
      std::span<const domain::PlayerId> blocked,
      std::optional<std::uint8_t> recipient_slot = std::nullopt);
  /** @brief 从所属实例离开；最后一人离开时释放该实例。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> Leave(
      domain::PlayerId player);
  /** @brief 移动仅转发到发送者所在广场。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  HandleMovement(domain::PlayerId player, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  RelayMinigame(domain::PlayerId player,
                const protocol::LobbyMinigameSync& sync) const;
  /** @brief 附近聊天仅在所属实例广播，保留双向屏蔽过滤。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> HandleChat(
      domain::PlayerId player, const domain::UserMessage& message,
      std::span<const domain::PlayerId> blocked_peers) const;
  /** @brief 更新所属实例中的一致性玩家资料。 */
  [[nodiscard]] application::ServiceResult<bool> UpdatePlayerSnapshot(
      application::PlayerMenuSnapshot snapshot);
  /** @brief 向所属实例同步换装。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  EquipmentChanged(application::PlayerMenuSnapshot snapshot);

  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  CoupleChanged(const application::PlayerMenuSnapshot& snapshot);
  /** @brief 向所属实例同步家族徽章。 */
  [[nodiscard]] PlazaCoordinatorResult ClanMarkChanged(domain::PlayerId player,
                                                       std::uint32_t mark);
  /** @brief 判断玩家是否属于任一广场。 */
  [[nodiscard]] bool Contains(domain::PlayerId player) const noexcept;
  /** @brief 返回全部广场的在线成员数。 */
  [[nodiscard]] std::size_t MemberCount() const noexcept;
  /** @brief 返回当前非空广场数量。 */
  [[nodiscard]] std::size_t SceneCount() const noexcept;
  /** @brief 查询玩家所在场景；离场返回空。 */
  [[nodiscard]] std::optional<protocol::SceneInfo> Scene(
      domain::PlayerId player) const;

  /** @brief 已提交的图标偏好同步为完整 973，保留席位及准备状态。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  LevelIconChanged(application::PlayerMenuSnapshot snapshot);

 private:
  /** @brief 独立场景及其频道归属。 */
  struct Instance final {
    std::uint32_t channel;  ///< 原生频道编号。
    std::unique_ptr<PlazaCoordinator>
        coordinator;  ///< 独立席位、房主和移动缓存。
  };

  [[nodiscard]] PlazaCoordinator* Find(domain::PlayerId player);
  [[nodiscard]] const PlazaCoordinator* Find(domain::PlayerId player) const;
  std::uint16_t capacity_;
  std::map<std::uint32_t, Instance> instances_;
  std::map<domain::PlayerId, std::uint32_t> membership_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_DIRECTORY_H_
