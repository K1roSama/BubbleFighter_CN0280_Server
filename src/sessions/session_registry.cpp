#include "sessions/session_registry.h"

#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace bf::runtime {
namespace {

[[nodiscard]] application::ServiceError MakeError(application::ErrorCode code,
                                                  std::string message) {
  return application::ServiceError{.code = code,
                                   .safe_message = std::move(message),
                                   .correlation_id = {},
                                   .is_retryable = false};
}

}  // namespace

class SessionRegistrySharedState final {
 public:
  explicit SessionRegistrySharedState(std::size_t capacity)
      : kMaximumSessions(capacity) {}

  std::mutex mutex;
  const std::size_t kMaximumSessions;
  std::uint64_t next_session_id{1U};
  std::map<SessionHandle, std::weak_ptr<SessionMailbox>> sessions;
  std::map<domain::PlayerId, SessionHandle> players;
  std::map<SessionHandle, domain::PlayerId> claims;
  std::map<SessionHandle, SocialPresenceSnapshot> social_capabilities;
  std::map<SessionHandle, WorldSessionState> world_states;
};

namespace {

void ReleaseClaim(const std::shared_ptr<SessionRegistrySharedState>& state,
                  SessionHandle handle, domain::PlayerId player_id) noexcept {
  std::scoped_lock lock{state->mutex};
  const auto claim = state->claims.find(handle);
  if (claim != state->claims.end() && claim->second == player_id) {
    state->claims.erase(claim);
  }
  const auto player = state->players.find(player_id);
  if (player != state->players.end() && player->second == handle) {
    state->players.erase(player);
  }
}

class SessionOnlinePlayerLease final : public OnlinePlayerLease {
 public:
  SessionOnlinePlayerLease(std::weak_ptr<SessionRegistrySharedState> state,
                           SessionHandle handle, domain::PlayerId player_id)
      : state_(std::move(state)), handle_(handle), player_id_(player_id) {}

  ~SessionOnlinePlayerLease() override {
    if (const auto state = state_.lock()) {
      ReleaseClaim(state, handle_, player_id_);
    }
  }

 private:
  std::weak_ptr<SessionRegistrySharedState> state_;
  SessionHandle handle_;
  domain::PlayerId player_id_;
};

class SessionOnlinePlayerRegistry final : public OnlinePlayerRegistry {
 public:
  SessionOnlinePlayerRegistry(std::weak_ptr<SessionRegistrySharedState> state,
                              SessionHandle handle)
      : state_(std::move(state)), handle_(handle) {}

  application::ServiceResult<std::unique_ptr<OnlinePlayerLease>> Claim(
      domain::PlayerId player_id) override {
    const auto state = state_.lock();
    if (!state || !handle_.IsValid() || !player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Session 已失效，不能认领在线玩家");
    }

    std::scoped_lock lock{state->mutex};
    const auto session = state->sessions.find(handle_);
    if (session == state->sessions.end() || session->second.expired()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Session 已注销，不能认领在线玩家");
    }
    if (state->claims.contains(handle_)) {
      return MakeError(application::ErrorCode::kConflict,
                       "同一 Session 已经认领一个在线玩家");
    }
    if (state->players.contains(player_id)) {
      return MakeError(application::ErrorCode::kConflict,
                       "玩家已经在其他 Session 在线");
    }

    try {
      state->players.emplace(player_id, handle_);
      state->claims.emplace(handle_, player_id);
      return std::unique_ptr<OnlinePlayerLease>{
          std::make_unique<SessionOnlinePlayerLease>(state, handle_,
                                                     player_id)};
    } catch (...) {
      const auto player = state->players.find(player_id);
      if (player != state->players.end() && player->second == handle_) {
        state->players.erase(player);
      }
      const auto claim = state->claims.find(handle_);
      if (claim != state->claims.end() && claim->second == player_id) {
        state->claims.erase(claim);
      }
      throw;
    }
  }

