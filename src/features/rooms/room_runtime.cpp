#include "features/rooms/room_runtime.h"

#include <algorithm>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

#include "features/battle/event_mole_profile.h"
#include "features/practice/horde_tutor_profile.h"
#include "features/rooms/room_item_policy.h"

namespace bf::runtime {
namespace {

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

[[nodiscard]] bool ValidTiming(const RoomMatchTiming& timing) noexcept {
  using namespace std::chrono_literals;
  return timing.loading_timeout > 0ms && timing.countdown >= 0ms &&
         timing.match_duration > 0ms && timing.result_transition >= 0ms &&
         timing.result_duration > 0ms && timing.movement_tick > 0ms &&
         timing.result_transition <= timing.result_duration;
}

}  // namespace

RoomRuntime::RoomRuntime(domain::RoomId room_id, domain::PlayerId host,
                         RoomCreateSpec spec)
    : room_id_(room_id),
      host_player_id_(host),
      capacity_(spec.capacity),
      mode_id_(spec.mode_id),
      password_(std::move(spec.password)),
      user_hosted_(spec.user_hosted),
      hide_seek_v1_(spec.hide_seek_v1) {
  members_.emplace(
      host, Member{.player_id = host,
                   .seat = static_cast<std::uint16_t>(user_hosted_ ? 12U : 0U),
                   .team = static_cast<std::uint8_t>(user_hosted_ ? 36U : 1U)});
}

application::ServiceResult<RoomEvent> RoomRuntime::SetReady(
    domain::PlayerId player_id, bool ready) {
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间已开始转换，不能修改准备状态");
  }
  auto member_iterator = members_.find(player_id);
  if (member_iterator == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState, "玩家不在当前房间");
  }
  const bool requested = player_id == host_player_id_ ? false : ready;
  if (member_iterator->second.ready == requested) {
    return RoomEvent::kNoChange;
  }
  member_iterator->second.ready = requested;
  return RoomEvent::kReadyChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::ConvertHideSeekMember(
    domain::PlayerId player_id) {
  const auto it = members_.find(player_id);
  if (!hide_seek_v1_ || phase_ != RoomPhase::kPlaying || it == members_.end() ||
      it->second.team == 36U)
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前状态不允许躲猫猫转阵营");
  if (it->second.team == 2U) return RoomEvent::kNoChange;
  it->second.team = 2U;
  return RoomEvent::kTeamChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::SwitchTeam(
    domain::PlayerId player_id) {
  if (hide_seek_v1_)
    return MakeError(application::ErrorCode::kInvalidState,
                     "躲猫猫阵营由每次开局随机分配");
  if (mode_id_ == 13U || mode_id_ == 23U)
    return MakeError(application::ErrorCode::kInvalidState,
                     "个人战不允许切换队伍");
  if (IsTraining())
    return MakeError(application::ErrorCode::kInvalidState,
                     "训练场不允许切换队伍");
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间已开始转换，不能切换队伍");
  }
  auto member_iterator = members_.find(player_id);
  if (member_iterator == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState, "玩家不在当前房间");
  }
  if ((mode_id_ == 101U || mode_id_ == 102U) || mode_id_ == 16U ||
      mode_id_ == 8U || mode_id_ == 28U ||
      member_iterator->second.team == 36U) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "共同作战模式不能切换到对方队伍");
  }

  const auto target_team =
      static_cast<std::uint8_t>(member_iterator->second.team == 1U ? 2U : 1U);
  const auto target_count = std::count_if(
      members_.begin(), members_.end(), [target_team](const auto& entry) {
        return entry.second.team == target_team;
      });
  if (target_count >= static_cast<std::ptrdiff_t>(capacity_ / 2U)) {
    return MakeError(application::ErrorCode::kConflict, "目标队伍已满");
  }
  member_iterator->second.team = target_team;
  member_iterator->second.ready = false;
  return RoomEvent::kTeamChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::TransferHost(
    HostTransfer request) {
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能在等待房间移交房主");
  }
  if (request.actor != host_player_id_) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有当前房主可以移交房主");
  }
  const auto member = members_.find(request.target);
  if (member == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "目标玩家不在当前房间");
  }
  if (request.target == host_player_id_) {
    return RoomEvent::kNoChange;
  }
  if (user_hosted_) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "赏金主办者不能在未重新确认出资时移交房主");
  }
  host_player_id_ = request.target;
  member->second.ready = false;
  return RoomEvent::kHostChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::SetSeatLocked(
    domain::PlayerId actor, std::uint16_t seat, bool locked) {
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间已开始转换，不能修改座位锁");
  }
  if (actor != host_player_id_) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以修改座位锁");
  }
  if (seat >= capacity_) {
    return MakeError(application::ErrorCode::kInvalidInput, "座位超出房间容量");
  }
  const bool occupied = std::any_of(
      members_.begin(), members_.end(),
      [seat](const auto& entry) { return entry.second.seat == seat; });
  if (occupied) {
    return MakeError(application::ErrorCode::kConflict, "已占用座位不能锁定");
  }

  const bool changed = locked ? locked_seats_.insert(seat).second
                              : locked_seats_.erase(seat) != 0U;
  return changed ? RoomEvent::kSeatLockChanged : RoomEvent::kNoChange;
}

