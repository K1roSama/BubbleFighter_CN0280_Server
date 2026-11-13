#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_COUPLE_FLOW_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_COUPLE_FLOW_H_

/**
 * @file couple_flow.h
 * @brief 会话代次绑定的情侣邀请、同意和关系刷新流程。
 */
#include <functional>
#include <map>
#include <mutex>

#include "features/lobby/player_menu_snapshot.h"
#include "features/social/couple_service.h"
#include "features/social/social_query.h"
#include "sessions/session_registry.h"

namespace bf::runtime {
/** @brief 按接收会话绑定的通知与提交后资料变化。 */
struct CoupleFlowResult final {
  SocialCommandResult messages;
  std::vector<application::PlayerMenuSnapshot> changed;
  std::map<domain::PlayerId, SessionHandle> expected_sessions;
};

/** @brief 有时限且不可跨重连继承的情侣同意流程。 */
class CoupleFlow final {
 public:
  CoupleFlow(application::CoupleStore& store,
             application::PlayerMenuSnapshotLoader& profiles,
             application::SocialSnapshotLoader& social,
             SessionRegistry& sessions)
      : store_(store),
        profiles_(profiles),
        social_(social),
        sessions_(sessions) {}

  [[nodiscard]] application::ServiceResult<CoupleFlowResult> Handle(
      domain::PlayerId actor, SessionHandle session,
      const protocol::Packet& packet, std::chrono::steady_clock::time_point now,
      std::chrono::system_clock::time_point utc,
      const std::function<void(CoupleFlowResult&)>& publish = {});
  [[nodiscard]] CoupleFlowResult Disconnect(domain::PlayerId player,
                                            SessionHandle handle);
  /** @brief 改名后取消旧名字邀请，并同步伴侣名称；与情侣提交使用同一顺序锁。 */
  [[nodiscard]] application::ServiceResult<CoupleFlowResult> IdentityChanged(
      domain::PlayerId player, SessionHandle handle,
      const std::function<void(CoupleFlowResult&)>& publish);

 private:
  [[nodiscard]] application::ServiceResult<CoupleFlowResult> HandleLocked(
      domain::PlayerId actor, SessionHandle session,
      const protocol::Packet& packet, std::chrono::steady_clock::time_point now,
      std::chrono::system_clock::time_point utc);

  /** @brief 绑定双方在线会话及三分钟截止时间的待确认邀请。 */
  struct Invitation final {
    application::CoupleFormCommand command;
    SessionHandle inviter_session;
    SessionHandle recipient_session;
    std::chrono::steady_clock::time_point expires;
  };

  CoupleFlowResult CancelLocked(domain::PlayerId player, SessionHandle handle);
  application::ServiceResult<CoupleFlowResult> Refresh(
      CoupleFlowResult result, const application::CoupleChange& change);
  application::CoupleStore& store_;
  application::PlayerMenuSnapshotLoader& profiles_;
  application::SocialSnapshotLoader& social_;
  SessionRegistry& sessions_;
  std::mutex mutex_;
  std::map<domain::PlayerId, Invitation> invitations_;  // keyed by recipient
  std::map<domain::PlayerId, std::chrono::steady_clock::time_point>
      last_invite_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_COUPLE_FLOW_H_
