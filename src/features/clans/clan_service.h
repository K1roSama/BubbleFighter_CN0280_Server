#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_SERVICE_H_

/**
 * @file clan_service.h
 * @brief 定义战队查询、成员管理、留言和聊天的业务用例边界。
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/system_clock.h"

namespace bf::application {

/** @brief 本队今日已成功结算的比赛数，原版顺序为胜、平、负。 */
struct ClanTodayResults final {
  std::uint32_t wins{0U};
  std::uint32_t draws{0U};
  std::uint32_t losses{0U};
};

/** @brief 原生战绩页的一项成员/对手记录，累计统计不受所选月份限制。 */
struct ClanRecordEntry final {
  std::uint32_t id{0U};            ///< 成员玩家号或对手战队号。
  std::string name;                ///< 当前权威昵称或队名。
  std::uint32_t display_code{0U};  ///< 成员等级，或对手徽章编号。
  ClanTodayResults monthly;        ///< 所选自然月的胜平负。
  ClanTodayResults lifetime;       ///< 全部已成功结算的胜平负。
};

/** @brief 当前战队的成员/对手战绩查询；month 是中国自然月首日。 */
struct ClanRecordsQuery final {
  std::uint64_t clan_id{0U};
  std::chrono::sys_days month{};
  bool opponents{false};
};

/** @brief 最近一场战队赛的对手、胜平负和双方各六个原生详情席位。 */
struct ClanRecentMatch final {
  std::uint32_t opponent_id{0U};
  std::uint32_t opponent_mark{0U};
  std::string opponent_name;
  ClanTodayResults result;  ///< 只允许胜/平/负其中一项为1。
  std::array<std::string, 12U>
      participant_names{};  ///< 本队六人，随后对手六人；空席为空串。
  std::chrono::system_clock::time_point ended_at{};
};

/** @brief 原生活跃页：上海自然周、本队周积分和本人在本队累计贡献。 */
struct ClanActivityView final {
  std::uint64_t clan_id{0U};
  std::chrono::system_clock::time_point period_start{};
  std::chrono::system_clock::time_point period_end{};
  std::uint32_t weekly_points{0U};
  std::uint32_t personal_points{0U};
  std::uint32_t level{1U};
};

/** @brief 原生月度或累计排行榜条目；无赛果的本人排名为0。 */
struct ClanRankingEntry final {
  std::uint32_t clan_id{0U};
  std::string name;
  std::uint32_t rank{0U};
  std::uint32_t level{1U};
  ClanTodayResults results;
};

/** @brief month为空取累计榜；clan_id为空取前100名，否则只取本队。 */
struct ClanRankingQuery final {
  std::optional<std::chrono::sys_days> month;
  std::optional<std::uint32_t> clan_id;
};

/** @brief Nx 私聊已提交的权威双方身份；普通聊天和自动忙碌回复共用原生线路。 */
struct ClanWhisperResult final {
  domain::PlayerId recipient;  ///< 数据库解析的目标，不能由客户端指定玩家号。
  std::string
      sender_account;  ///< 旧登录名回复的兼容解析事实，不作为/r目标发送。
  std::string sender_nickname;  ///< 接收端显示名。
  std::string
      recipient_account;  ///< 旧登录名输入的兼容解析事实，不作为原生回执显示名。
  std::string recipient_nickname;  ///< 回执中的目标显示名。
};

/** @brief 数据库解析目标后查询当前 Nx 连接；必须无阻塞且不访问数据库。 */
using ClanOnlineLookup = std::function<bool(domain::PlayerId)>;

inline constexpr std::uint64_t kTrainingClanId =
    99U;  ///< 原客户端训练所固定战队编号。
inline constexpr std::uint32_t kClanMemberRole =
    100U;  ///< 普通成员协议角色码。
inline constexpr std::uint32_t kClanMasterRole = 101U;  ///< 会长协议角色码。
inline constexpr std::uint32_t kClanDeputyRole = 102U;  ///< 副会长协议角色码。
inline constexpr std::uint32_t kClanApplicantRole =
    1U;  ///< 待审批申请协议角色码。
inline constexpr std::uint32_t kClanCreationCard30 =
    100819U;  ///< 30 人建队卡模板。