application::ServiceResult<RoomEvent> RoomRuntime::ApplyConfigurationMode(
    domain::PlayerId actor, std::uint32_t mode_id, bool rules_changed) {
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间已开始转换，不能修改配置");
  }
  if (actor != host_player_id_) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以修改房间配置");
  }
  if (mode_id == 0U) {
    return MakeError(application::ErrorCode::kInvalidInput, "房间模式不能为零");
  }
  if (hide_seek_v1_ && mode_id != 1U)
    return MakeError(application::ErrorCode::kInvalidState,
                     "躲猫猫不能切换原生场景模式");
  if (mode_id == kHordeTutorMode && mode_id_ != kHordeTutorMode)
    return MakeError(application::ErrorCode::kInvalidState,
                     "HordeTutor只能使用原107专用单人入口");
  if (IsTraining() || mode_id == kTrainingMode)
    return MakeError(application::ErrorCode::kInvalidState,
                     "训练场只能通过专用入口创建");
  if (rules_changed || mode_id_ != mode_id) {
    ResetReadyState();
  }
  const bool was_individual = mode_id_ == 13U || mode_id_ == 23U;
  mode_id_ = mode_id;
  if (mode_id_ == 13U || mode_id_ == 23U || was_individual) {
    for (auto& [id, member] : members_) {
      static_cast<void>(id);
      if (member.team != 36U)
        member.team = static_cast<std::uint8_t>(
            mode_id_ == 13U || mode_id_ == 23U ? member.seat + 1U
                                               : member.seat % 2U + 1U);
    }
  }
  if ((mode_id_ == 101U || mode_id_ == 102U) || mode_id_ == 16U ||
      mode_id_ == 8U || mode_id_ == 28U) {
    for (auto& [unused_player_id, room_member] : members_) {
      static_cast<void>(unused_player_id);
      if (room_member.team != 36U) room_member.team = 1U;
    }
  }
  return RoomEvent::kConfigurationChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::ApplyRotatingMapChange(
    domain::PlayerId actor) {
  if (IsTraining())
    return MakeError(application::ErrorCode::kInvalidState,
                     "训练场不允许切换地图");
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "房间已开始转换，不能切换地图");
  }
  if (actor != host_player_id_) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以切换房间地图");
  }
  return RoomEvent::kConfigurationChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::StartLoading(
    domain::PlayerId actor, bool force,
    std::chrono::steady_clock::time_point now, RoomMatchTiming timing,
    bool random_teams) {
  if (phase_ != RoomPhase::kWaiting) {
    return MakeError(application::ErrorCode::kInvalidState, "房间不在等待阶段");
  }
  if (actor != host_player_id_) {
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以开始游戏");
  }
  if (mode_id_ == kEventMoleMode) {
    // No force flag, timer or caller can turn a read-only scene profile into
    // authenticated ChrMole actors. Keep generation/ready/load facts unchanged.
    return MakeError(application::ErrorCode::kInvalidState,
                     std::string{EventMoleStartBlocker()});
  }
  if (!ValidTiming(timing)) {
    return MakeError(application::ErrorCode::kInvalidInput, "房间计时参数无效");
  }
  if (hide_seek_v1_) {
    const auto count = Participants().size();
    if (count < 4U || count > 8U)
      return MakeError(application::ErrorCode::kInvalidState,
                       "躲猫猫需要4～8名玩家");
    timing.match_duration = std::chrono::seconds{270};
    timing.use_room_duration = false;
  }
  if (user_hosted_) {
    const auto first = std::count_if(
        members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.team == 1U; });
    const auto second = std::count_if(
        members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.team == 2U; });
    if (first == 0 || first != second) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "赏金对战要求两队均有人且人数相同");
    }
  }
  if (!force) {
    const bool has_unready_member = std::any_of(
        members_.begin(), members_.end(), [this](const auto& entry) {
          return entry.first != host_player_id_ && entry.second.team != 36U &&
                 !entry.second.ready;
        });
    if (has_unready_member) {
      return MakeError(application::ErrorCode::kInvalidState, "仍有成员未准备");
    }
  }

  timing_ = timing;
  round_loading_ = false;
  ResetTransientMemberState();
  if (hide_seek_v1_) {
    std::vector<Member*> shuffled;
    for (auto& [id, member] : members_) {
      static_cast<void>(id);
      if (member.team != 36U) shuffled.push_back(&member);
    }
    static thread_local std::mt19937 random{std::random_device{}()};
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    const auto hunters = shuffled.size() <= 5U ? 1U : 2U;
    for (std::size_t i = 0; i < shuffled.size(); ++i)
      shuffled[i]->team = i < hunters ? 2U : 1U;
  } else if ((mode_id_ == 101U || mode_id_ == 102U) || mode_id_ == 16U ||
             mode_id_ == 8U || mode_id_ == 28U) {
    // 共同作战等待界面统一为队伍 1；进入地图后原版脚本把真人放到队伍 2，
    // 海盗敌人使用队伍 1；ZombiePve 友方 NPC 为 2、僵尸为 35（DFD450）。
    // 两种模式都不能把真人拆成普通红蓝队。返回等待期时再归一为 1。
    for (auto& [unused_player_id, room_member] : members_) {
      static_cast<void>(unused_player_id);
      if (room_member.team != 36U)
        room_member.team = (mode_id_ == 8U || mode_id_ == 28U) ? 1U : 2U;
    }
  } else if (random_teams && mode_id_ != 13U && mode_id_ != 23U) {
    std::vector<Member*> shuffled;
    shuffled.reserve(members_.size());
    for (auto& [unused_player_id, member] : members_) {
      static_cast<void>(unused_player_id);
      if (member.team != 36U) shuffled.push_back(&member);
    }
    static thread_local std::mt19937 random{std::random_device{}()};
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    const auto first = std::uniform_int_distribution<unsigned>{0U, 1U}(random);
    for (std::size_t index = 0U; index < shuffled.size(); ++index)
      shuffled[index]->team =
          static_cast<std::uint8_t>(1U + (index + first) % 2U);
  }
  InvalidatePhase();
  phase_ = RoomPhase::kLoading;
  loading_deadline_ = now + timing_.loading_timeout;
  countdown_deadline_.reset();
  match_deadline_.reset();
  result_display_deadline_.reset();
  result_return_deadline_.reset();
  return RoomEvent::kLoadingStarted;
}

