#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CLAN_REPOSITORY_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CLAN_REPOSITORY_H_

/**
 * @file clan_repository.h
 * @brief 定义战队用例的 MySQL 短事务适配器。
 */

#include "features/clans/clan_service.h"
#include "persistence/mysql/connection_pool.h"

namespace bf::infrastructure::mysql {

/**
 * @brief 持久化战队、成员、申请、管理流程、留言和聊天。
 * @note
 * 写路径统一先锁战队父行，再锁成员或资产；对象不持有数据库连接，可由有界工作池共享。
 *       系统训练营没有会长，任何管理动作都会在数据库事务内拒绝。
 */
class MySqlClanRepository final : public application::ClanRepository {
 public:
  /** @brief 注入生命周期长于本对象的连接池。 */
  explicit MySqlClanRepository(MySqlConnectionPool& pool) noexcept;
  /** @copydoc application::ClanRepository::activity */
  [[nodiscard]] application::ServiceResult<application::ClanActivityView>
  Activity(domain::PlayerId player) override;
  /** @copydoc application::ClanRepository::rankings */
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanRankingEntry>>
  Rankings(domain::PlayerId player,
           const application::ClanRankingQuery& query) override;
  /** @copydoc application::ClanRepository::recent_matches */
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanRecentMatch>>
  RecentMatches(domain::PlayerId player, std::uint64_t clan) override;

  /** @copydoc application::ClanRepository::records */
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanRecordEntry>>
  Records(domain::PlayerId player,
          const application::ClanRecordsQuery& query) override;

  /** @copydoc application::ClanRepository::today_results */
  [[nodiscard]] application::ServiceResult<application::ClanTodayResults>
  TodayResults(domain::PlayerId player, std::uint64_t clan) override;
  /** @copydoc application::ClanRepository::set_recruitment */
  [[nodiscard]] application::ServiceResult<application::ClanSummary>
  SetRecruitment(domain::PlayerId manager, std::uint64_t clan_id,
                 std::uint32_t mode) override;

  /** @copydoc application::ClanRepository::whisper */
  [[nodiscard]] application::ServiceResult<application::ClanWhisperResult>
  Whisper(domain::PlayerId sender, const std::string& target,
          const std::string& body, std::uint32_t kind,
          const application::ClanOnlineLookup& online) override;

  [[nodiscard]] application::ServiceResult<application::ClanMarkResult>
  ChangeMark(domain::PlayerId player_id, std::uint32_t mark_code) override;

  [[nodiscard]] application::ServiceResult<application::ClanHomeState> LoadHome(
      domain::PlayerId player_id) override;
  [[nodiscard]] application::ServiceResult<
      std::optional<application::ClanSummary>>
  LoadProfile(domain::PlayerId player_id) override;
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanMemberView>>
  LoadRoster(domain::PlayerId player_id) override;
  [[nodiscard]] application::ServiceResult<application::ClanSearchPage> Search(
      std::uint32_t page, const std::string& query) override;
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanApplicantView>>
  LoadApplicants(domain::PlayerId manager_id) override;
  [[nodiscard]] application::ServiceResult<
      std::vector<application::ClanPostView>>
  LoadPosts(domain::PlayerId player_id, std::uint32_t kind,
            std::uint64_t cursor) override;
  [[nodiscard]] application::ServiceResult<bool> EnsureStarterCard(
      domain::PlayerId player_id) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary>
  JoinTraining(domain::PlayerId player_id) override;
  [[nodiscard]] application::ServiceResult<
      std::optional<application::ClanSummary>>
  Leave(domain::PlayerId player_id, bool training_only) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary> Create(
      domain::PlayerId player_id, const std::string& name,
      const std::string& introduction, domain::ItemId card_item_id,
      std::uint32_t capacity) override;
  [[nodiscard]] application::ServiceResult<application::ClanHomeState> Apply(
      domain::PlayerId player_id, std::uint64_t clan_id,
      const std::string& clan_name) override;
  [[nodiscard]] application::ServiceResult<application::ClanHomeState>
  CancelApplication(domain::PlayerId player_id, std::uint64_t clan_id) override;
  [[nodiscard]] application::ServiceResult<application::ClanMemberChangeResult>
  ChangeMember(domain::PlayerId manager_id, domain::PlayerId target_id,
               const std::string& target_nickname, std::int32_t old_role,
               std::int32_t new_role) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary> EditText(
      domain::PlayerId manager_id, bool introduction,
      const std::string& value) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary>
  ScheduleDisband(domain::PlayerId master_id, bool schedule) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary>
  RequestTransfer(domain::PlayerId master_id, domain::PlayerId target_id,
                  std::chrono::system_clock::time_point expires_at) override;
  [[nodiscard]] application::ServiceResult<application::ClanSummary>
  RespondTransfer(domain::PlayerId target_id, domain::PlayerId source_id,
                  bool accepted) override;
  [[nodiscard]] application::ServiceResult<application::ClanMemberChangeResult>
  FinishTransfer(domain::PlayerId master_id, domain::PlayerId target_id,
                 const std::string& target_nickname) override;
  [[nodiscard]] application::ServiceResult<application::ClanPostView> AddPost(
      domain::PlayerId author_id, const std::string& body) override;
  [[nodiscard]] application::ServiceResult<bool> DeletePost(
      domain::PlayerId actor_id, std::uint64_t post_id) override;
  [[nodiscard]] application::ServiceResult<application::ClanChatResult>
  SendChat(domain::PlayerId sender_id, const std::string& body) override;

 private:
  MySqlConnectionPool& pool_;
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_CLAN_REPOSITORY_H_