inline constexpr std::uint32_t kClanCreationCard10 =
    101979U;  ///< 10 人建队卡模板。

/** @brief 家族徽章更换提交结果，区分库存余额与本次消耗。 */
struct ClanMarkResult final {
  std::uint32_t mark_code{0U};        ///< 已提交的新徽章编号。
  std::uint32_t remaining_cards{0U};  ///< 提交后兑换券数量。
  std::uint32_t consumed_cards{
      0U};  ///< 原版 990 第三字段：本次消耗数，重放为零。
};

/** @brief 战队展示资料及当前成员容量。 */
struct ClanSummary final {
  std::uint64_t clan_id{0U};  ///< 战队数据库主键。
  std::string name;           ///< UTF-8 展示名称。
  std::optional<domain::PlayerId>
      leader_player_id;            ///< 普通战队当前会长；训练营为空。
  std::string leader_nickname;     ///< 查询时冻结的会长昵称。
  std::string introduction;        ///< 战队简介。
  std::string notice;              ///< 战队公告。
  std::uint32_t mark_code{0U};     ///< 原客户端徽章编号。
  std::uint32_t capacity{0U};      ///< 最大成员数。
  std::uint32_t member_count{0U};  ///< 当前成员数。
  bool pending_disband{false};     ///< 是否等待显式维护任务解散。
  std::chrono::system_clock::time_point created_at{};  ///< 创建 UTC 时间。
  std::optional<std::chrono::system_clock::time_point>
      disband_requested_at;  ///< 解散申请时间。
  std::uint32_t recruitment_mode{
      1U};  ///< 0 关闭，1 审批加入，2 直接加入；对应原版 feature 322。
  std::uint32_t level{1U};         ///< 从真实贡献流水计算的本服等级。
  std::uint32_t level_points{0U};  ///< 当前等级内的积分进度。
};

/** @brief 战队搜索的单页条目和同一数据库快照中的总匹配数。 */
struct ClanSearchPage final {
  std::vector<ClanSummary> entries;  ///< 当前页，原客户端每页最多 5 条。
  std::uint32_t total_count{0U};     ///< 全部匹配数，不是当前页大小。
};

/** @brief 一名战队成员的权威资料、等级和角色。 */
struct ClanMemberView final {
  domain::PlayerId player_id;   ///< 玩家编号。
  std::string nickname;         ///< 当前昵称。
  std::uint32_t level{0U};      ///< 由发布规则计算的显示等级。
  std::uint32_t role_code{0U};  ///< 100/101/102。
  std::chrono::system_clock::time_point joined_at{};  ///< 加入时间。
};

/** @brief 一项待审批申请。 */
struct ClanApplicantView final {
  domain::PlayerId player_id;                          ///< 申请玩家。
  std::string nickname;                                ///< 当前昵称。
  std::uint32_t level{0U};                             ///< 当前显示等级。
  std::chrono::system_clock::time_point created_at{};  ///< 申请时间。
};

/** @brief 一条战队留言板记录。 */
struct ClanPostView final {
  std::uint64_t post_id{0U};                           ///< 稳定留言游标。
  domain::PlayerId author_player_id;                   ///< 作者玩家。
  std::string author_nickname;                         ///< 发布时昵称快照。
  std::uint32_t author_level{0U};                      ///< 查询时显示等级。
  std::string body;                                    ///< UTF-8 留言正文。
  std::chrono::system_clock::time_point created_at{};  ///< 发布时间。
};

/** @brief 玩家当前战队或申请状态；均为空表示尚未加入且未申请。 */
struct ClanHomeState final {
  std::optional<ClanSummary> clan;               ///< 当前成员所属战队。
  std::optional<std::uint32_t> role_code;        ///< 当前成员角色。
  std::optional<std::uint64_t> pending_clan_id;  ///< 未入队时的待审批战队。
  std::string pending_clan_name;                 ///< 待审批战队名称。
};