application::ServiceResult<RoomEvent> RoomRuntime::RecordAssetsReceived(
    domain::PlayerId player_id, std::chrono::steady_clock::time_point now) {
  return RecordLoadSignal(player_id, true, now);
}

application::ServiceResult<RoomEvent> RoomRuntime::RecordLoadCompleted(
    domain::PlayerId player_id, std::chrono::steady_clock::time_point now) {
  return RecordLoadSignal(player_id, false, now);
}

application::ServiceResult<RoomEvent> RoomRuntime::RecordObjectId(
    domain::PlayerId player_id, std::uint32_t object_id) {
  if (phase_ != RoomPhase::kLoading && phase_ != RoomPhase::kCountdown &&
      phase_ != RoomPhase::kPlaying) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "当前房间阶段不接受战斗对象标识");
  }
  auto member_iterator = members_.find(player_id);
  if (member_iterator == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState, "玩家不在当前房间");
  }
  constexpr std::uint32_t object_base = 1'000'000U;
  constexpr std::uint32_t object_stride = 100'000U;
  const auto expected =
      object_base +
      static_cast<std::uint32_t>(member_iterator->second.seat) * object_stride;
  if (object_id != expected) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "战斗对象标识与玩家座位不一致");
  }
  if (member_iterator->second.object_id == object_id) {
    return RoomEvent::kNoChange;
  }
  member_iterator->second.object_id = object_id;
  return RoomEvent::kLoadProgress;
}

application::ServiceResult<RoomEvent> RoomRuntime::FinishMatch(
    std::chrono::steady_clock::time_point now) {
  if (phase_ == RoomPhase::kResults) {
    return RoomEvent::kNoChange;
  }
  if (phase_ != RoomPhase::kPlaying) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "对局尚未处于战斗阶段");
  }
  return EnterResults(now);
}

