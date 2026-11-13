#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_SPACES_FLOW_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_SPACES_FLOW_H_

/**
 * @file social_spaces_flow.h
 * @brief 校验活动会话、限制请求频率并按提交顺序推送 NM 通讯快照。
 */
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include "features/social/social_spaces.h"
#include "sessions/session_registry.h"

namespace bf::runtime {
/** @brief 发布顺序由互斥保护；活动会话资格不能仅依赖客户端身份字段。 */
class SocialSpacesFlow final {
 public:
  SocialSpacesFlow(application::SocialSpacesStore& store,
                   SessionRegistry& sessions)
      : store_(store), sessions_(sessions) {}

  [[nodiscard]] std::optional<application::ServiceError> Handle(
      domain::PlayerId, SessionHandle, std::span<const std::uint8_t>,
      std::chrono::steady_clock::time_point);

 private:
  application::SocialSpacesStore& store_;
  SessionRegistry& sessions_;
  std::mutex mutex_;
  std::map<domain::PlayerId, std::deque<std::chrono::steady_clock::time_point>>
      quota_;
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_SOCIAL_SPACES_FLOW_H_