 private:
  std::weak_ptr<SessionRegistrySharedState> state_;
  SessionHandle handle_;
};

struct PendingDispatch final {
  std::vector<OutboundMessage> messages;
  std::optional<RoomSessionStage> room_stage;
  std::optional<PlazaSessionStage> plaza_stage;
  std::optional<WorldSessionPhase> world_phase;
  std::optional<std::uint32_t> channel;
};

struct ResolvedDispatch final {
  domain::PlayerId player_id;
  std::shared_ptr<SessionMailbox> mailbox;
  SessionDispatch dispatch;
};

void RemoveExpiredSession(SessionRegistrySharedState& state,
                          SessionHandle handle) {
  const auto claim = state.claims.find(handle);
  if (claim != state.claims.end()) {
    const auto player = state.players.find(claim->second);
    if (player != state.players.end() && player->second == handle) {
      state.players.erase(player);
    }
    state.claims.erase(claim);
  }
  state.sessions.erase(handle);
  state.social_capabilities.erase(handle);
  state.world_states.erase(handle);
}

[[nodiscard]] SessionDispatchReport PostPending(
    const std::shared_ptr<SessionRegistrySharedState>& state,
    std::map<domain::PlayerId, PendingDispatch> pending) {
  SessionDispatchReport report;
  std::vector<ResolvedDispatch> resolved;
  resolved.reserve(pending.size());
  {
    std::scoped_lock lock{state->mutex};
    for (auto& [player_id, batch] : pending) {
      const auto player = state->players.find(player_id);
      if (player == state->players.end()) {
        report.offline_players.push_back(player_id);
        continue;
      }
      const auto session = state->sessions.find(player->second);
      auto mailbox =
          session == state->sessions.end() ? nullptr : session->second.lock();
      if (!mailbox) {
        const auto stale_handle = player->second;
        RemoveExpiredSession(*state, stale_handle);
        report.offline_players.push_back(player_id);
        continue;
      }
      resolved.push_back({.player_id = player_id,
                          .mailbox = std::move(mailbox),
                          .dispatch = {.messages = std::move(batch.messages),
                                       .room_stage = batch.room_stage,
                                       .plaza_stage = batch.plaza_stage,
                                       .world_phase = batch.world_phase,
                                       .channel = batch.channel}});
      if (batch.world_phase || batch.channel) {
        auto& world = state->world_states[player->second];
        if (batch.world_phase) world.phase = *batch.world_phase;
        if (batch.channel) world.channel = *batch.channel;
      }
    }
  }

  for (auto& target : resolved) {
    try {
      if (auto error = target.mailbox->Post(std::move(target.dispatch));
          error.has_value()) {
        report.rejected_batches.push_back(
            {.player_id = target.player_id, .error = std::move(*error)});
      } else {
        ++report.submitted_sessions;
      }
    } catch (const std::exception&) {
      report.rejected_batches.push_back(
          {.player_id = target.player_id,
           .error = MakeError(application::ErrorCode::kInternal,
                              "Session mailbox 投递抛出异常")});
    } catch (...) {
      report.rejected_batches.push_back(
          {.player_id = target.player_id,
           .error = MakeError(application::ErrorCode::kInternal,
                              "Session mailbox 投递发生未知异常")});
    }
  }
  return report;
}

}  // namespace

bool SessionHandle::IsValid() const noexcept {
  return id != 0U && generation != 0U;
}

SessionRegistration::SessionRegistration(
    SessionHandle handle, std::weak_ptr<SessionRegistrySharedState> state)
    : handle_(handle), state_(std::move(state)) {}

SessionRegistration::~SessionRegistration() { Release(); }

SessionRegistration::SessionRegistration(SessionRegistration&& other) noexcept
    : handle_(other.handle_), state_(std::move(other.state_)) {
  other.handle_ = {};
}

SessionRegistration& SessionRegistration::operator=(
    SessionRegistration&& other) noexcept {
  if (this != &other) {
    Release();
    handle_ = other.handle_;
    state_ = std::move(other.state_);
    other.handle_ = {};
  }
  return *this;
}

SessionHandle SessionRegistration::Handle() const noexcept { return handle_; }

std::unique_ptr<OnlinePlayerRegistry>
SessionRegistration::MakeOnlinePlayerRegistry() const {
  return std::make_unique<SessionOnlinePlayerRegistry>(state_, handle_);
}

bool SessionRegistration::Active() const noexcept {
  return handle_.IsValid() && !state_.expired();
}

void SessionRegistration::Release() noexcept {
  const auto state = state_.lock();
  if (!state || !handle_.IsValid()) {
    handle_ = {};
    return;
  }
  {
    std::scoped_lock lock{state->mutex};
    RemoveExpiredSession(*state, handle_);
  }
  handle_ = {};
  state_.reset();
}

SessionRegistry::SessionRegistry(std::size_t maximum_sessions) {
  if (maximum_sessions == 0U) {
    throw std::invalid_argument{"session registry capacity must be positive"};
  }
  state_ = std::make_shared<SessionRegistrySharedState>(maximum_sessions);
}