application::ServiceResult<RoomEvent> RoomRuntime::ApplyTimer(
    const RoomTimerToken& token, std::chrono::steady_clock::time_point now) {
  if (token.room_id != room_id_ || token.generation != generation_) {
    return RoomEvent::kIgnoredStaleTimer;
  }

  const auto matches =
      [&token](
          RoomTimerKind kind,
          const std::optional<std::chrono::steady_clock::time_point>& value) {
        return token.kind == kind && value.has_value() &&
               *value == token.deadline;
      };
  const bool is_active =
      matches(RoomTimerKind::kLoadingTimeout, loading_deadline_) ||
      matches(RoomTimerKind::kCountdown, countdown_deadline_) ||
      matches(RoomTimerKind::kMatchTimeout, match_deadline_) ||
      matches(RoomTimerKind::kResultsDisplay, result_display_deadline_) ||
      matches(RoomTimerKind::kResultsReturn, result_return_deadline_) ||
      matches(RoomTimerKind::kModeObjective, objective_deadline_) ||
      matches(RoomTimerKind::kCombatTick, combat_tick_deadline_);
  if (!is_active) {
    return RoomEvent::kIgnoredStaleTimer;
  }
  if (now < token.deadline) {
    return RoomEvent::kNoChange;
  }

  switch (token.kind) {
    case RoomTimerKind::kCombatTick:
      combat_tick_deadline_ = now + timing_.movement_tick;
      return RoomEvent::kCombatTickDue;
    case RoomTimerKind::kModeObjective:
      objective_deadline_.reset();
      return RoomEvent::kModeObjectiveDue;
    case RoomTimerKind::kLoadingTimeout:
      return ResetToWaiting();
    case RoomTimerKind::kCountdown:
      return EnterPlaying(now);
    case RoomTimerKind::kMatchTimeout:
      return EnterResults(now);
    case RoomTimerKind::kResultsDisplay:
      result_display_deadline_.reset();
      return RoomEvent::kResultsDisplayDue;
    case RoomTimerKind::kResultsReturn:
      return ResetToWaiting();
  }
  return MakeError(application::ErrorCode::kInternal, "未知房间计时种类");
}

application::ServiceResult<RoomEvent> RoomRuntime::ReturnToWaiting() {
  if (phase_ == RoomPhase::kClosed) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "已关闭房间不能恢复");
  }
  if (phase_ == RoomPhase::kWaiting) {
    return RoomEvent::kNoChange;
  }
  return ResetToWaiting();
}

void RoomRuntime::AwaitCommittedRewards() {
  if (phase_ != RoomPhase::kResults) {
    throw std::logic_error{"reward wait requires results phase"};
  }
  result_display_deadline_.reset();
  result_return_deadline_.reset();
}

void RoomRuntime::BeginCommittedResults(
    std::chrono::steady_clock::time_point now) {
  if (phase_ != RoomPhase::kResults) {
    throw std::logic_error{"reward display requires results phase"};
  }
  result_display_deadline_ = now + timing_.result_transition;
  result_return_deadline_ = now + timing_.result_duration;
}

void RoomRuntime::BeginAuthorizedMatchClock(
    std::chrono::steady_clock::time_point now) {
  if (phase_ != RoomPhase::kPlaying)
    throw std::logic_error{"match clock requires playing phase"};
  if (mode_id_ == 23U || mode_id_ == 25U || IsTraining())
    match_deadline_.reset();
  else
    match_deadline_ = now + timing_.match_duration;
}

void RoomRuntime::EnableCombatTick(std::chrono::steady_clock::time_point now) {
  if (phase_ != RoomPhase::kPlaying)
    throw std::logic_error{"combat tick requires playing phase"};
  combat_tick_deadline_ = now + timing_.movement_tick;
}

std::vector<RoomTimerToken> RoomRuntime::ActiveTimers() const {
  std::vector<RoomTimerToken> timers;
  timers.reserve(7U);
  const auto append = [this, &timers](RoomTimerKind kind,
                                      const auto& deadline) {
    if (deadline.has_value()) {
      timers.push_back(RoomTimerToken{.room_id = room_id_,
                                      .generation = generation_,
                                      .kind = kind,
                                      .deadline = *deadline});
    }
  };
  append(RoomTimerKind::kLoadingTimeout, loading_deadline_);
  append(RoomTimerKind::kCountdown, countdown_deadline_);
  append(RoomTimerKind::kMatchTimeout, match_deadline_);
  append(RoomTimerKind::kResultsDisplay, result_display_deadline_);
  append(RoomTimerKind::kResultsReturn, result_return_deadline_);
  append(RoomTimerKind::kModeObjective, objective_deadline_);
  append(RoomTimerKind::kCombatTick, combat_tick_deadline_);
  return timers;
}

