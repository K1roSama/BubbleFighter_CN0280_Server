#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_SINGLE_PLAY_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_SINGLE_PLAY_H_

/**
 * @file local_single_play.h
 * @brief 原15客户端本地关卡的独立加载、报告、等待与重开生命周期。
 */

#include <array>
#include <span>
#include <string_view>

#include "features/practice/local_horde_tutor.h"

namespace bf::runtime {
/** @brief 当前原始24关配置，客户端本地目标执行；stage_key不是Difficulty。 */
struct SinglePlayMission final {
  std::string_view mission_key;
  std::string_view directory;
  std::uint32_t mission_id;
  std::uint32_t difficulty;
  std::uint32_t stage_key;
  std::uint32_t duration_ms;
};

[[nodiscard]] std::span<const SinglePlayMission> SinglePlayMissions() noexcept;
[[nodiscard]] const SinglePlayMission* FindSinglePlayMission(
    std::string_view key) noexcept;

/** @brief 独立单人私有场景；没有正式对局、资产或竞技统计权限。 */
class LocalSinglePlayCoordinator final {
 public:
  using Time = std::chrono::steady_clock::time_point;
  [[nodiscard]] application::ServiceResult<LocalTutorialUpdate> Begin(
      application::PlayerMenuSnapshot snapshot, const protocol::Packet& request,
      const domain::MatchId& id, Time now);
  [[nodiscard]] application::ServiceResult<LocalTutorialUpdate> Handle(
      domain::PlayerId player, const protocol::Packet& packet, Time now);
  [[nodiscard]] LocalTutorialUpdate Disconnect(domain::PlayerId player);
  bool Acknowledge(domain::PlayerId player, const domain::MatchId& id,
                   bool begin);
  bool BeginFailed(domain::PlayerId player, const domain::MatchId& id);
  [[nodiscard]] bool Contains(domain::PlayerId player) const;
  [[nodiscard]] bool Waiting(domain::PlayerId player) const;
  [[nodiscard]] std::optional<domain::MatchId> SessionId(
      domain::PlayerId player) const;
  [[nodiscard]] std::optional<Time> Deadline() const;
  [[nodiscard]] std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>>
  Tick(Time now);

 private:
  /** @brief 单人SQL门控阶段，终局保留原Room4等待。 */
  enum class Stage { kLoading, kPlaying, kFinishing, kWaiting };

  /** @brief 冻结精确关卡、身份和首次终态；不保存伪造本地NPC战斗事实。 */
  struct Session final {
    domain::MatchId id;
    const SinglePlayMission* mission;
    std::vector<std::uint8_t> menu;
    std::vector<std::uint8_t> scene;
    Time loading_at;
    std::optional<Time> playing_at;
    Stage stage{Stage::kLoading};
    bool committed{false};
    bool object{false};
    bool assets{false};
    bool loaded{false};
    bool leave_after_finish{false};
    std::optional<LocalTutorialUpdate> terminal;
    std::vector<application::SinglePlayClearView> clears;
  };

  [[nodiscard]] LocalTutorialUpdate Finish(domain::PlayerId player,
                                           Session& session, bool clear,
                                           std::string reason, Time now,
                                           bool stay_in_room, bool deliver);
  std::map<domain::PlayerId, Session> sessions_;
  std::map<domain::PlayerId, Session>
      waiting_rollback_;  ///< 待确认重开失败时保留原Room4。
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_SINGLE_PLAY_H_
