#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_USER_HOST_PRIZE_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_USER_HOST_PRIZE_REPOSITORY_H_

/**
 * @file user_host_prize_repository.h
 * @brief 在调用方对局事务中管理主办奖金、退款和原生余额上限下的耐久待领取资产。
 */

#include <optional>
#include <string_view>

#include "features/battle/match_settlement.h"
#include "features/battle/match_start.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {

/** @brief
 * 已锁定且首次创建对局后扣除主办者金币和101164卡；失败由调用方回滚整局。 */
[[nodiscard]] std::optional<application::ServiceError> FundUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::MatchStartCommand& command);

/** @brief 在对局锁下查找主办者；供普通结算按全体玩家编号统一锁资料和钱包。 */
[[nodiscard]] std::optional<domain::PlayerId> UserHostSponsor(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid);

/** @brief
 * 以单条SQL一致读取已关闭托管、债权、实际付款汇总和当前钱包；未托管返回空。 */
[[nodiscard]] std::optional<application::UserHostPrizeReceipt>
ReadUserHostPrizeReceipt(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid);

/** @brief
 * 普通奖励写入后同事务分配净奖金或退款；只能使用可信终局，关闭后不重复分配。 */
[[nodiscard]] std::optional<application::ServiceError> SettleUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    const application::MatchSettlementCommand& command);

/** @brief 已锁定对局的开局失败或启动恢复退款；重复调用不重复返还金币或卡。 */
void RefundUserHostPrize(
    bf::infrastructure::mysql::TransactionBase& transaction,
    std::string_view match_uuid);

/**
 * @brief
 * 按玩家资料、钱包、待领取行、库存顺序将可容纳金额和卡入账，剩余资产继续持久保存。
 * @note 不锁对局；用于菜单刷新及结算。调用方必须提交；异常时整个事务回滚。
 */
void ClaimUserHostPrizes(
    bf::infrastructure::mysql::TransactionBase& transaction,
    domain::PlayerId player_id);

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_USER_HOST_PRIZE_REPOSITORY_H_