std::vector<RoomMemberSnapshot> RoomRuntime::Members() const {
  std::vector<RoomMemberSnapshot> result;
  result.reserve(members_.size());
  for (const auto& [unused_player_id, room_member] : members_) {
    static_cast<void>(unused_player_id);
    result.push_back(RoomRuntime::SnapshotOf(room_member));
  }
  std::ranges::sort(result, {}, &RoomMemberSnapshot::seat);
  return result;
}

std::optional<RoomMemberSnapshot> RoomRuntime::GetMember(
    domain::PlayerId player_id) const {
  const auto iterator = members_.find(player_id);
  if (iterator == members_.end()) {
    return std::nullopt;
  }
  return RoomRuntime::SnapshotOf(iterator->second);
}

std::vector<RoomMemberSnapshot> RoomRuntime::Participants() const {
  auto result = Members();
  std::erase_if(result, [](const auto& member) { return member.IsObserver(); });
  return result;
}

domain::RoomId RoomRuntime::RoomId() const noexcept { return room_id_; }

domain::PlayerId RoomRuntime::HostPlayerId() const noexcept {
  return host_player_id_;
}

RoomPhase RoomRuntime::Phase() const noexcept { return phase_; }

std::uint64_t RoomRuntime::Generation() const noexcept { return generation_; }

std::uint16_t RoomRuntime::Capacity() const noexcept { return capacity_; }

std::uint16_t RoomRuntime::SessionSlotCount() const noexcept {
  return user_hosted_ ? 16U : capacity_;
}

bool RoomRuntime::UserHosted() const noexcept { return user_hosted_; }

std::uint32_t RoomRuntime::ModeId() const noexcept { return mode_id_; }

protocol::RoomNetworkMode RoomRuntime::NetworkMode() const noexcept {
  return network_mode_;
}

application::ServiceResult<RoomEvent> RoomRuntime::SetNetworkMode(
    domain::PlayerId actor, protocol::RoomNetworkMode mode) {
  return SetRoomSettings(actor, mode, global_rules_);
}

std::uint32_t RoomRuntime::GlobalRules() const noexcept {
  return global_rules_;
}

application::ServiceResult<RoomEvent> RoomRuntime::SetRoomSettings(
    domain::PlayerId actor, protocol::RoomNetworkMode mode,
    std::uint32_t global_rules) {
  if (actor != host_player_id_)
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以更改房间设置");
  if (phase_ != RoomPhase::kWaiting)
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能在等待房间更改房间设置");
  if (mode != protocol::RoomNetworkMode::kServerDirect &&
      mode != protocol::RoomNetworkMode::kP2pPreferred)
    return MakeError(application::ErrorCode::kInvalidInput, "网络模式无效");
  if (global_rules & ~protocol::kRoomRuleNoGrenades)
    return MakeError(application::ErrorCode::kInvalidInput, "房间全局规则无效");
  if (network_mode_ == mode && global_rules_ == global_rules)
    return RoomEvent::kNoChange;
  network_mode_ = mode;
  global_rules_ = global_rules;
  ResetReadyState();
  return RoomEvent::kConfigurationChanged;
}

application::ServiceResult<RoomEvent> RoomRuntime::SetRestrictedItems(
    domain::PlayerId actor, std::vector<std::uint32_t> items) {
  if (actor != host_player_id_)
    return MakeError(application::ErrorCode::kPermissionDenied,
                     "只有房主可以修改禁用物品");
  if (phase_ != RoomPhase::kWaiting)
    return MakeError(application::ErrorCode::kInvalidState,
                     "只能在等待房间修改禁用物品");
  if (!domain::ValidRoomItems(items))
    return MakeError(application::ErrorCode::kInvalidInput, "禁用物品列表无效");
  if (items == restricted_items_) return RoomEvent::kNoChange;
  restricted_items_ = std::move(items);
  ++item_rules_revision_;
  ResetReadyState();
  return RoomEvent::kConfigurationChanged;
}

std::vector<std::uint16_t> RoomRuntime::LockedSeats() const {
  return {locked_seats_.begin(), locked_seats_.end()};
}