/** @brief 一次成员或申请变更后的广播上下文。 */
struct ClanMemberChangeResult final {
  ClanSummary clan;                     ///< 变更所属战队。
  domain::PlayerId target_player_id;    ///< 被变更玩家。
  std::string target_nickname;          ///< 权威昵称。
  std::int32_t new_role{0};             ///< 100/102 或负数离开原因。
  bool joined_from_application{false};  ///< 是否由申请转成员。
};

/** @brief 持久化战队聊天后的成员目标和正文。 */
struct ClanChatResult final {
  std::uint64_t clan_id{0U};                 ///< 聊天所属战队。
  std::vector<domain::PlayerId> recipients;  ///< 已排除屏蔽发送者的当前成员。
  std::string sender_nickname;               ///< 数据库权威昵称。
  std::string body;                          ///< 已校验正文。
};

/**
 * @brief 战队用例依赖的事务仓储。
 * @note
 * 每个写方法必须自行开启短事务、先锁战队父行再锁成员/资产；成功返回前提交审计。
 *       历史解散请求不得在普通启动或读取路径自动执行。
 */
class ClanRepository {
 public:
  virtual ~ClanRepository() = default;

  /** @brief 查询当前普通战队的真实活跃积分，不在读取时发放奖励。 */
  [[nodiscard]] virtual ServiceResult<ClanActivityView> Activity(
      domain::PlayerId) {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队活跃度尚未配置"};
  }

  /** @brief 从已成功结算的赛果生成稳定排行榜和完整排名的本队查询。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRankingEntry>> Rankings(
      domain::PlayerId, const ClanRankingQuery&) {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队排行榜尚未配置"};
  }

  /** @brief 读取本队最近50场已成功完成结算的比赛与参赛者身份。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRecentMatch>>
  RecentMatches(domain::PlayerId, std::uint64_t) {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队最近比赛尚未配置"};
  }

  /** @brief 从开局冻结的归属与成功结算读取本队战绩。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRecordEntry>> Records(
      domain::PlayerId, const ClanRecordsQuery&) {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队战绩查询尚未配置"};
  }

  /** @brief 查询本人所属普通战队的当日已提交赛果；旧适配器明确返回未恢复。 */
  [[nodiscard]] virtual ServiceResult<ClanTodayResults> TodayResults(
      domain::PlayerId, std::uint64_t) {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队比赛查询尚未配置"};
  }

  /** @brief 管理者原子修改指定本队的招募模式；重放不重复写审计。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> SetRecruitment(
      domain::PlayerId manager, std::uint64_t clan_id, std::uint32_t mode) = 0;

  /** @brief 锁定双方、检查在线与屏蔽并写入私聊；自动回复必须有近期反向消息。 */
  [[nodiscard]] virtual ServiceResult<ClanWhisperResult> Whisper(
      domain::PlayerId sender, const std::string& target,
      const std::string& body, std::uint32_t kind,
      const ClanOnlineLookup& online) = 0;

  /** @brief 锁定普通家族并验证会长，原子扣除一张兑换券、更换徽章及写入流水。 */
  [[nodiscard]] virtual ServiceResult<ClanMarkResult> ChangeMark(
      domain::PlayerId player_id, std::uint32_t mark_code) = 0;

  /** @brief 读取玩家当前成员关系或待审批申请，用于构造战队主页。 */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> LoadHome(
      domain::PlayerId player_id) = 0;
  /** @brief 读取玩家所属战队资料；未加入战队时返回空值。 */
  [[nodiscard]] virtual ServiceResult<std::optional<ClanSummary>> LoadProfile(
      domain::PlayerId player_id) = 0;
  /** @brief 读取玩家所属战队的全部正式成员，按协议要求稳定排序。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanMemberView>> LoadRoster(
      domain::PlayerId player_id) = 0;
  /** @brief 按名称搜索普通战队，并在同一快照中返回当前页和总数。 */
  [[nodiscard]] virtual ServiceResult<ClanSearchPage> Search(
      std::uint32_t page, const std::string& query) = 0;
  /** @brief 读取管理者所属战队的待审批申请；仓储必须校验管理权限。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanApplicantView>>
  LoadApplicants(domain::PlayerId manager_id) = 0;
  /** @brief 按 kind 和稳定游标读取玩家所属战队的留言。 */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanPostView>> LoadPosts(
      domain::PlayerId player_id, std::uint32_t kind, std::uint64_t cursor) = 0;

