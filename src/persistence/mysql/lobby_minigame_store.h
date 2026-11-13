#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOBBY_MINIGAME_STORE_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOBBY_MINIGAME_STORE_H_

/** @file lobby_minigame_store.h
 *  @brief 广场小游戏事务适配器；自然日与资产流水必须由服务端统一确定。
 *
 *  Each completed action is a succeeded `bf.operation_requests` row
 * (lmg_fishing, lmg_basketball, lmg_fortune) keyed by the Asia/Shanghai server
 * day; the fortune key is the day itself, so the unique constraint enforces one
 * paper per day. Charges and rewards reuse the wallet/inventory ledgers with
 * the operation ID as business ID.
 */
#include "features/lobby/lobby_minigame.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {
/** @brief 每次费用、奖励及每日记录必须在同一 MySQL 事务内提交。 */
class MySqlLobbyMinigameStore final : public runtime::LobbyMinigameStore {
 public:
  explicit MySqlLobbyMinigameStore(MySqlConnectionPool& pool) noexcept
      : pool_(pool) {}

  [[nodiscard]] application::ServiceResult<runtime::LobbyMinigameRecord> Load(
      domain::PlayerId player,
      const protocol::LobbyMinigameConfig::Fishing& config) override;
  [[nodiscard]] application::ServiceResult<runtime::LobbyMinigameCharge>
  CompleteFishing(domain::PlayerId player,
                  const protocol::LobbyMinigameConfig::Fishing& config,
                  const protocol::LobbyFishingTerms& terms,
                  std::uint16_t daily_limit,
                  const std::vector<std::uint32_t>& rewards) override;
  [[nodiscard]] application::ServiceResult<runtime::LobbyMinigameCharge>
  ShootBasketball(domain::PlayerId player, std::int64_t cost,
                  std::optional<std::uint32_t> reward) override;
  [[nodiscard]] application::ServiceResult<runtime::LobbyFortuneOutcome>
  Fortune(domain::PlayerId player, std::uint64_t paper, std::int64_t cost,
          std::optional<std::uint32_t> lucky_item) override;

 private:
  MySqlConnectionPool& pool_;
};
}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_LOBBY_MINIGAME_STORE_H_
