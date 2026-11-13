#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_TERMINAL_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_TERMINAL_H_

/**
 * @file match_terminal.h
 * @brief 定义普通击爆对局运行时事实到可靠结算命令的显式边界。
 */

#include <cstdint>
#include <vector>

#include "application/error.h"
#include "features/battle/match_result.h"
#include "features/battle/match_settlement.h"

namespace bf::application {

/**
 * @brief 房间 executor 在终局时冻结、交给数据库工作池前的完整普通对局事实。
 * @note 本结构不包含客户端声称的奖励或余额；统计必须来自已认证运行时账本。
 */
struct MatchTerminalFacts final {
  domain::MatchId match_id;           ///< 开局前已持久化的稳定对局 UUID。
  std::uint32_t mode_id{0U};          ///< 开局冻结的正模式编号。
  std::uint32_t elapsed_seconds{0U};  ///< 夹紧前应为 0～3600 秒。
  MatchCompletionKind completion_kind{
      MatchCompletionKind::kCompleted};  ///< 完成、中断或放弃。
  domain::MatchScoreModel score_model{
      domain::MatchScoreModel::kTeamPangs};  ///< 明确支持的计分规则。
  std::vector<domain::MatchResultParticipantInput>
      participants;  ///< 所有开局玩家及确认统计。
  std::optional<std::uint8_t>
      decided_team;  ///< 目标状态机确认的爆破/合作胜方。
};

/**
 * @brief 计算胜负/并列名次并生成可幂等提交的可靠结算命令。
 * @return 输入缺失、范围非法或使用未知计分模型时返回
 * kInvalidInput，不产生部分命令。
 * @note
 * 爆破、拾取、海盗等目标模式应有各自终局构建器，不得调用本函数套用击爆胜负。
 */
[[nodiscard]] ServiceResult<MatchSettlementCommand> BuildMatchSettlementCommand(
    const MatchTerminalFacts& facts);

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_TERMINAL_H_