  /** @brief 幂等保证玩家拥有一张可用的初始建队卡。 */
  [[nodiscard]] virtual ServiceResult<bool> EnsureStarterCard(
      domain::PlayerId player_id) = 0;
  /** @brief 把玩家加入固定训练所；训练所不受普通战队容量与会长规则约束。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> JoinTraining(
      domain::PlayerId player_id) = 0;
  /** @brief 离开当前战队；training_only 为真时只允许离开训练所。 */
  [[nodiscard]] virtual ServiceResult<std::optional<ClanSummary>> Leave(
      domain::PlayerId player_id, bool training_only) = 0;
  /** @brief 原子消耗建队卡并创建战队、会长成员行与审计记录。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> Create(
      domain::PlayerId player_id, const std::string& name,
      const std::string& introduction, domain::ItemId card_item_id,
      std::uint32_t capacity) = 0;
  /** @brief 提交加入普通战队的唯一待审批申请。 */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> Apply(
      domain::PlayerId player_id, std::uint64_t clan_id,
      const std::string& clan_name) = 0;
  /** @brief 取消玩家对指定战队的待审批申请。 */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> CancelApplication(
      domain::PlayerId player_id, std::uint64_t clan_id) = 0;
  /** @brief 由管理者批准、拒绝、踢出或调整一名成员角色，并写审计。 */
  [[nodiscard]] virtual ServiceResult<ClanMemberChangeResult> ChangeMember(
      domain::PlayerId manager_id, domain::PlayerId target_id,
      const std::string& target_nickname, std::int32_t old_role,
      std::int32_t new_role) = 0;
  /** @brief 修改战队简介或公告；仓储必须锁战队并校验管理权限。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> EditText(
      domain::PlayerId manager_id, bool introduction,
      const std::string& value) = 0;
  /** @brief 建立或取消显式解散请求；本方法不直接删除战队。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> ScheduleDisband(
      domain::PlayerId master_id, bool schedule) = 0;
  /** @brief 由会长建立有期限的会长转让请求。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> RequestTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id,
      std::chrono::system_clock::time_point expires_at) = 0;
  /** @brief 由目标玩家接受或拒绝指定来源会长的转让请求。 */
  [[nodiscard]] virtual ServiceResult<ClanSummary> RespondTransfer(
      domain::PlayerId target_id, domain::PlayerId source_id,
      bool accepted) = 0;
  /** @brief 完成已经接受的转让，原子交换会长/成员角色并写审计。 */
  [[nodiscard]] virtual ServiceResult<ClanMemberChangeResult> FinishTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id,
      const std::string& target_nickname) = 0;
  /** @brief 在玩家所属战队新增一条已校验留言。 */
  [[nodiscard]] virtual ServiceResult<ClanPostView> AddPost(
      domain::PlayerId author_id, const std::string& body) = 0;
  /** @brief 删除指定留言；普通成员只能删除自己的记录，管理者可按规则管理。 */
  [[nodiscard]] virtual ServiceResult<bool> DeletePost(
      domain::PlayerId actor_id, std::uint64_t post_id) = 0;
  /** @brief 持久化战队聊天并返回经过屏蔽过滤的在线投递目标。 */
  [[nodiscard]] virtual ServiceResult<ClanChatResult> SendChat(
      domain::PlayerId sender_id, const std::string& body) = 0;
};

/** @brief Nx 网络层使用的完整战队用例接口。 */
class ClanExecutor {
 public:
  virtual ~ClanExecutor() = default;

  /** @copydoc ClanRepository::activity */
  [[nodiscard]] virtual ServiceResult<ClanActivityView> Activity(
      domain::PlayerId) const {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队活跃度尚未配置"};
  }

  /** @copydoc ClanRepository::rankings */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRankingEntry>> Rankings(
      domain::PlayerId, const ClanRankingQuery&) const {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队排行榜尚未配置"};
  }

  /** @copydoc ClanRepository::recent_matches */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRecentMatch>>
  RecentMatches(domain::PlayerId, std::uint64_t) const {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队最近比赛尚未配置"};
  }