application::ServiceResult<RoomEvent> RoomRuntime::AddMember(
    domain::PlayerId player_id, std::string_view password) {
  const bool late_observer = hide_seek_v1_ && phase_ == RoomPhase::kPlaying;
  if (phase_ != RoomPhase::kWaiting && !late_observer) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "对局已经开始，请等待结束");
  }
  if (members_.contains(player_id)) {
    return MakeError(application::ErrorCode::kConflict, "玩家已经在当前房间");
  }
  if (password != password_) {
    return MakeError(application::ErrorCode::kPermissionDenied, "房间口令错误");
  }

  std::optional<std::uint16_t> free_seat;
  for (std::uint16_t seat = 0U; seat < capacity_; ++seat) {
    const bool occupied = std::any_of(
        members_.begin(), members_.end(),
        [seat](const auto& entry) { return entry.second.seat == seat; });
    if (!occupied && !locked_seats_.contains(seat)) {
      free_seat = seat;
      break;
    }
  }
  if (!free_seat.has_value()) {
    return MakeError(application::ErrorCode::kConflict, "房间已满");
  }

  std::uint8_t team = 1U;
  if (mode_id_ == 13U || mode_id_ == 23U) {
    // Native projectile filters compare Character+400 directly. Individual
    // players need distinct factions; B41510 uses this seat+1 convention too.
    team = static_cast<std::uint8_t>(*free_seat + 1U);
  } else if ((mode_id_ != 101U && mode_id_ != 102U) && mode_id_ != 16U &&
             mode_id_ != 8U && mode_id_ != 28U) {
    const auto team_one = std::count_if(
        members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.team == 1U; });
    const auto team_two = std::count_if(
        members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.team == 2U; });
    team = team_one <= team_two ? 1U : 2U;
  }
  members_.emplace(
      player_id,
      Member{.player_id = player_id,
             .seat = *free_seat,
             .team = static_cast<std::uint8_t>(late_observer ? 36U : team)});
  return RoomEvent::kMemberJoined;
}

RoomRuntime::LeaveState RoomRuntime::RemoveMember(domain::PlayerId player_id) {
  const auto iterator = members_.find(player_id);
  LeaveState state;
  state.departed_seat = iterator->second.seat;
  const bool was_host = player_id == host_player_id_;
  const bool sponsor_left = was_host && user_hosted_;
  members_.erase(iterator);

  if (members_.empty()) {
    phase_ = RoomPhase::kClosed;
    loading_deadline_.reset();
    countdown_deadline_.reset();
    match_deadline_.reset();
    result_display_deadline_.reset();
    result_return_deadline_.reset();
    combat_tick_deadline_.reset();
    InvalidatePhase();
    state.closed = true;
    return state;
  }

  if (was_host) {
    user_hosted_ = false;
    const auto new_host =
        std::min_element(members_.begin(), members_.end(),
                         [](const auto& left, const auto& right) {
                           return left.second.seat < right.second.seat;
                         });
    host_player_id_ = new_host->first;
    new_host->second.ready = false;
    state.new_host = host_player_id_;
  }
  if (phase_ == RoomPhase::kLoading || phase_ == RoomPhase::kCountdown ||
      (phase_ == RoomPhase::kPlaying &&
       (sponsor_left || mode_id_ == 101U || mode_id_ == 102U))) {
    static_cast<void>(ResetToWaiting());
    state.start_interrupted = true;
  }
  return state;
}

application::ServiceResult<RoomEvent> RoomRuntime::RecordLoadSignal(
    domain::PlayerId player_id, bool assets,
    std::chrono::steady_clock::time_point now) {
  if (phase_ != RoomPhase::kLoading) {
    return MakeError(application::ErrorCode::kInvalidState, "房间不在加载阶段");
  }
  auto iterator = members_.find(player_id);
  if (iterator == members_.end()) {
    return MakeError(application::ErrorCode::kInvalidState, "玩家不在当前房间");
  }
  bool& target = assets ? iterator->second.assets_received
                        : iterator->second.load_completed;
  if (target) {
    return RoomEvent::kNoChange;
  }
  target = true;
  if (!AllMembersLoaded()) {
    return RoomEvent::kLoadProgress;
  }
  return AdvanceAfterLoading(now);
}

bool RoomRuntime::AllMembersLoaded() const noexcept {
  return std::all_of(members_.begin(), members_.end(), [](const auto& entry) {
    return entry.second.assets_received && entry.second.load_completed;
  });
}

RoomEvent RoomRuntime::AdvanceAfterLoading(
    std::chrono::steady_clock::time_point now) {
  loading_deadline_.reset();
  if (timing_.countdown.count() == 0) {
    return EnterPlaying(now);
  }
  InvalidatePhase();
  phase_ = RoomPhase::kCountdown;
  countdown_deadline_ = now + timing_.countdown;
  return RoomEvent::kCountdownStarted;
}

