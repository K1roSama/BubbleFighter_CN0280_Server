#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_COORDINATOR_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_COORDINATOR_H_

/**
 * @file plaza_coordinator.h
 * @brief 定义自由广场成员、快照和移动广播的串行内存协调器。
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "features/chat/user_message.h"
#include "features/lobby/player_menu_snapshot.h"
#include "protocol/lobby/lobby_minigame_messages.h"
#include "protocol/scenes/plaza_messages.h"
#include "protocol/scenes/scene_messages.h"
#include "protocol/scenes/scene_session_messages.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/** @brief 广场命令处理后要求目标 Session 应用的场景阶段。 */
enum class PlazaSessionStage : std::uint8_t {
  kOutsidePlaza,
  kPlaza,
};

/** @brief 一名玩家的广场场景阶段更新。 */
struct PlazaSessionUpdate final {
  domain::PlayerId player_id;  ///< SessionRegistry 可定位的玩家。
  PlazaSessionStage stage{
      PlazaSessionStage::kPlaza};  ///< 操作完成后的目标阶段。

  /** @brief 按玩家和目标广场阶段比较。 */
  bool operator==(const PlazaSessionUpdate&) const = default;
};

/** @brief 一条已确定玩家目标、拥有完整负载的广场消息。 */
struct PlazaDelivery final {
  domain::PlayerId recipient;  ///< 接收消息的已认证玩家。
  OutboundMessage message;     ///< 可安全移动到 Session 顺序发送队列的消息。

  /** @brief 按接收玩家和完整出站消息比较。 */
  bool operator==(const PlazaDelivery&) const = default;
};

/** @brief 广场操作产生的成员事件、定向消息和会话阶段变化。 */
struct PlazaCoordinatorResult final {
  /** @brief 一次广场命令提交后产生的稳定成员事件。 */
  enum class Event : std::uint8_t {
    kNoChange,
    kMemberEntered,
    kMemberRefreshed,
    kMemberLeft,
    kMovementBroadcast,
    kChatBroadcast,
  };

  Event event{Event::kNoChange};          ///< 本次权威成员状态变化。
  std::vector<PlazaDelivery> deliveries;  ///< 按每名接收者观察顺序排列。
  std::vector<PlazaSessionUpdate>
      session_updates;  ///< Session 应用的阶段变化。
};

/**
 * @brief 在单一 executor 上独占自由广场成员与最近有效移动的同步协调器。
 * @note 本类不加锁、不查数据库、不持有
 * Session；调用方必须串行提交进入、离开、换装资料刷新 和
 * C2S511。成员资料及移动均按值保存，返回消息拥有自己的负载。
 */
class PlazaCoordinator final {
 public:
  /** @brief 创建固定真人容量和场景编号的广场；糯米由客户端创建，不占场景席位。
   */
  explicit PlazaCoordinator(std::uint16_t capacity = 16U,
                            std::uint32_t scene_id = 1U);

  PlazaCoordinator(const PlazaCoordinator&) = delete;
  PlazaCoordinator& operator=(const PlazaCoordinator&) = delete;

  /**
   * @brief 进入或幂等刷新广场成员，并生成旧成员增量、本人的完整 S2C112
   * 和旧成员姿态。
   * @note 新成员使用最低可用席位；编码失败不提交任何成员变化。
   */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> Enter(
      application::PlayerMenuSnapshot snapshot,
      protocol::ScenePartyState party = {.leader = true});

  /** @brief 同实例同步权威组队状态；保留席位、移动和装备。 */
  [[nodiscard]] PlazaCoordinatorResult UpdateParty(
      domain::PlayerId player, protocol::ScenePartyState party);
  /** @brief 缓存招募文字并只向同实例未屏蔽玩家广播，供后来者补发。 */
  [[nodiscard]] PlazaCoordinatorResult RecruitmentText(
      domain::PlayerId player, std::u16string text,
      std::span<const domain::PlayerId> blocked,
      std::optional<std::uint8_t> recipient_slot = std::nullopt);