  /** @copydoc ClanRepository::records */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanRecordEntry>> Records(
      domain::PlayerId, const ClanRecordsQuery&) const {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队战绩查询尚未配置"};
  }

  /** @copydoc ClanRepository::today_results */
  [[nodiscard]] virtual ServiceResult<ClanTodayResults> TodayResults(
      domain::PlayerId, std::uint64_t) const {
    return ServiceError{.code = ErrorCode::kUnsupportedOpcode,
                        .safe_message = "战队比赛查询尚未配置"};
  }

  /** @copydoc ClanRepository::set_recruitment */
  [[nodiscard]] virtual ServiceResult<ClanSummary> SetRecruitment(
      domain::PlayerId manager, std::uint64_t clan_id,
      std::uint32_t mode) const = 0;

  /** @copydoc ClanRepository::whisper */
  [[nodiscard]] virtual ServiceResult<ClanWhisperResult> Whisper(
      domain::PlayerId sender, const std::string& target,
      const std::string& body, std::uint32_t kind,
      const ClanOnlineLookup& online) const = 0;

  /** @brief 校验当前原版徽章目录版本、兑换券、编号及数量后执行更换。 */
  [[nodiscard]] virtual ServiceResult<ClanMarkResult> ChangeMark(
      domain::PlayerId player_id, std::uint32_t version,
      std::uint32_t card_item_id, std::uint32_t mark_code,
      std::uint32_t quantity) const = 0;
  /** @copydoc ClanRepository::load_home */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> Home(
      domain::PlayerId player_id) const = 0;
  /** @copydoc ClanRepository::load_profile */
  [[nodiscard]] virtual ServiceResult<std::optional<ClanSummary>> Profile(
      domain::PlayerId player_id) const = 0;
  /** @copydoc ClanRepository::load_roster */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanMemberView>> Roster(
      domain::PlayerId player_id) const = 0;
  /** @copydoc ClanRepository::search */
  [[nodiscard]] virtual ServiceResult<ClanSearchPage> Search(
      std::uint32_t page, const std::string& query) const = 0;
  /** @copydoc ClanRepository::load_applicants */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanApplicantView>>
  Applicants(domain::PlayerId manager_id) const = 0;
  /** @copydoc ClanRepository::load_posts */
  [[nodiscard]] virtual ServiceResult<std::vector<ClanPostView>> Posts(
      domain::PlayerId player_id, std::uint32_t kind,
      std::uint64_t cursor) const = 0;

  /** @copydoc ClanRepository::ensure_starter_card */
  [[nodiscard]] virtual ServiceResult<bool> EnsureStarterCard(
      domain::PlayerId player_id) const = 0;
  /** @copydoc ClanRepository::join_training */
  [[nodiscard]] virtual ServiceResult<ClanSummary> JoinTraining(
      domain::PlayerId player_id) const = 0;
  /** @copydoc ClanRepository::leave */
  [[nodiscard]] virtual ServiceResult<std::optional<ClanSummary>> Leave(
      domain::PlayerId player_id, bool training_only) const = 0;
  /** @copydoc ClanRepository::create */
  [[nodiscard]] virtual ServiceResult<ClanSummary> Create(
      domain::PlayerId player_id, const std::string& name,
      const std::string& introduction, std::uint32_t card_item_id,
      std::uint32_t capacity) const = 0;
  /** @copydoc ClanRepository::apply */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> Apply(
      domain::PlayerId player_id, std::uint64_t clan_id,
      const std::string& clan_name) const = 0;
  /** @copydoc ClanRepository::cancel_application */
  [[nodiscard]] virtual ServiceResult<ClanHomeState> CancelApplication(
      domain::PlayerId player_id, std::uint64_t clan_id) const = 0;
  /** @copydoc ClanRepository::change_member */
  [[nodiscard]] virtual ServiceResult<ClanMemberChangeResult> ChangeMember(
      domain::PlayerId manager_id, domain::PlayerId target_id,
      const std::string& target_nickname, std::int32_t old_role,
      std::int32_t new_role) const = 0;
  /** @copydoc ClanRepository::edit_text */
  [[nodiscard]] virtual ServiceResult<ClanSummary> EditText(
      domain::PlayerId manager_id, bool introduction,
      const std::string& value) const = 0;
  /** @copydoc ClanRepository::schedule_disband */
  [[nodiscard]] virtual ServiceResult<ClanSummary> ScheduleDisband(
      domain::PlayerId master_id, bool schedule) const = 0;
  /** @copydoc ClanRepository::request_transfer */
  [[nodiscard]] virtual ServiceResult<ClanSummary> RequestTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id) const = 0;
  /** @copydoc ClanRepository::respond_transfer */
  [[nodiscard]] virtual ServiceResult<ClanSummary> RespondTransfer(
      domain::PlayerId target_id, domain::PlayerId source_id,
      bool accepted) const = 0;
  /** @copydoc ClanRepository::finish_transfer */
  [[nodiscard]] virtual ServiceResult<ClanMemberChangeResult> FinishTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id,
      const std::string& target_nickname) const = 0;
  /** @copydoc ClanRepository::add_post */
  [[nodiscard]] virtual ServiceResult<ClanPostView> AddPost(
      domain::PlayerId author_id, const std::string& body) const = 0;
  /** @copydoc ClanRepository::delete_post */
  [[nodiscard]] virtual ServiceResult<bool> DeletePost(
      domain::PlayerId actor_id, domain::PlayerId claimed_actor_id,
      std::uint64_t post_id) const = 0;
  /** @copydoc ClanRepository::send_chat */
  [[nodiscard]] virtual ServiceResult<ClanChatResult> SendChat(
      domain::PlayerId sender_id, const std::string& body) const = 0;
};