application::ServiceResult<SessionRegistration>
SessionRegistry::RegisterSession(
    const std::shared_ptr<SessionMailbox>& mailbox) {
  if (!mailbox) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "不能注册空 Session mailbox");
  }
  std::scoped_lock lock{state_->mutex};
  if (state_->sessions.size() >= state_->kMaximumSessions) {
    return MakeError(application::ErrorCode::kOverloaded,
                     "活动 Session 已达到配置上限");
  }
  if (state_->next_session_id == std::numeric_limits<std::uint64_t>::max()) {
    return MakeError(application::ErrorCode::kOverloaded,
                     "Session 编号已经耗尽");
  }

  const SessionHandle handle{.id = state_->next_session_id, .generation = 1U};
  ++state_->next_session_id;
  state_->sessions.emplace(handle, mailbox);
  return SessionRegistration{handle, state_};
}

application::ServiceResult<SessionDispatchReport> SessionRegistry::Dispatch(
    RoomCoordinatorResult result) {
  std::map<domain::PlayerId, PendingDispatch> pending;
  for (auto& delivery : result.deliveries) {
    if (!delivery.recipient.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "房间投递包含无效玩家标识");
    }
    pending[delivery.recipient].messages.push_back(std::move(delivery.message));
  }
  for (const auto& update : result.session_updates) {
    if (!update.player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "房间阶段更新包含无效玩家标识");
    }
    auto& stage = pending[update.player_id].room_stage;
    if (stage.has_value() && *stage != update.stage) {
      return MakeError(application::ErrorCode::kInternal,
                       "同一房间结果包含冲突的 Session 阶段更新");
    }
    stage = update.stage;
  }

  return PostPending(state_, std::move(pending));
}

application::ServiceResult<SessionDispatchReport> SessionRegistry::Dispatch(
    PlazaCoordinatorResult result) {
  std::map<domain::PlayerId, PendingDispatch> pending;
  for (auto& delivery : result.deliveries) {
    if (!delivery.recipient.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "广场投递包含无效玩家标识");
    }
    pending[delivery.recipient].messages.push_back(std::move(delivery.message));
  }
  for (const auto& update : result.session_updates) {
    if (!update.player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "广场阶段更新包含无效玩家标识");
    }
    auto& stage = pending[update.player_id].plaza_stage;
    if (stage.has_value() && *stage != update.stage) {
      return MakeError(application::ErrorCode::kInternal,
                       "同一广场结果包含冲突的 Session 阶段更新");
    }
    stage = update.stage;
  }
  return PostPending(state_, std::move(pending));
}

application::ServiceResult<SessionDispatchReport> SessionRegistry::Dispatch(
    WorldCoordinatorResult result) {
  std::map<domain::PlayerId, PendingDispatch> pending;
  for (auto& delivery : result.deliveries) {
    if (!delivery.recipient.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "世界投递包含无效玩家标识");
    }
    pending[delivery.recipient].messages.push_back(std::move(delivery.message));
  }
  for (const auto& update : result.session_updates) {
    if (!update.player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "世界阶段更新包含无效玩家标识");
    }
    auto& batch = pending[update.player_id];
    if (batch.world_phase.has_value() && *batch.world_phase != update.phase) {
      return MakeError(application::ErrorCode::kInternal,
                       "同一世界结果包含冲突的 Session 阶段更新");
    }
    if (batch.channel.has_value() && update.channel.has_value() &&
        *batch.channel != *update.channel) {
      return MakeError(application::ErrorCode::kInternal,
                       "同一世界结果包含冲突的 Session 频道更新");
    }
    batch.world_phase = update.phase;
    if (update.channel.has_value()) {
      batch.channel = update.channel;
    }
  }
  return PostPending(state_, std::move(pending));
}

application::ServiceResult<SessionDispatchReport> SessionRegistry::Dispatch(
    SocialCommandResult result) {
  std::map<domain::PlayerId, PendingDispatch> pending;
  for (auto& delivery : result.deliveries) {
    if (!delivery.recipient.IsValid()) {
      return MakeError(application::ErrorCode::kInternal,
                       "社交投递包含无效玩家标识");
    }
    pending[delivery.recipient].messages.push_back(std::move(delivery.message));
  }
  return PostPending(state_, std::move(pending));
}

std::optional<application::ServiceError> SessionRegistry::SetSocialCapabilities(
    SessionHandle handle, bool extended, bool native_social) {
  if (!handle.IsValid() || (native_social && !extended)) {
    return MakeError(application::ErrorCode::kInvalidInput,
                     "Session 社交能力参数无效");
  }
  std::scoped_lock lock{state_->mutex};
  const auto session = state_->sessions.find(handle);
  const auto claim = state_->claims.find(handle);
  if (session == state_->sessions.end() || session->second.expired() ||
      claim == state_->claims.end()) {
    return MakeError(application::ErrorCode::kInvalidState,
                     "Session 尚未认领在线玩家，不能发布社交能力");
  }
  state_->social_capabilities[handle] = {
      .online = true, .extended = extended, .native_social = native_social};
  return std::nullopt;
}

