#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_HORDE_TUTOR_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_HORDE_TUTOR_H_

/**
 * @file local_horde_tutor.h
 * @brief
 * 原生客户端本地103教程的会话编排；完成仅记录CLIENT_REPORTED，不产生正式对局。
 */

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "features/battle/combat_relay.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/practice/tutorial_session.h"

namespace bf::runtime {

/** @brief 仅本地教程的网关阶段；无普通房间等待或成绩阶段。 */
enum class LocalTutorialPhase { kLoading, kPlaying, kMenu, kSingleWaiting };

/**
 * @brief 本地会话变更和只发给认证本人的候选响应。
 * @note persistence存在时，调用者必须先提交SQL并按UUID
 * acknowledge，再发布phase/deliveries；
 *       提交失败不得发送候选成功消息。此类型没有正式对局结算或奖励能力。
 */
struct LocalTutorialUpdate final {
  std::vector<CombatDelivery> deliveries;
  std::optional<LocalTutorialPhase> phase;
  std::optional<domain::MatchId>
      release_owner;  ///< Room4退出仍绑定原网关与UUID发布，随后撤销槽owner。
  std::optional<std::variant<application::TutorialSessionStart,
                             application::TutorialSessionFinish>>
      persistence;
};

/**
 * @brief 世界线程独占、每名认证玩家一个私有教程槽的生命周期编排。
 * @note
 * GameSession+28405=1使所有0x80xx动作走本机队列；这里仅严格检查并丢弃误入TCP的
 *       relay数据，绝不构造NPC、登记击杀或把本机完成声明描述为服务端独立验证。
 */
class LocalHordeTutorCoordinator final {
 public:
  using Time = std::chrono::steady_clock::time_point;

  /** @brief
   * 接受唯一已启用的原生107；返回SQL开始命令及其提交后才能发送的112、510。 */
  [[nodiscard]] application::ServiceResult<LocalTutorialUpdate> Begin(
      application::PlayerMenuSnapshot snapshot, const protocol::Packet& request,
      const domain::MatchId& session_id, Time now);
  /** @brief
   * 校验本人加载/单波声明和directTCP704复活；业务不匹配返回ServiceError，畸形wire抛ProtocolError。
   */
  [[nodiscard]] application::ServiceResult<LocalTutorialUpdate> Handle(
      domain::PlayerId player, const protocol::Packet& packet, Time now);
  /** @brief 中断一次且不向断开的连接发送消息；已存在的终局命令不被改写。 */
  [[nodiscard]] LocalTutorialUpdate Disconnect(
      domain::PlayerId player, std::string reason = "disconnect");
  /** @brief SQL提交确认；只处理匹配的玩家、UUID和开始/结束方向。 */
  bool Acknowledge(domain::PlayerId player, const domain::MatchId& session_id,
                   bool is_begin);
  /** @brief 开始事务失败只删除同UUID且尚未提交的槽，不能移除后续的新会话。 */
  bool BeginFailed(domain::PlayerId player, const domain::MatchId& session_id);
  /** @brief 包括等待SQL的开始/结束槽，防止重复进入或重连冒用旧声明。 */
  [[nodiscard]] bool Contains(domain::PlayerId player) const;
  /** @brief 取得仍由编排持有的稳定UUID。 */
  [[nodiscard]] std::optional<domain::MatchId> SessionId(
      domain::PlayerId player) const;
  /** @brief 中断90秒加载或交付待发clear ack；绝无180秒或普通对局Playing超时。
   */
  [[nodiscard]] std::vector<std::pair<domain::PlayerId, LocalTutorialUpdate>>
  Tick(Time now);
  /** @brief
   * 最早加载截止/已声明clear的兼容ack时刻；没有Playing过期或终局SQL计时器。 */
  [[nodiscard]] std::optional<Time> Deadline() const;

 private:
  /** @brief clear_ack仍是本地Playing；finishing槽只能等待SQL或重试相同的空905。
   */
  enum class Stage { kLoading, kPlaying, kClearAck, kFinishing };

  /** @brief 不保存NPC或战斗成绩，只保存本地会话的加载/声明与事务身份。 */
  struct Session final {
    domain::MatchId id;
    std::vector<std::uint8_t> menu_scene;
    Time loading_started;
    Stage stage{Stage::kLoading};
    bool begin_committed{false};
    bool object_received{false};
    bool assets_received{false};
    bool loading_received{false};
    bool early_wave_start{false};
    std::optional<Time> wave_started;
    std::optional<Time> clear_due;
    std::optional<Time> clear_ack_at;
    std::vector<std::uint8_t> plan;
    std::optional<LocalTutorialUpdate> terminal;
  };

  /** @brief 已接收一次有效声明后按兼容SERVER POLICY发一次ack；并非NPC死亡验证。
   */
  [[nodiscard]] static LocalTutorialUpdate AdvanceClear(domain::PlayerId player,
                                                        Session& session,
                                                        Time now);
  /** @brief
   * 冻结一次结束命令；只有reported_clear空905重试会重新返回相同候选结果。 */
  [[nodiscard]] static LocalTutorialUpdate Finish(domain::PlayerId player,
                                                  Session& session,
                                                  bool reported_clear,
                                                  std::string reason,
                                                  bool return_to_menu);
  std::map<domain::PlayerId, Session> sessions_;
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_LOCAL_HORDE_TUTOR_H_