RoomEvent RoomRuntime::EnterPlaying(std::chrono::steady_clock::time_point now) {
  loading_deadline_.reset();
  countdown_deadline_.reset();
  InvalidatePhase();
  phase_ = RoomPhase::kPlaying;
  BeginAuthorizedMatchClock(now);
  const auto event =
      round_loading_ ? RoomEvent::kRoundStarted : RoomEvent::kMatchStarted;
  round_loading_ = false;
  return event;
}

RoomEvent RoomRuntime::EnterResults(std::chrono::steady_clock::time_point now) {
  match_deadline_.reset();
  InvalidatePhase();
  phase_ = RoomPhase::kResults;
  result_display_deadline_ = now + timing_.result_transition;
  result_return_deadline_ = now + timing_.result_duration;
  return RoomEvent::kResultsStarted;
}

RoomEvent RoomRuntime::ResetToWaiting() {
  loading_deadline_.reset();
  countdown_deadline_.reset();
  match_deadline_.reset();
  result_display_deadline_.reset();
  result_return_deadline_.reset();
  InvalidatePhase();
  phase_ = RoomPhase::kWaiting;
  if (hide_seek_v1_)
    for (auto& [unused_player, member] : members_) {
      static_cast<void>(unused_player);
      member.team = 1U;
    }
  ResetTransientMemberState();
  ResetReadyState();
  if ((mode_id_ == 101U || mode_id_ == 102U) || mode_id_ == 16U ||
      mode_id_ == 8U || mode_id_ == 28U) {
    for (auto& [unused_player_id, room_member] : members_) {
      static_cast<void>(unused_player_id);
      if (room_member.team != 36U) room_member.team = 1U;
    }
  }
  return RoomEvent::kReturnedToWaiting;
}

RoomMemberSnapshot RoomRuntime::SnapshotOf(const Member& member) {
  return RoomMemberSnapshot{.player_id = member.player_id,
                            .seat = member.seat,
                            .team = member.team,
                            .ready = member.ready,
                            .assets_received = member.assets_received,
                            .load_completed = member.load_completed,
                            .object_id = member.object_id};
}

void RoomRuntime::ResetReadyState() noexcept {
  for (auto& [unused_player_id, room_member] : members_) {
    static_cast<void>(unused_player_id);
    room_member.ready = false;
  }
}

void RoomRuntime::ResetTransientMemberState() noexcept {
  for (auto& [unused_player_id, room_member] : members_) {
    static_cast<void>(unused_player_id);
    room_member.assets_received = false;
    room_member.load_completed = false;
    room_member.object_id.reset();
  }
}

void RoomRuntime::InvalidatePhase() noexcept {
  objective_deadline_.reset();
  combat_tick_deadline_.reset();
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    generation_ = 1U;
    return;
  }
  ++generation_;
}

void RoomRuntime::SetObjectiveDeadline(
    std::optional<std::chrono::steady_clock::time_point> deadline) {
  objective_deadline_ = phase_ == RoomPhase::kPlaying ? deadline : std::nullopt;
}

void RoomRuntime::SuspendMatchTimeout() noexcept { match_deadline_.reset(); }

application::ServiceResult<RoomEvent> RoomRuntime::RestartRound(
    std::chrono::steady_clock::time_point now) {
  if (phase_ != RoomPhase::kPlaying) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "回合重载要求 Playing 状态");
  }
  ResetTransientMemberState();
  InvalidatePhase();
  phase_ = RoomPhase::kLoading;
  round_loading_ = true;
  loading_deadline_ = now + timing_.loading_timeout;
  countdown_deadline_.reset();
  match_deadline_.reset();
  result_display_deadline_.reset();
  result_return_deadline_.reset();
  return RoomEvent::kRoundLoadingStarted;
}

RoomRegistry::RoomRegistry(std::optional<domain::RoomId> isolated_room)
    : isolated_room_(isolated_room) {
  if (isolated_room && (!isolated_room->IsValid() ||
                        isolated_room->Value() > kMaximumWireRoomId)) {
    throw std::invalid_argument{"invalid isolated room id"};
  }
}