SocialPresenceSnapshot SessionRegistry::Lookup(
    domain::PlayerId player_id) const {
  if (!player_id.IsValid()) {
    return {};
  }
  std::scoped_lock lock{state_->mutex};
  const auto player = state_->players.find(player_id);
  if (player == state_->players.end()) {
    return {};
  }
  const auto capability = state_->social_capabilities.find(player->second);
  if (capability == state_->social_capabilities.end()) {
    return {.online = true, .extended = false, .native_social = false};
  }
  return capability->second;
}

bool SessionRegistry::IsCurrentPlayerSession(domain::PlayerId player_id,
                                             SessionHandle handle) const {
  if (!player_id.IsValid() || !handle.IsValid()) {
    return false;
  }
  std::scoped_lock lock{state_->mutex};
  const auto player = state_->players.find(player_id);
  if (player == state_->players.end() || player->second != handle) {
    return false;
  }
  const auto session = state_->sessions.find(handle);
  return session != state_->sessions.end() && !session->second.expired();
}

std::optional<SessionHandle> SessionRegistry::CurrentPlayerSession(
    domain::PlayerId player) const {
  std::scoped_lock lock{state_->mutex};
  const auto found = state_->players.find(player);
  if (found == state_->players.end()) return std::nullopt;
  const auto session = state_->sessions.find(found->second);
  if (session == state_->sessions.end() || session->second.expired())
    return std::nullopt;
  return found->second;
}

std::size_t SessionRegistry::SessionCount() const {
  std::scoped_lock lock{state_->mutex};
  return state_->sessions.size();
}

void SessionRegistry::SetWorldState(SessionHandle handle,
                                    WorldSessionState world) {
  std::scoped_lock lock{state_->mutex};
  if (state_->claims.contains(handle)) state_->world_states[handle] = world;
}

application::ServiceResult<bool> SessionRegistry::DispatchIfCurrent(
    domain::PlayerId player, SessionHandle handle,
    std::vector<OutboundMessage> messages) {
  return DispatchIfCurrent(player, handle,
                           SessionDispatch{.messages = std::move(messages)});
}

application::ServiceResult<bool> SessionRegistry::DispatchIfCurrent(
    domain::PlayerId player, SessionHandle handle, SessionDispatch dispatch) {
  std::shared_ptr<SessionMailbox> mailbox;
  {
    std::scoped_lock lock{state_->mutex};
    const auto claim = state_->players.find(player);
    if (claim == state_->players.end() || claim->second != handle) return false;
    const auto found = state_->sessions.find(handle);
    if (found == state_->sessions.end()) return false;
    mailbox = found->second.lock();
    if (mailbox && (dispatch.world_phase || dispatch.channel)) {
      auto& world = state_->world_states[handle];
      if (dispatch.world_phase) world.phase = *dispatch.world_phase;
      if (dispatch.channel) world.channel = *dispatch.channel;
    }
  }
  if (!mailbox) return false;
  if (auto error = mailbox->Post(std::move(dispatch))) return *error;
  return true;
}

std::optional<WorldSessionState> SessionRegistry::WorldState(
    domain::PlayerId player) const {
  std::scoped_lock lock{state_->mutex};
  const auto claim = state_->players.find(player);
  if (claim == state_->players.end()) return std::nullopt;
  const auto session = state_->sessions.find(claim->second);
  if (session == state_->sessions.end() || session->second.expired())
    return std::nullopt;
  const auto found = state_->world_states.find(claim->second);
  return found == state_->world_states.end() ? std::nullopt
                                             : std::optional{found->second};
}

std::size_t SessionRegistry::OnlinePlayerCount() const {
  std::scoped_lock lock{state_->mutex};
  return state_->players.size();
}

std::vector<std::pair<domain::PlayerId, SessionHandle>>
SessionRegistry::OnlineSessions() const {
  std::scoped_lock lock{state_->mutex};
  std::vector<std::pair<domain::PlayerId, SessionHandle>> result;
  for (const auto& entry : state_->players)
    if (state_->social_capabilities.contains(entry.second))
      result.push_back(entry);
  return result;
}

void SessionRegistry::DisconnectIfCurrent(domain::PlayerId player,
                                          SessionHandle handle) {
  std::shared_ptr<SessionMailbox> mailbox;
  {
    std::scoped_lock lock{state_->mutex};
    const auto found = state_->players.find(player);
    if (found == state_->players.end() || found->second != handle) return;
    const auto session = state_->sessions.find(handle);
    if (session != state_->sessions.end()) mailbox = session->second.lock();
  }
  if (mailbox)
    static_cast<void>(mailbox->Post(SessionDispatch{.disconnect = true}));
}

}  // namespace bf::runtime
