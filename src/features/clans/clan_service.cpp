#include "features/clans/clan_service.h"

#include <algorithm>
#include <chrono>
#include <string_view>

#include "application/utf8.h"
#include "features/chat/message_policy.h"

namespace bf::application {
namespace {

ServiceError Invalid(std::string message) {
  return ServiceError{.code = ErrorCode::kInvalidInput,
                      .safe_message = std::move(message)};
}

bool ValidPlayer(domain::PlayerId player_id) noexcept {
  return player_id.IsValid();
}

bool ValidText(std::string_view value, std::size_t minimum, std::size_t maximum,
               bool permit_empty = false) {
  if ((!permit_empty && value.empty()) || value.size() > maximum * 4U) {
    return false;
  }
  const auto code_points = domain::CountUtf8CodePoints(value);
  if (!code_points.has_value() || *code_points < minimum ||
      *code_points > maximum) {
    return false;
  }
  return std::ranges::none_of(value, [](unsigned char character) {
    return character < 0x20U && character != '\t' && character != '\r' &&
           character != '\n';
  });
}

template <typename Value>
ServiceResult<Value> InvalidPlayerResult() {
  return Invalid("战队请求缺少有效玩家标识");
}

}  // namespace

ClanService::ClanService(const SystemClock& clock,
                         ClanRepository& repository) noexcept
    : clock_(clock), repository_(repository) {}

ServiceResult<ClanActivityView> ClanService::Activity(
    domain::PlayerId player) const {
  return ValidPlayer(player) ? repository_.Activity(player)
                             : InvalidPlayerResult<ClanActivityView>();
}

ServiceResult<std::vector<ClanRankingEntry>> ClanService::Rankings(
    domain::PlayerId player, const ClanRankingQuery& query) const {
  if (!ValidPlayer(player) ||
      (query.clan_id &&
       (*query.clan_id == 0U || *query.clan_id == kTrainingClanId)))
    return Invalid("排行榜玩家或战队无效");
  if (query.month) {
    const std::chrono::year_month_day date{*query.month};
    if (date.day() != std::chrono::day{1} ||
        date.year() < std::chrono::year{2000} ||
        date.year() > std::chrono::year{2100})
      return Invalid("排行榜月份无效");
  }
  return repository_.Rankings(player, query);
}

ServiceResult<ClanTodayResults> ClanService::TodayResults(
    domain::PlayerId player, std::uint64_t clan) const {
  if (!player.IsValid() || clan == 0U || clan == kTrainingClanId)
    return ServiceError{.code = ErrorCode::kInvalidInput,
                        .safe_message = "请选择当前普通战队"};
  return repository_.TodayResults(player, clan);
}

ServiceResult<std::vector<ClanRecordEntry>> ClanService::Records(
    domain::PlayerId player, const ClanRecordsQuery& query) const {
  const std::chrono::year_month_day date{query.month};
  if (!ValidPlayer(player) || query.clan_id == 0U ||
      query.clan_id == kTrainingClanId || query.clan_id > 0xFFFFFFFFULL ||
      date.day() != std::chrono::day{1} ||
      date.year() < std::chrono::year{2000} ||
      date.year() > std::chrono::year{2100}) {
    return Invalid("战队战绩的战队或月份无效");
  }
  return repository_.Records(player, query);
}

ServiceResult<std::vector<ClanRecentMatch>> ClanService::RecentMatches(
    domain::PlayerId player, std::uint64_t clan) const {
  if (!ValidPlayer(player) || clan == 0U || clan == kTrainingClanId ||
      clan > 0xFFFFFFFFULL)
    return Invalid("最近比赛需要有效的普通战队");
  return repository_.RecentMatches(player, clan);
}

ServiceResult<ClanSummary> ClanService::SetRecruitment(
    domain::PlayerId manager, std::uint64_t clan_id, std::uint32_t mode) const {
  if (!ValidPlayer(manager) || clan_id == 0U || clan_id > 0xFFFFFFFFULL ||
      mode > 2U) {
    return Invalid("战队招募参数不正确");
  }
  return repository_.SetRecruitment(manager, clan_id, mode);
}

ServiceResult<ClanWhisperResult> ClanService::Whisper(
    domain::PlayerId sender, const std::string& target, const std::string& body,
    std::uint32_t kind, const ClanOnlineLookup& online) const {
  if (!ValidPlayer(sender) || !ValidText(target, 1U, 32U) ||
      !ValidText(body, 1U, 256U) || kind < 1U || kind > 4U || !online) {
    return Invalid("私聊参数不正确");
  }
  if (domain::IsRestrictedMessage(body))
    return Invalid("消息包含禁用词，请修改后再发送");
  return repository_.Whisper(sender, target, body, kind, online);
}

ServiceResult<ClanMarkResult> ClanService::ChangeMark(
    domain::PlayerId player_id, std::uint32_t version,
    std::uint32_t card_item_id, std::uint32_t mark_code,
    std::uint32_t quantity) const {
  // Build 752 Config/ClanMarkList.ini：版本 2，编号 2..31，每次一张 101050。
  if (!ValidPlayer(player_id) || version != 2U || card_item_id != 101050U ||
      quantity != 1U || mark_code < 2U || mark_code > 31U) {
    return Invalid("家族徽章参数与当前原版目录不符");
  }
  return repository_.ChangeMark(player_id, mark_code);
}

ServiceResult<ClanHomeState> ClanService::Home(
    domain::PlayerId player_id) const {
  return ValidPlayer(player_id) ? repository_.LoadHome(player_id)
                                : InvalidPlayerResult<ClanHomeState>();
}

ServiceResult<std::optional<ClanSummary>> ClanService::Profile(
    domain::PlayerId player_id) const {
  return ValidPlayer(player_id)
             ? repository_.LoadProfile(player_id)
             : InvalidPlayerResult<std::optional<ClanSummary>>();
}

ServiceResult<std::vector<ClanMemberView>> ClanService::Roster(
    domain::PlayerId player_id) const {
  return ValidPlayer(player_id)
             ? repository_.LoadRoster(player_id)
             : InvalidPlayerResult<std::vector<ClanMemberView>>();
}

ServiceResult<ClanSearchPage> ClanService::Search(
    std::uint32_t page, const std::string& query) const {
  if (page == 0U || page > 100'000U || !ValidText(query, 0U, 32U, true)) {
    return Invalid("战队搜索参数不正确");
  }
  return repository_.Search(page, query);
}

ServiceResult<std::vector<ClanApplicantView>> ClanService::Applicants(
    domain::PlayerId manager_id) const {
  return ValidPlayer(manager_id)
             ? repository_.LoadApplicants(manager_id)
             : InvalidPlayerResult<std::vector<ClanApplicantView>>();
}

ServiceResult<std::vector<ClanPostView>> ClanService::Posts(
    domain::PlayerId player_id, std::uint32_t kind,
    std::uint64_t cursor) const {
  if (!ValidPlayer(player_id)) {
    return InvalidPlayerResult<std::vector<ClanPostView>>();
  }
  if (kind < 1U || kind > 3U) {
    return Invalid("战队留言翻页参数不正确");
  }
  return repository_.LoadPosts(player_id, kind, cursor);
}

ServiceResult<bool> ClanService::EnsureStarterCard(
    domain::PlayerId player_id) const {
  return ValidPlayer(player_id) ? repository_.EnsureStarterCard(player_id)
                                : InvalidPlayerResult<bool>();
}

ServiceResult<ClanSummary> ClanService::JoinTraining(
    domain::PlayerId player_id) const {
  return ValidPlayer(player_id) ? repository_.JoinTraining(player_id)
                                : InvalidPlayerResult<ClanSummary>();
}

ServiceResult<std::optional<ClanSummary>> ClanService::Leave(
    domain::PlayerId player_id, bool training_only) const {
  return ValidPlayer(player_id)
             ? repository_.Leave(player_id, training_only)
             : InvalidPlayerResult<std::optional<ClanSummary>>();
}

ServiceResult<ClanSummary> ClanService::Create(domain::PlayerId player_id,
                                               const std::string& name,
                                               const std::string& introduction,
                                               std::uint32_t card_item_id,
                                               std::uint32_t capacity) const {
  if (!ValidPlayer(player_id)) {
    return InvalidPlayerResult<ClanSummary>();
  }
  if (!ValidText(name, 2U, 12U) || !ValidText(introduction, 0U, 100U, true)) {
    return Invalid("战队名称或介绍长度不正确");
  }
  const auto expected_capacity =
      card_item_id == kClanCreationCard30
          ? 30U
          : (card_item_id == kClanCreationCard10 ? 10U : 0U);
  if (expected_capacity == 0U || capacity != expected_capacity) {
    return Invalid("战队创建卡或人数上限不正确");
  }
  return repository_.Create(player_id, name, introduction,
                            domain::ItemId{card_item_id}, capacity);
}

ServiceResult<ClanHomeState> ClanService::Apply(
    domain::PlayerId player_id, std::uint64_t clan_id,
    const std::string& clan_name) const {
  if (!ValidPlayer(player_id) || clan_id == 0U ||
      !ValidText(clan_name, 2U, 32U)) {
    return Invalid("入队申请参数不正确");
  }
  return repository_.Apply(player_id, clan_id, clan_name);
}

ServiceResult<ClanHomeState> ClanService::CancelApplication(
    domain::PlayerId player_id, std::uint64_t clan_id) const {
  if (!ValidPlayer(player_id) || clan_id == 0U) {
    return Invalid("取消入队申请参数不正确");
  }
  return repository_.CancelApplication(player_id, clan_id);
}

ServiceResult<ClanMemberChangeResult> ClanService::ChangeMember(
    domain::PlayerId manager_id, domain::PlayerId target_id,
    const std::string& target_nickname, std::int32_t old_role,
    std::int32_t new_role) const {
  if (!ValidPlayer(manager_id) || !ValidPlayer(target_id) ||
      manager_id == target_id || !ValidText(target_nickname, 1U, 32U)) {
    return Invalid("成员管理参数不正确");
  }
  const bool known_old =
      old_role == static_cast<std::int32_t>(kClanApplicantRole) ||
      old_role == static_cast<std::int32_t>(kClanMemberRole) ||
      old_role == static_cast<std::int32_t>(kClanDeputyRole);
  const bool known_new =
      new_role == static_cast<std::int32_t>(kClanMemberRole) ||
      new_role == static_cast<std::int32_t>(kClanDeputyRole) ||
      new_role == -10 || new_role == -30;
  if (!known_old || !known_new) {
    return Invalid("成员职位动作不正确");
  }
  return repository_.ChangeMember(manager_id, target_id, target_nickname,
                                  old_role, new_role);
}

ServiceResult<ClanSummary> ClanService::EditText(
    domain::PlayerId manager_id, bool introduction,
    const std::string& value) const {
  const auto maximum = introduction ? 100U : 256U;
  if (!ValidPlayer(manager_id) || !ValidText(value, 0U, maximum, true)) {
    return Invalid("战队文本过长或包含非法字符");
  }
  return repository_.EditText(manager_id, introduction, value);
}

ServiceResult<ClanSummary> ClanService::ScheduleDisband(
    domain::PlayerId master_id, bool schedule) const {
  return ValidPlayer(master_id)
             ? repository_.ScheduleDisband(master_id, schedule)
             : InvalidPlayerResult<ClanSummary>();
}

ServiceResult<ClanSummary> ClanService::RequestTransfer(
    domain::PlayerId master_id, domain::PlayerId target_id) const {
  if (!ValidPlayer(master_id) || !ValidPlayer(target_id) ||
      master_id == target_id) {
    return Invalid("会长转让目标不正确");
  }
  return repository_.RequestTransfer(master_id, target_id,
                                     clock_.Now() + std::chrono::seconds{120});
}

ServiceResult<ClanSummary> ClanService::RespondTransfer(
    domain::PlayerId target_id, domain::PlayerId source_id,
    bool accepted) const {
  if (!ValidPlayer(target_id) || !ValidPlayer(source_id) ||
      target_id == source_id) {
    return Invalid("会长转让回复不正确");
  }
  return repository_.RespondTransfer(target_id, source_id, accepted);
}

ServiceResult<ClanMemberChangeResult> ClanService::FinishTransfer(
    domain::PlayerId master_id, domain::PlayerId target_id,
    const std::string& target_nickname) const {
  if (!ValidPlayer(master_id) || !ValidPlayer(target_id) ||
      master_id == target_id || !ValidText(target_nickname, 1U, 32U)) {
    return Invalid("会长转让完成参数不正确");
  }
  return repository_.FinishTransfer(master_id, target_id, target_nickname);
}

ServiceResult<ClanPostView> ClanService::AddPost(
    domain::PlayerId author_id, const std::string& body) const {
  if (!ValidPlayer(author_id) || !ValidText(body, 1U, 256U)) {
    return Invalid("战队留言不能为空或包含非法字符");
  }
  return repository_.AddPost(author_id, body);
}

ServiceResult<bool> ClanService::DeletePost(domain::PlayerId actor_id,
                                            domain::PlayerId claimed_actor_id,
                                            std::uint64_t post_id) const {
  if (!ValidPlayer(actor_id) || actor_id != claimed_actor_id || post_id == 0U) {
    return Invalid("删除战队留言的身份或游标不正确");
  }
  return repository_.DeletePost(actor_id, post_id);
}

ServiceResult<ClanChatResult> ClanService::SendChat(
    domain::PlayerId sender_id, const std::string& body) const {
  if (!ValidPlayer(sender_id) || !ValidText(body, 1U, 256U)) {
    return Invalid("战队聊天不能为空或包含非法字符");
  }
  if (domain::IsRestrictedMessage(body))
    return Invalid("消息包含禁用词，请修改后再发送");
  return repository_.SendChat(sender_id, body);
}

}  // namespace bf::application