  /**
   * @brief 移除成员、清除其姿态并向剩余成员广播 S2C511/S2C501。
   * @note 不在广场的玩家幂等返回 outside 阶段，不制造清席消息。
   */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> Leave(
      domain::PlayerId player_id);

  /** @brief 校验 C2S511，保存最近姿态并向同广场其他成员广播 S2C514。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  HandleMovement(domain::PlayerId player_id,
                 std::span<const std::uint8_t> body);

  /**
   * @brief 以权威席位和昵称广播附近聊天，并跳过与发送者存在双向屏蔽关系的目标。
   * @note blocked_peers
   * 必须来自已认证社交查询；调用方不能用客户端提交的玩家列表。
   */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult> HandleChat(
      domain::PlayerId player_id, const domain::UserMessage& message,
      std::span<const domain::PlayerId> blocked_peers) const;

  /** @brief 只替换现有成员的一致性资料快照；成员不存在或 ID 不一致时返回错误。
   */
  [[nodiscard]] application::ServiceResult<bool> UpdatePlayerSnapshot(
      application::PlayerMenuSnapshot snapshot);

  /** @brief 已提交换装刷新广场成员，并将原生外观和装备广播给同行。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  EquipmentChanged(application::PlayerMenuSnapshot snapshot);

  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  CoupleChanged(const application::PlayerMenuSnapshot& snapshot);

  /** @brief 刷新已提交的家族徽章并广播原生 991，不改变准备或战斗状态。 */
  [[nodiscard]] PlazaCoordinatorResult ClanMarkChanged(domain::PlayerId player,
                                                       std::uint32_t mark);

  /** @brief 把 C2S572 小游戏动作转发给同广场其他成员（S2C572，9763C0 以玩家 ID
   * 定位 LmoGamer）。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  RelayMinigame(domain::PlayerId player_id,
                const protocol::LobbyMinigameSync& sync) const;

  /** @brief 返回玩家当前是否属于广场。 */
  [[nodiscard]] bool Contains(domain::PlayerId player_id) const noexcept;
  /** @brief 返回当前广场成员数。 */
  [[nodiscard]] std::size_t MemberCount() const noexcept;
  /** @brief 复制当前人数已经更新的权威广场 SceneInfo。 */
  [[nodiscard]] protocol::SceneInfo Scene() const;

  /** @brief 已提交的图标偏好同步为完整 973，保留席位及准备状态。 */
  [[nodiscard]] application::ServiceResult<PlazaCoordinatorResult>
  LevelIconChanged(application::PlayerMenuSnapshot snapshot);

 private:
  /** @brief 广场内一名成员的稳定席位、资料和最近一次有效移动。 */
  struct Member final {
    std::uint8_t seat{0U};                     ///< 最低可用稳定席位。
    application::PlayerMenuSnapshot snapshot;  ///< 一致性资料值快照。
    std::optional<protocol::SceneMovementRequest>
        movement;  ///< 供后来者恢复远端姿态。
    protocol::ScenePartyState party;
    std::u16string recruitment_text;
    std::vector<domain::PlayerId> recruitment_blocked;
  };

  /** @brief 按玩家强类型标识稳定排序的广场成员容器。 */
  using MemberMap = std::map<domain::PlayerId, Member>;

  /** @brief 构造 S2C112 时不可交换的房主与本地玩家选择。 */
  struct SnapshotSelection final {
    domain::PlayerId host;      ///< 当前稳定广场房主。
    domain::PlayerId selected;  ///< 接收快照的本地玩家。
  };

  [[nodiscard]] protocol::SceneInfo SceneFor(const MemberMap& members) const;
  [[nodiscard]] std::vector<domain::PlayerId> OrderedPlayers(
      const MemberMap& members) const;
  [[nodiscard]] std::uint8_t HostSeat(const MemberMap& members,
                                      domain::PlayerId host) const;
  [[nodiscard]] protocol::SceneSessionSnapshot SessionSnapshot(
      const MemberMap& members, SnapshotSelection selection) const;

  std::uint16_t human_capacity_{0U};
  protocol::SceneInfo base_scene_;
  MemberMap members_;
  domain::PlayerId host_player_id_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SCENES_PLAZA_COORDINATOR_H_
