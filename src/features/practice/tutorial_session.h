#ifndef BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TUTORIAL_SESSION_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TUTORIAL_SESSION_H_

/**
 * @file tutorial_session.h
 * @brief 定义原103本地教程的独立报告账本，不赋予竞技成绩或奖励权限。
 */

#include <cstdint>
#include <optional>
#include <string>

#include "application/error.h"
#include "application/identifier.h"
#include "features/battle/match_id.h"

namespace bf::application {
/** @brief 由世界执行器分配UUID并绑定认证玩家及不可变原生证据profile的开始记录。
 */
struct TutorialSessionStart final {
  domain::MatchId session_id;
  domain::PlayerId player_id;
  std::string profile_hash;
  std::uint32_t mode_id{103U};
  std::string mission_key;
  std::string directory;
  std::uint32_t difficulty{0U};
  std::uint32_t stage_key{0U};
};

/** @brief 本地客户端报告通关或服务端中断；不是独立验证NPC死亡或竞技胜负。 */
struct TutorialSessionFinish final {
  domain::MatchId session_id;
  domain::PlayerId player_id;
  bool reported_clear{false};
  std::string reason;
  std::uint32_t mode_id{103U};
  std::optional<std::uint32_t> elapsed_ms;
};

/** @brief
 * 非竞技SQL端口；成功后才能发布场景/736，无钱包、道具、勋章或段位写入能力。 */
class TutorialSessionStore {
 public:
  virtual ~TutorialSessionStore() = default;
  /** @brief 新开始为true，同UUID同玩家/profile重放为false，变更身份拒绝。 */
  [[nodiscard]] virtual ServiceResult<bool> Begin(
      const TutorialSessionStart& command) = 0;
  /** @brief 唯一终态变更为true，同终态重放为false；冲突拒绝且不覆盖历史。 */
  [[nodiscard]] virtual ServiceResult<bool> Finish(
      const TutorialSessionFinish& command) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_PRACTICE_TUTORIAL_SESSION_H_