application::ServiceResult<std::shared_ptr<RoomRuntime>> RoomRegistry::Create(
    domain::PlayerId host, RoomCreateSpec spec) {
  if (!host.IsValid()) {
    return MakeError(application::ErrorCode::kInvalidInput, "房主标识无效");
  }
  if (membership_.contains(host)) {
    return MakeError(application::ErrorCode::kConflict, "玩家已经在其他房间");
  }
  const bool training = spec.mode_id == kTrainingMode;
  if (spec.hide_seek_v1 && (spec.mode_id != 1U || spec.user_hosted ||
                            spec.capacity < 4U || spec.capacity > 8U))
    return MakeError(application::ErrorCode::kInvalidInput,
                     "躲猫猫场景或容量无效");
  if ((training ? (spec.capacity != 1U || spec.user_hosted)
                : (spec.capacity < 2U || spec.capacity > 16U ||
                   spec.capacity % 2U != 0U)) ||
      spec.mode_id == 0U) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "房间容量或模式无效");
  }
  if (spec.user_hosted && (spec.capacity > 12U || spec.mode_id == 23U)) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "普通赏金房间不能覆盖原生固定观战席");
  }
  if (rooms_.size() >= kMaximumRooms) {
    return MakeError(application::ErrorCode::kOverloaded, "活动房间已达到上限");
  }
  const auto room_id = AllocateRoomId();
  if (!room_id.has_value()) {
    return MakeError(application::ErrorCode::kOverloaded, "没有可用房间标识");
  }

  auto room = std::shared_ptr<RoomRuntime>(
      new RoomRuntime(*room_id, host, std::move(spec)));
  rooms_.emplace(*room_id, room);
  membership_.emplace(host, *room_id);
  return room;
}

application::ServiceResult<std::shared_ptr<RoomRuntime>> RoomRegistry::Join(
    domain::PlayerId player_id, domain::RoomId room_id,
    std::string_view password) {
  if (!player_id.IsValid() || !room_id.IsValid()) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "玩家或房间标识无效");
  }
  if (membership_.contains(player_id)) {
    return MakeError(application::ErrorCode::kConflict, "玩家已经在其他房间");
  }
  const auto room_iterator = rooms_.find(room_id);
  if (room_iterator == rooms_.end()) {
    return MakeError(application::ErrorCode::kInvalidState, "房间已不存在");
  }
  auto& room = room_iterator->second;
  auto joined = room->AddMember(player_id, password);
  if (std::holds_alternative<application::ServiceError>(joined)) {
    return std::get<application::ServiceError>(std::move(joined));
  }
  membership_.emplace(player_id, room_id);
  return room;
}

std::optional<RoomDeparture> RoomRegistry::Leave(domain::PlayerId player_id) {
  const auto membership_iterator = membership_.find(player_id);
  if (membership_iterator == membership_.end()) {
    return std::nullopt;
  }
  const auto room_iterator = rooms_.find(membership_iterator->second);
  if (room_iterator == rooms_.end()) {
    membership_.erase(membership_iterator);
    return std::nullopt;
  }

  auto room = room_iterator->second;
  membership_.erase(membership_iterator);
  const auto state = room->RemoveMember(player_id);
  if (state.closed) {
    rooms_.erase(room_iterator);
  }
  return RoomDeparture{.room = std::move(room),
                       .departed_seat = state.departed_seat,
                       .new_host = state.new_host,
                       .start_interrupted = state.start_interrupted,
                       .room_removed = state.closed};
}

std::shared_ptr<RoomRuntime> RoomRegistry::FindByPlayer(
    domain::PlayerId player_id) const {
  const auto membership_iterator = membership_.find(player_id);
  if (membership_iterator == membership_.end()) {
    return {};
  }
  return FindById(membership_iterator->second);
}

std::shared_ptr<RoomRuntime> RoomRegistry::FindById(
    domain::RoomId room_id) const {
  const auto iterator = rooms_.find(room_id);
  return iterator == rooms_.end() ? std::shared_ptr<RoomRuntime>{}
                                  : iterator->second;
}

std::size_t RoomRegistry::RoomCount() const noexcept { return rooms_.size(); }

std::optional<domain::RoomId> RoomRegistry::AllocateRoomId() {
  if (isolated_room_) {
    if (isolated_room_used_) return std::nullopt;
    isolated_room_used_ = true;
    return isolated_room_;
  }
  for (std::uint64_t attempt = 0U; attempt < kMaximumWireRoomId; ++attempt) {
    next_room_id_ = next_room_id_ % kMaximumWireRoomId + 1U;
    const domain::RoomId candidate{next_room_id_};
    if (!rooms_.contains(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::string_view RoomPhaseName(RoomPhase phase) noexcept {
  switch (phase) {
    case RoomPhase::kWaiting:
      return "waiting";
    case RoomPhase::kLoading:
      return "loading";
    case RoomPhase::kCountdown:
      return "countdown";
    case RoomPhase::kPlaying:
      return "playing";
    case RoomPhase::kResults:
      return "results";
    case RoomPhase::kClosed:
      return "closed";
  }
  return "closed";
}

}  // namespace bf::runtime