/** @brief 在协议输入与事务仓储之间执行通用参数、文本和动作白名单校验。 */
class ClanService final : public ClanExecutor {
 public:
  /** @copydoc ClanExecutor::activity */
  [[nodiscard]] ServiceResult<ClanActivityView> Activity(
      domain::PlayerId player) const override;
  /** @copydoc ClanExecutor::rankings */
  [[nodiscard]] ServiceResult<std::vector<ClanRankingEntry>> Rankings(
      domain::PlayerId player, const ClanRankingQuery& query) const override;
  /** @copydoc ClanExecutor::recent_matches */
  [[nodiscard]] ServiceResult<std::vector<ClanRecentMatch>> RecentMatches(
      domain::PlayerId player, std::uint64_t clan) const override;
  /** @copydoc ClanExecutor::records */
  [[nodiscard]] ServiceResult<std::vector<ClanRecordEntry>> Records(
      domain::PlayerId player, const ClanRecordsQuery& query) const override;
  /** @copydoc ClanExecutor::today_results */
  [[nodiscard]] ServiceResult<ClanTodayResults> TodayResults(
      domain::PlayerId player, std::uint64_t clan) const override;
  /** @copydoc ClanExecutor::set_recruitment */
  [[nodiscard]] ServiceResult<ClanSummary> SetRecruitment(
      domain::PlayerId manager, std::uint64_t clan_id,
      std::uint32_t mode) const override;
  /** @copydoc ClanExecutor::whisper */
  [[nodiscard]] ServiceResult<ClanWhisperResult> Whisper(
      domain::PlayerId sender, const std::string& target,
      const std::string& body, std::uint32_t kind,
      const ClanOnlineLookup& online) const override;
  /** @brief 注入 UTC 时钟和事务仓储；两个依赖必须覆盖服务生命周期。 */
  ClanService(const SystemClock& clock, ClanRepository& repository) noexcept;

  /** @copydoc ClanExecutor::change_mark */
  [[nodiscard]] ServiceResult<ClanMarkResult> ChangeMark(
      domain::PlayerId player_id, std::uint32_t version,
      std::uint32_t card_item_id, std::uint32_t mark_code,
      std::uint32_t quantity) const override;

  /** @copydoc ClanExecutor::home */
  [[nodiscard]] ServiceResult<ClanHomeState> Home(
      domain::PlayerId player_id) const override;
  /** @copydoc ClanExecutor::profile */
  [[nodiscard]] ServiceResult<std::optional<ClanSummary>> Profile(
      domain::PlayerId player_id) const override;
  /** @copydoc ClanExecutor::roster */
  [[nodiscard]] ServiceResult<std::vector<ClanMemberView>> Roster(
      domain::PlayerId player_id) const override;
  /** @copydoc ClanExecutor::search */
  [[nodiscard]] ServiceResult<ClanSearchPage> Search(
      std::uint32_t page, const std::string& query) const override;
  /** @copydoc ClanExecutor::applicants */
  [[nodiscard]] ServiceResult<std::vector<ClanApplicantView>> Applicants(
      domain::PlayerId manager_id) const override;
  /** @copydoc ClanExecutor::posts */
  [[nodiscard]] ServiceResult<std::vector<ClanPostView>> Posts(
      domain::PlayerId player_id, std::uint32_t kind,
      std::uint64_t cursor) const override;
  /** @copydoc ClanExecutor::ensure_starter_card */
  [[nodiscard]] ServiceResult<bool> EnsureStarterCard(
      domain::PlayerId player_id) const override;
  /** @copydoc ClanExecutor::join_training */
  [[nodiscard]] ServiceResult<ClanSummary> JoinTraining(
      domain::PlayerId player_id) const override;
  /** @copydoc ClanExecutor::leave */
  [[nodiscard]] ServiceResult<std::optional<ClanSummary>> Leave(
      domain::PlayerId player_id, bool training_only) const override;
  /** @copydoc ClanExecutor::create */
  [[nodiscard]] ServiceResult<ClanSummary> Create(
      domain::PlayerId player_id, const std::string& name,
      const std::string& introduction, std::uint32_t card_item_id,
      std::uint32_t capacity) const override;
  /** @copydoc ClanExecutor::apply */
  [[nodiscard]] ServiceResult<ClanHomeState> Apply(
      domain::PlayerId player_id, std::uint64_t clan_id,
      const std::string& clan_name) const override;
  /** @copydoc ClanExecutor::cancel_application */
  [[nodiscard]] ServiceResult<ClanHomeState> CancelApplication(
      domain::PlayerId player_id, std::uint64_t clan_id) const override;
  /** @copydoc ClanExecutor::change_member */
  [[nodiscard]] ServiceResult<ClanMemberChangeResult> ChangeMember(
      domain::PlayerId manager_id, domain::PlayerId target_id,
      const std::string& target_nickname, std::int32_t old_role,
      std::int32_t new_role) const override;
  /** @copydoc ClanExecutor::edit_text */
  [[nodiscard]] ServiceResult<ClanSummary> EditText(
      domain::PlayerId manager_id, bool introduction,
      const std::string& value) const override;
  /** @copydoc ClanExecutor::schedule_disband */
  [[nodiscard]] ServiceResult<ClanSummary> ScheduleDisband(
      domain::PlayerId master_id, bool schedule) const override;
  /** @copydoc ClanExecutor::request_transfer */
  [[nodiscard]] ServiceResult<ClanSummary> RequestTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id) const override;
  /** @copydoc ClanExecutor::respond_transfer */
  [[nodiscard]] ServiceResult<ClanSummary> RespondTransfer(
      domain::PlayerId target_id, domain::PlayerId source_id,
      bool accepted) const override;
  /** @copydoc ClanExecutor::finish_transfer */
  [[nodiscard]] ServiceResult<ClanMemberChangeResult> FinishTransfer(
      domain::PlayerId master_id, domain::PlayerId target_id,
      const std::string& target_nickname) const override;
  /** @copydoc ClanExecutor::add_post */
  [[nodiscard]] ServiceResult<ClanPostView> AddPost(
      domain::PlayerId author_id, const std::string& body) const override;
  /** @copydoc ClanExecutor::delete_post */
  [[nodiscard]] ServiceResult<bool> DeletePost(
      domain::PlayerId actor_id, domain::PlayerId claimed_actor_id,
      std::uint64_t post_id) const override;
  /** @copydoc ClanExecutor::send_chat */
  [[nodiscard]] ServiceResult<ClanChatResult> SendChat(
      domain::PlayerId sender_id, const std::string& body) const override;

 private:
  const SystemClock& clock_;
  ClanRepository& repository_;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CLANS_CLAN_SERVICE_H_
