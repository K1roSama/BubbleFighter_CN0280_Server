#include "sessions/world_executor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "features/chat/message_policy.h"
#include "protocol/battle/combat_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"

namespace bf::runtime {
namespace {

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

using TimerKey = std::pair<domain::RoomId, RoomTimerKind>;
using SteadyTime = std::chrono::steady_clock::time_point;

/** @brief worker 每次持锁从共享队列批量取出的任务上限，降低与投递线程的锁竞争。
 */
constexpr std::size_t kMaxTasksPerTurn = 64U;

/**
 * @brief 按房间和计时种类去重、按截止点有序的计时表。
 * @note 只由 world worker 访问；替换同键计时器时同步删除旧排序项，取最早项为
 * O(log n)。
 */
class RoomTimerTable final {
 public:
  void Upsert(const RoomTimerToken& token) {
    const TimerKey key{token.room_id, token.kind};
    if (const auto found = by_key_.find(key); found != by_key_.end()) {
      by_deadline_.erase({found->second.deadline, key});
      found->second = token;
    } else {
      by_key_.emplace(key, token);
    }
    by_deadline_.emplace(token.deadline, key);
  }

  [[nodiscard]] std::optional<RoomTimerToken> PopDue(SteadyTime now) {
    if (by_deadline_.empty() || by_deadline_.begin()->first > now) {
      return std::nullopt;
    }
    const auto key = by_deadline_.begin()->second;
    by_deadline_.erase(by_deadline_.begin());
    auto node = by_key_.extract(key);
    return node.mapped();
  }

  [[nodiscard]] std::optional<SteadyTime> NextDeadline() const {
    if (by_deadline_.empty()) {
      return std::nullopt;
    }
    return by_deadline_.begin()->first;
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_key_.size(); }

 private:
  std::map<TimerKey, RoomTimerToken> by_key_;
  std::set<std::pair<SteadyTime, TimerKey>> by_deadline_;
};

template <typename Key>
std::optional<Key> PopDueRetry(std::map<Key, SteadyTime>& retries,
                               SteadyTime now) {
  const auto earliest = std::ranges::min_element(
      retries, {}, [](const auto& entry) { return entry.second; });
  if (earliest == retries.end() || earliest->second > now) {
    return std::nullopt;
  }
  auto key = earliest->first;
  retries.erase(earliest);
  return key;
}

template <typename Key>
std::optional<SteadyTime> EarliestRetry(
    const std::map<Key, SteadyTime>& retries) {
  const auto earliest = std::ranges::min_element(
      retries, {}, [](const auto& entry) { return entry.second; });
  if (earliest == retries.end()) {
    return std::nullopt;
  }
  return earliest->second;
}

bool SenderFailed(const SessionDispatchReport& report,
                  domain::PlayerId sender) {
  return std::ranges::find(report.offline_players, sender) !=
             report.offline_players.end() ||
         std::ranges::any_of(report.rejected_batches,
                             [sender](const auto& failure) {
                               return failure.player_id == sender;
                             });
}

struct MatchStartSuccess final {
  application::PreparedMatch prepared;
  std::optional<application::MatchStartResult> persisted;
  std::optional<application::PlayerMenuSnapshot> sponsor_snapshot;
  std::optional<application::ServiceError> asset_error;
};

struct UncertainPrizeStart final {
  domain::MatchId match_id;
  application::ServiceError error;
};

using MatchStartWorkResult =
    std::variant<application::ServiceError, MatchStartSuccess,
                 UncertainPrizeStart>;

struct PendingTutorialWork final {
  WorldCoordinatorResult result;
  std::optional<domain::PlayerId> sender;
  WorldCommandCompletion completion;
  bool in_flight{false};
  std::optional<SessionHandle> origin;
};

struct PendingEquipmentStart final {
  std::chrono::steady_clock::time_point started_at;
  WorldCoordinatorResult result;
  std::optional<domain::PlayerId> sender;
  WorldCommandCompletion completion;
  MatchStartSuccess success;
  std::chrono::steady_clock::time_point deadline;
};

struct DeferredDisconnect final {
  domain::PlayerId player_id;
  WorldSessionPhase phase{WorldSessionPhase::kMenu};
};

struct PendingSettlement final {
  WorldCoordinatorResult result;
  std::optional<domain::PlayerId> sender;
  WorldCommandCompletion completion;
  std::size_t failed_attempts{0U};
};

/** @brief 已提交开局无法安装时，由 world worker
 * 持有直到数据库中断记录成功的补偿上下文。 */
struct PendingMatchCompensation final {
  domain::MatchId match_id;
  std::chrono::system_clock::time_point interrupted_at;
  std::optional<domain::PlayerId> sender;
  WorldCommandCompletion completion;
  application::ServiceError install_error;
  std::size_t failed_attempts{0U};
};

}  // namespace

class WorldExecutor::Impl final {
 public:
  Impl(WorldExecutorOptions options, SessionRegistry& sessions,
       application::BackgroundTaskExecutor& background_tasks,
       application::MatchPreparationExecutor& match_preparation,
       application::MatchIdSource& match_ids,
       application::MatchStartExecutor& match_starts,
       application::MatchLifecycleRepairExecutor& match_repairs,
       const application::SystemClock& clock,
       DurableMatchSettlementFlow& settlements,
       application::EventLogger& logger,
       application::ItemUseExecutor* megaphones,
       application::WorldAccessLoader* megaphone_access)
      : options_(options),
        sessions_(sessions),
        background_tasks_(background_tasks),
        match_preparation_(match_preparation),
        match_ids_(match_ids),
        match_starts_(match_starts),
        match_repairs_(match_repairs),
        clock_(clock),
        settlements_(settlements),
        logger_(logger),
        world_(options.match_timing),
        megaphones_(megaphones),
        megaphone_access_(megaphone_access) {
    if (options_.queue_capacity == 0U || options_.queue_capacity > 1'000'000U ||
        options_.settlement_retry_delay <= std::chrono::milliseconds::zero() ||
        options_.start_compensation_retry_delay <=
            std::chrono::milliseconds::zero() ||
        options_.recovery_interval <= std::chrono::milliseconds::zero() ||
        options_.recovery_batch_size == 0U ||
        options_.recovery_batch_size > 4'096U) {
      throw std::invalid_argument{"invalid world executor options"};
    }
  }

  ~Impl() { DrainAndStop(); }

  std::optional<application::ServiceError> Start() {
    std::scoped_lock lock{mutex_};
    if (started_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "世界执行器只能启动一次");
    }
    started_ = true;
    accepting_ = true;
    next_recovery_ =
        std::chrono::steady_clock::now() + options_.recovery_interval;
    worker_ = std::thread{[this] { WorkerLoop(); }};
    logger_.Log(application::LogLevel::kDebug, "world executor started");
    return std::nullopt;
  }

  std::optional<application::ServiceError> Post(
      WorldActorContext actor, protocol::Packet packet,
      WorldCommandCompletion completion) {
    if (!completion || !actor.player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "世界命令上下文或回调无效");
    }
    if (options_.room_execution &&
        options_.room_execution->Routes(actor, packet))
      return options_.room_execution->Post(std::move(actor), std::move(packet),
                                           std::move(completion));
    return EnqueueExternal([this, actor = std::move(actor),
                            packet = std::move(packet),
                            completion = std::move(completion)]() mutable {
      ExecutePacket(std::move(actor), packet, std::move(completion));
    });
  }

  std::optional<application::ServiceError> PostPlayerOnline(
      application::PlayerMenuSnapshot snapshot,
      std::optional<SessionHandle> origin) {
    const auto player = snapshot.player_id;
    const auto expected =
        origin ? origin : sessions_.CurrentPlayerSession(player);
    return EnqueueExternal([this, player, expected,
                            snapshot = std::move(snapshot)]() mutable {
      if (expected && !sessions_.IsCurrentPlayerSession(player, *expected))
        return;
      const auto previous = online_gateway_sessions_.find(player);
      if (expected && (previous == online_gateway_sessions_.end() ||
                       previous->second != *expected)) {
        online_gateway_sessions_.insert_or_assign(player, *expected);
        world_.GatewayPlayerOnline(std::move(snapshot));
      } else {
        world_.PlayerOnline(std::move(
            snapshot));  // Asset refresh on the same handle preserves phase.
      }
    });
  }

  std::optional<application::ServiceError> PostCoupleProfiles(
      std::vector<application::PlayerMenuSnapshot> snapshots) {
    return EnqueueExternal([this, snapshots = std::move(snapshots)]() mutable {
      for (auto& snapshot : snapshots) {
        auto result = world_.CoupleChanged(std::move(snapshot));
        if (const auto* error =
                std::get_if<application::ServiceError>(&result)) {
          logger_.Log(application::LogLevel::kWarning,
                      "couple room refresh failed: " + error->safe_message);
          continue;
        }
        const auto sent = sessions_.Dispatch(
            std::get<WorldCoordinatorResult>(std::move(result)));
        if (const auto* error = std::get_if<application::ServiceError>(&sent))
          logger_.Log(application::LogLevel::kWarning,
                      "couple room dispatch failed: " + error->safe_message);
      }
    });
  }

  std::optional<application::ServiceError> PostDisconnect(
      domain::PlayerId player_id, WorldSessionPhase phase,
      std::optional<SessionHandle> origin) {
    if (!player_id.IsValid()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "断线清理缺少玩家标识");
    }
    if (options_.room_execution)
      options_.room_execution->Disconnect(player_id, origin);
    return EnqueueExternal([this, player_id, phase, origin] {
      ExecuteDisconnect(player_id, phase, origin);
    });
  }

  std::optional<application::ServiceError> PostRoomHandoff(
      WorldActorContext actor, bool entering,
      WorldCommandCompletion completion) {
    if (!actor.orig_session || !completion)
      return MakeError(application::ErrorCode::kInvalidInput,
                       "房间移交缺少网关会话");
    return EnqueueExternal([this, actor = std::move(actor), entering,
                            completion]() mutable {
      const SessionHandle origin{actor.orig_session->first,
                                 actor.orig_session->second};
      if (!sessions_.IsCurrentPlayerSession(actor.player_id, origin)) {
        completion(MakeError(application::ErrorCode::kInvalidState,
                             "房间移交会话已过期"));
        return;
      }
      auto result = entering ? world_.EnterDedicatedRoom(actor)
                             : world_.LeaveDedicatedRoom(std::move(actor));
      if (const auto* error = std::get_if<application::ServiceError>(&result))
        completion(*error);
      else {
        const auto sent = sessions_.Dispatch(
            std::get<WorldCoordinatorResult>(std::move(result)));
        if (const auto* dispatch_error =
                std::get_if<application::ServiceError>(&sent))
          completion(*dispatch_error);
        else
          completion(std::vector<OutboundMessage>{});
      }
    });
  }

  void DrainAndStop() noexcept {
    {
      std::scoped_lock lock{mutex_};
      if (stopping_) {
        return;
      }
      // 计时表和重试表只属于 worker；停止后 worker
      // 不再触发它们，析构时统一释放。
      accepting_ = false;
      stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    // Sockets are stopped first by the composition root. Complete held
    // callbacks even if the ordered acknowledgement never arrives; durable
    // started rows use startup recovery.
    for (auto& [room, pending] : pending_equipment_starts_) {
      options_.equipment_relay->RevokeRoom(room);
      if (pending.completion) {
        try {
          pending.completion(MakeError(application::ErrorCode::kInvalidState,
                                       "服务关闭，装备初始化已取消"));
        } catch (...) {
          logger_.Log(application::LogLevel::kWarning,
                      "equipment close callback failed");
        }
      }
    }
    pending_equipment_starts_.clear();
    logger_.Log(application::LogLevel::kDebug, "world executor stopped");
  }

  bool PostSettlementResult(
      application::MatchSettlementResult result) noexcept {
    try {
      if (options_.room_execution &&
          options_.room_execution->SettlementResult(result))
        return true;
      return EnqueueInternal([this, result = std::move(result)]() mutable {
        for (const auto& [room_id, pending] : pending_settlements_) {
          if (pending.result.settlement_command.has_value() &&
              pending.result.settlement_command->match_id == result.match_id) {
            early_reward_results_.insert_or_assign(room_id, std::move(result));
            return;
          }
        }
        ApplyRewardResult(result);
      });
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "could not retain committed reward result");
      return false;
    }
  }

  void ApplyRewardResult(const application::MatchSettlementResult& result) {
    auto applied = world_.ApplySettlementResult(result);
    if (const auto* error = std::get_if<application::ServiceError>(&applied)) {
      logger_.Log(application::LogLevel::kError,
                  "reward display update failed: " + error->safe_message);
      return;
    }
    auto display = std::get<WorldCoordinatorResult>(std::move(applied));
    const bool refresh_ticket =
        !display.deliveries.empty() && result.user_host_prize &&
        result.user_host_prize->refunded && options_.prize_menus;
    auto delivered = Dispatch(std::move(display), std::nullopt);
    if (refresh_ticket) {
      const auto sponsor = result.user_host_prize->sponsor_player_id;
      const auto queued = background_tasks_.Post([this, sponsor] {
        try {
          auto loaded = options_.prize_menus->Load(sponsor);
          if (const auto* error =
                  std::get_if<application::ServiceError>(&loaded)) {
            logger_.Log(application::LogLevel::kError,
                        "退款库存刷新失败: " + error->safe_message);
            return;  // Durable debt and the next normal menu refresh remain
                     // authoritative.
          }
          auto snapshot =
              std::get<application::PlayerMenuSnapshot>(std::move(loaded));
          static_cast<void>(
              EnqueueInternal([this, snapshot = std::move(snapshot)]() mutable {
                auto assets = world_.AssetsCommitted(std::move(snapshot));
                if (auto* value = std::get_if<WorldCoordinatorResult>(&assets))
                  RouteResult(std::move(*value), std::nullopt, {},
                              std::chrono::steady_clock::now());
              }));
        } catch (...) {
          logger_.Log(application::LogLevel::kError,
                      "退款库存刷新发生异常，资产仍可通过菜单刷新领取");
        }
      });
      if (queued)
        logger_.Log(application::LogLevel::kError,
                    "退款库存刷新队列已满，资产仍可通过菜单刷新领取");
    }
    if (const auto* error =
            std::get_if<application::ServiceError>(&delivered)) {
      logger_.Log(application::LogLevel::kError,
                  "reward display dispatch failed: " + error->safe_message);
    }
  }

  [[nodiscard]] std::size_t QueuedTaskCount() const noexcept {
    std::scoped_lock lock{mutex_};
    return tasks_.size();
  }

  [[nodiscard]] WorldExecutorMetrics Metrics() const noexcept {
    return {
        .queued_tasks = QueuedTaskCount(),
        .active_rooms = active_rooms_.load(std::memory_order_relaxed),
        .plaza_players = plaza_players_.load(std::memory_order_relaxed),
        .scheduled_timers = scheduled_timers_.load(std::memory_order_relaxed),
        .pending_match_starts =
            pending_match_starts_metric_.load(std::memory_order_relaxed),
        .pending_match_settlements =
            pending_match_settlements_metric_.load(std::memory_order_relaxed),
        .pending_match_compensations = pending_match_compensations_metric_.load(
            std::memory_order_relaxed)};
  }

 private:
  std::optional<application::ServiceError> EnqueueExternal(
      std::function<void()> task) {
    {
      std::scoped_lock lock{mutex_};
      if (!accepting_) {
        return MakeError(application::ErrorCode::kInvalidState,
                         "世界执行器尚未启动或正在停止");
      }
      if (tasks_.size() >= options_.queue_capacity) {
        return MakeError(application::ErrorCode::kOverloaded,
                         "世界命令队列已满", true);
      }
      tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
    return std::nullopt;
  }

  bool EnqueueInternal(std::function<void()> task) noexcept {
    try {
      {
        std::scoped_lock lock{mutex_};
        if (stopping_) {
          return false;
        }
        // 内部 completion
        // 数量受后台线程数和房间数约束；不得因外部队列恰好满而丢失已提交
        // 数据库工作的唯一回执，否则房间会永久停留在过渡态。
        tasks_.push_back(std::move(task));
      }
      wake_.notify_one();
      return true;
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "world executor could not retain a persistence completion");
      return false;
    }
  }

  bool RoomHasPendingExternalWork(domain::RoomId room_id) const {
    return pending_match_starts_.contains(room_id) ||
           pending_settlements_.contains(room_id) ||
           pending_equipment_rooms_.contains(room_id);
  }

 public:
  std::optional<application::ServiceError> PostMegaphone(
      application::MegaphoneCommand command, MegaphoneWorldTask work,
      WorldCommandCompletion completion) {
    if (!megaphones_ || !megaphone_access_ || !work || !completion) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "喇叭事务服务未装配");
    }
    return EnqueueExternal([this, command = std::move(command),
                            work = std::move(work),
                            completion = std::move(completion)]() mutable {
      if (megaphone_sent_.size() >= 16384U) {
        completion(MakeError(application::ErrorCode::kOverloaded,
                             "喇叭投递正在恢复，请稍后重试", true));
        return;
      }
      auto prepared = world_.PrepareMegaphone(std::move(command));
      if (const auto* error =
              std::get_if<application::ServiceError>(&prepared)) {
        completion(*error);
        return;
      }
      ++megaphone_work_in_flight_;
      const auto failure = background_tasks_.Post(
          [this,
           command =
               std::get<application::MegaphoneCommand>(std::move(prepared)),
           work, completion] {
            application::ServiceResult<MegaphoneWorldResult> result;
            try {
              result = work(command);
            } catch (...) {
              result = MakeError(application::ErrorCode::kInternal,
                                 "喇叭事务任务失败");
            }
            static_cast<void>(EnqueueInternal([this, result = std::move(result),
                                               completion]() mutable {
              --megaphone_work_in_flight_;
              if (const auto* error =
                      std::get_if<application::ServiceError>(&result)) {
                PruneMegaphoneCache();
                completion(*error);
                return;
              }
              auto value = std::get<MegaphoneWorldResult>(std::move(result));
              if (value.receipt)
                DeliverMegaphone(*value.receipt, value.blocked);
              PruneMegaphoneCache();
              completion(std::move(value.replies));
            }));
          });
      if (failure) {
        --megaphone_work_in_flight_;
        completion(*failure);
      }
    });
  }

 private:
  void PruneMegaphoneCache() {
    // 只回收已耐久确认且不存在旧快照在途的记录；回执失败不能丢弃防重信息。
    if (megaphone_work_in_flight_ != 0U || recovery_in_flight_) return;
    for (auto it = megaphone_completed_order_.begin();
         it != megaphone_completed_order_.end() &&
         megaphone_completed_order_.size() > 8192U;) {
      if (!megaphone_acknowledged_.contains(*it)) {
        ++it;
        continue;
      }
      megaphone_sent_.erase(*it);
      megaphone_completed_.erase(*it);
      megaphone_acknowledged_.erase(*it);
      it = megaphone_completed_order_.erase(it);
    }
  }

  void DeliverMegaphone(const application::MegaphoneReceipt& receipt,
                        const std::vector<domain::PlayerId>& blocked) {
    if (receipt.published) return;
    const auto& id = receipt.business_id;
    auto& sent = megaphone_sent_[id];
    if (!megaphone_completed_.contains(id)) {
      auto deliveries = world_.MegaphoneDeliveries(receipt, blocked);
      // 重启后可能尚无人重连；保留待办，不能把没有接收者当作成功广播。
      if (deliveries.deliveries.empty() && sent.empty()) return;
      std::erase_if(deliveries.deliveries, [&sent](const auto& value) {
        return sent.contains(value.recipient);
      });
      std::vector<domain::PlayerId> attempted;
      for (const auto& value : deliveries.deliveries)
        attempted.push_back(value.recipient);
      auto dispatched = sessions_.Dispatch(std::move(deliveries));
      if (const auto* error =
              std::get_if<application::ServiceError>(&dispatched)) {
        logger_.Log(application::LogLevel::kWarning,
                    "megaphone dispatch deferred: " + error->safe_message);
        return;
      }
      const auto& report = std::get<SessionDispatchReport>(dispatched);
      for (auto player : attempted) {
        if (std::ranges::none_of(report.rejected_batches,
                                 [player](const auto& failure) {
                                   return failure.player_id == player;
                                 })) {
          sent.insert(player);
        }
      }
      if (!report.rejected_batches.empty()) return;
      megaphone_completed_.insert(id);
      megaphone_completed_order_.push_back(id);
    }
    const auto queued = background_tasks_.Post([this, id] {
      if (const auto error = megaphones_->AcknowledgeMegaphone(id)) {
        logger_.Log(
            application::LogLevel::kWarning,
            "megaphone acknowledgement deferred: " + error->safe_message);
      } else {
        static_cast<void>(EnqueueInternal([this, id] {
          if (megaphone_completed_.contains(id))
            megaphone_acknowledged_.insert(id);
          PruneMegaphoneCache();
        }));
      }
    });
    if (queued)
      logger_.Log(application::LogLevel::kWarning,
                  "megaphone acknowledgement queue full");
  }

 public:
  std::optional<application::ServiceError> PostInstantRespawn(
      domain::PlayerId player, std::uint32_t object,
      InstantRespawnWorldTask work, WorldCommandCompletion completion) {
    if (!work || !completion || !player.IsValid()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "即时复活任务缺少身份或回调");
    }
    if (options_.room_execution && options_.room_execution->Contains(player))
      return options_.room_execution->InstantRespawn(
          player, object, std::move(work), std::move(completion));
    return EnqueueExternal([this, player, object, work = std::move(work),
                            completion = std::move(completion)] {
      ExecuteInstantRespawn(player, object, work, completion);
    });
  }

 private:
  void ExecuteInstantRespawn(domain::PlayerId player, std::uint32_t object,
                             InstantRespawnWorldTask work,
                             WorldCommandCompletion completion) {
    const auto room = world_.Rooms().FindRoom(player);
    if (room && pending_equipment_rooms_.contains(room->RoomId())) {
      auto& deferred = deferred_equipment_commands_[room->RoomId()];
      if (deferred.size() >= options_.queue_capacity) {
        completion(MakeError(application::ErrorCode::kOverloaded,
                             "即时复活等待队列已满", true));
        return;
      }
      deferred.push_back([this, player, object, work, completion] {
        ExecuteInstantRespawn(player, object, work, completion);
      });
      return;
    }
    const auto command = world_.Rooms().PrepareInstantRespawn(player, object);
    if (!room || !command || RoomHasPendingExternalWork(room->RoomId())) {
      completion(std::vector<OutboundMessage>{});
      return;
    }
    const auto reserved = room->RoomId();
    pending_equipment_rooms_.insert(reserved);
    const auto failure = background_tasks_.Post([this, command = *command, work,
                                                 completion, reserved] {
      application::ServiceResult<InstantRespawnWorldResult> result;
      try {
        result = work(command);
      } catch (...) {
        result = MakeError(application::ErrorCode::kInternal,
                           "即时复活数据库任务失败");
      }
      static_cast<void>(EnqueueInternal([this, command, completion, reserved,
                                         result = std::move(result)]() mutable {
        pending_equipment_rooms_.erase(reserved);
        if (const auto* error =
                std::get_if<application::ServiceError>(&result)) {
          completion(*error);
        } else {
          auto value = std::get<InstantRespawnWorldResult>(std::move(result));
          if (value.authorized && !world_.AuthorizeInstantRespawn(command)) {
            completion(MakeError(application::ErrorCode::kInternal,
                                 "即时复活提交时生命状态发生变化"));
          } else {
            completion(std::move(value.replies));
          }
        }
        ResumeEquipmentCommands(reserved);
        ProcessDeferredDisconnects(reserved);
      }));
    });
    if (failure) {
      pending_equipment_rooms_.erase(reserved);
      completion(*failure);
      ResumeEquipmentCommands(reserved);
    }
  }

 public:
  std::optional<application::ServiceError> PostEquipmentChange(
      domain::PlayerId player, EquipmentWorldTask work,
      WorldCommandCompletion completion, bool preset_selection,
      std::optional<SessionHandle> origin) {
    if (!work || !completion || !player.IsValid()) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "换装任务缺少身份或回调");
    }
    if (options_.room_execution && options_.room_execution->Contains(player))
      return options_.room_execution->Equipment(player, std::move(work),
                                                std::move(completion),
                                                preset_selection, origin);
    return EnqueueExternal([this, player, preset_selection, origin,
                            work = std::move(work),
                            completion = std::move(completion)] {
      if (origin && !sessions_.IsCurrentPlayerSession(player, *origin)) {
        completion(std::vector<OutboundMessage>{});
        return;
      }
      const auto room = world_.Rooms().FindRoom(player);
      const bool allowed =
          !world_.HasTutorial(player) &&
          (!room || (!RoomHasPendingExternalWork(room->RoomId()) &&
                     (room->Phase() == RoomPhase::kWaiting ||
                      (preset_selection &&
                       world_.Rooms().CanChangeCombatPreset(player)))));
      const auto combat_mode =
          room && room->Phase() == RoomPhase::kPlaying ? room->ModeId() : 0U;
      const auto reserved =
          allowed && room ? std::optional{room->RoomId()} : std::nullopt;
      if (reserved) {
        pending_equipment_rooms_.insert(*reserved);
      }
      const auto failure = background_tasks_.Post([this, player, work,
                                                   completion, allowed, origin,
                                                   combat_mode, reserved] {
        application::ServiceResult<EquipmentWorldResult> result;
        try {
          result = work(allowed, combat_mode);
        } catch (const std::exception&) {
          result = MakeError(application::ErrorCode::kInternal,
                             "换装数据库任务失败");
        } catch (...) {
          result = MakeError(application::ErrorCode::kInternal,
                             "换装数据库任务发生未知错误");
        }
        static_cast<void>(EnqueueInternal([this, player, completion, reserved,
                                           origin,
                                           result =
                                               std::move(result)]() mutable {
          if (reserved) {
            pending_equipment_rooms_.erase(*reserved);
          }
          if (const auto* error =
                  std::get_if<application::ServiceError>(&result)) {
            completion(*error);
          } else {
            auto value = std::get<EquipmentWorldResult>(std::move(result));
            std::optional<application::ServiceError> mark_dispatch_error;
            for (const auto member : value.clan_members) {
              auto refreshed = world_.ClanMarkChanged(member, value.clan_mark);
              auto sent = Dispatch(std::move(refreshed), player);
              if (auto* self =
                      std::get_if<std::vector<OutboundMessage>>(&sent)) {
                value.replies.insert(value.replies.end(),
                                     std::make_move_iterator(self->begin()),
                                     std::make_move_iterator(self->end()));
              } else if (!mark_dispatch_error) {
                mark_dispatch_error =
                    std::get<application::ServiceError>(std::move(sent));
              }
            }
            if (mark_dispatch_error) {
              completion(*mark_dispatch_error);
            } else if (value.changed &&
                       (!origin ||
                        sessions_.IsCurrentPlayerSession(player, *origin))) {
              auto refreshed =
                  value.level_icon_only
                      ? world_.LevelIconChanged(std::move(*value.changed))
                      : world_.EquipmentChanged(std::move(*value.changed),
                                                value.refresh_menu_slot);
              if (const auto* refresh_error =
                      std::get_if<application::ServiceError>(&refreshed)) {
                completion(*refresh_error);
              } else {
                auto updates =
                    std::get<WorldCoordinatorResult>(std::move(refreshed));
                // 本人的换装确认、模型、库存和席位必须走同一个完成批次，不能让
                // mailbox 抢先发送。
                std::erase_if(
                    updates.deliveries, [&value, player](auto& delivery) {
                      if (delivery.recipient != player) {
                        return false;
                      }
                      value.replies.push_back(std::move(delivery.message));
                      return true;
                    });
                auto dispatched = Dispatch(std::move(updates), player);
                if (const auto* dispatch_error =
                        std::get_if<application::ServiceError>(&dispatched)) {
                  completion(*dispatch_error);
                } else {
                  auto self = std::get<std::vector<OutboundMessage>>(
                      std::move(dispatched));
                  value.replies.insert(value.replies.end(),
                                       std::make_move_iterator(self.begin()),
                                       std::make_move_iterator(self.end()));
                  completion(std::move(value.replies));
                }
              }
            } else {
              completion(std::move(value.replies));
            }
          }
          if (reserved) {
            ResumeEquipmentCommands(*reserved);
            ProcessDeferredDisconnects(*reserved);
          }
        }));
      });
      if (failure) {
        if (reserved) {
          pending_equipment_rooms_.erase(*reserved);
          ResumeEquipmentCommands(*reserved);
        }
        completion(*failure);
      }
    });
  }

 private:
  void ResumeEquipmentCommands(domain::RoomId room) {
    auto found = deferred_equipment_commands_.find(room);
    if (found == deferred_equipment_commands_.end()) {
      return;
    }
    auto commands = std::move(found->second);
    deferred_equipment_commands_.erase(found);
    for (auto& command : commands) {
      command();
    }
  }

  void RejectAndKeepConnection(domain::PlayerId sender,
                               const protocol::Packet& packet,
                               std::string_view detail,
                               WorldCommandCompletion completion) {
    const auto now = std::chrono::steady_clock::now();
    const auto previous = rejection_logs_.find(sender);
    if (previous != rejection_logs_.end() &&
        now - previous->second < std::chrono::seconds{5}) {
      completion(std::vector<OutboundMessage>{});
      return;
    }
    rejection_logs_[sender] = now;
    const auto room = world_.Rooms().FindRoom(sender);
    const auto scene =
        room ? world_.Rooms().Scene(room->RoomId()) : std::nullopt;
    logger_.Log(
        application::LogLevel::kWarning,
        "world rule rejected, keeping connection: opcode=" +
            std::to_string(packet.opcode) +
            " body_bytes=" + std::to_string(packet.body.size()) +
            " player_id=" + std::to_string(sender.Value()) + " room_id=" +
            std::to_string(room ? room->RoomId().Value() : 0U) + " mode_id=" +
            std::to_string(scene ? scene->configuration.mode_id : 0U) +
            " detail=" + std::string{detail});
    completion(std::vector<OutboundMessage>{});
  }

  void ExecutePacket(WorldActorContext actor, const protocol::Packet& packet,
                     WorldCommandCompletion completion) noexcept {
    try {
      const auto sender = actor.player_id;
      const auto origin =
          actor.orig_session
              ? std::optional{SessionHandle{actor.orig_session->first,
                                            actor.orig_session->second}}
              : std::nullopt;
      const bool tutorial_operation = packet.opcode == 107U ||
                                      world_.HasTutorial(sender) ||
                                      pending_tutorials_.contains(sender);
      if (tutorial_operation &&
          (!origin || !sessions_.IsCurrentPlayerSession(sender, *origin))) {
        completion(std::vector<OutboundMessage>{});  // An old socket cannot act
                                                     // for its replacement.
        return;
      }
      if (const auto owner = tutorial_owners_.find(sender);
          tutorial_operation && owner != tutorial_owners_.end() &&
          owner->second.first != *origin &&
          !pending_tutorials_.contains(sender)) {
        auto interrupted =
            world_.InterruptTutorial(sender, owner->second.second);
        if (interrupted.tutorial_command)
          BeginTutorialWork(std::move(interrupted), std::nullopt, {});
        else
          tutorial_owners_.erase(owner);
      }
      if (pending_tutorials_.contains(sender)) {
        auto& queued = deferred_tutorial_commands_[sender];
        if (queued.size() >= 16U) {
          completion(MakeError(application::ErrorCode::kOverloaded,
                               "单人教程数据库等待队列已满", true));
          return;
        }
        queued.push_back([this, actor = std::move(actor), packet,
                          completion = std::move(completion)]() mutable {
          ExecutePacket(std::move(actor), packet, std::move(completion));
        });
        return;
      }
      if (packet.opcode == 107U) {
        static_cast<void>(protocol::DecodeTutorialStartReport(packet.body));
        if (!options_.tutorial_sessions) {
          completion(MakeError(application::ErrorCode::kInvalidState,
                               "原单人教程报告账本不可用，暂不开放入口"));
          return;
        }
        if (!world_.HasTutorial(sender) || world_.HasSingleWaiting(sender)) {
          auto identity = match_ids_.Next();
          if (const auto* error =
                  std::get_if<application::ServiceError>(&identity)) {
            completion(*error);
            return;
          }
          actor.tutorial_session_id =
              std::get<domain::MatchId>(std::move(identity));
        }
      }
      if (packet.opcode == 601U) {
        protocol::PacketReader reader{packet.body};
        const auto status = reader.ReadU32();
        reader.RequireEnd();
        if (status > 1U)
          throw protocol::ProtocolError{
              protocol::ProtocolErrorCode::kInvalidValue,
              "invalid native relay status"};
        // B5B3F0 / B5A250 report main-connection status without a generation
        // field. Only the ordered auxiliary C2S6 can authorize this
        // generation's start.
        completion(std::vector<OutboundMessage>{});
        return;
      }
      if (packet.opcode == 502U) {
        actor.online_player_count = sessions_.OnlinePlayerCount();
      }
      if (const auto room = world_.Rooms().FindRoom(sender);
          room && room->Phase() == RoomPhase::kPlaying &&
          pending_equipment_rooms_.contains(room->RoomId()) &&
          packet.opcode != 32840U && packet.opcode != 502U) {
        // 数据库提交期间按接收顺序保留动作和出生握手，不能像开局阶段那样丢弃。
        auto& deferred = deferred_equipment_commands_[room->RoomId()];
        if (deferred.size() >= options_.queue_capacity) {
          completion(MakeError(application::ErrorCode::kOverloaded,
                               "预设同步等待队列已满", true));
          return;
        }
        deferred.push_back([this, actor = std::move(actor), packet,
                            completion = std::move(completion)]() mutable {
          ExecutePacket(std::move(actor), packet, std::move(completion));
        });
        return;
      }
      if (const auto room = world_.Rooms().FindRoom(sender);
          room && RoomHasPendingExternalWork(room->RoomId()) &&
          packet.opcode != 32840U) {
        if (pending_match_starts_.contains(room->RoomId()) &&
            (room->ModeId() == 101U || room->ModeId() == 102U) &&
            (packet.opcode == 903U ||
             PirateMapAgents::Accepts(packet.opcode))) {
          auto handled = world_.Handle(std::move(actor), packet,
                                       std::chrono::steady_clock::now());
          if (const auto* error =
                  std::get_if<application::ServiceError>(&handled)) {
            completion(*error);
          } else {
            auto effects = std::get<WorldCoordinatorResult>(std::move(handled));
            // 只同步地图角色或保存首波，不提前发布持久化尚未完成的 Playing
            // 状态。
            effects.session_updates.clear();
            effects.active_timers.clear();
            completion(Dispatch(std::move(effects), sender));
          }
          return;
        }
        if (packet.opcode == 502U) {
          auto handled = world_.Handle(std::move(actor), packet,
                                       std::chrono::steady_clock::now());
          if (const auto* error =
                  std::get_if<application::ServiceError>(&handled)) {
            completion(*error);
          } else {
            auto chat = std::get<WorldCoordinatorResult>(std::move(handled));
            // 聊天不依赖结算落库，也不能借广播提前发布内部 Results
            // 阶段或计时器。
            chat.session_updates.clear();
            chat.active_timers.clear();
            completion(Dispatch(std::move(chat), sender));
          }
          return;
        }
        if (pending_equipment_rooms_.contains(room->RoomId()) &&
            (packet.opcode == 503U || packet.opcode == 504U)) {
          static_cast<void>(
              protocol::ValidateRoomControlMessage(packet.opcode, packet.body));
          completion(std::vector<OutboundMessage>{
              {.opcode = 109U,
               .body =
                   protocol::EncodeNoticeResponse(u"装备更新中，请稍后开始")}});
          return;
        }
        if (protocol::ValidateRoomControlMessage(packet.opcode, packet.body)) {
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (protocol::IsPveEventOpcode(packet.opcode)) {
          protocol::ValidatePveEvent(packet.opcode, packet.body);
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (PirateMatch::Accepts(packet.opcode)) {
          PirateMatch::Validate(packet);
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (PirateMapAgents::Accepts(packet.opcode)) {
          PirateMapAgents::Validate(packet);
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (PirateMapAgents::AcceptsLink(packet.opcode)) {
          PirateMapAgents::ValidateLink(packet);
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (MatchObjectives::Accepts(packet.opcode)) {
          MatchObjectives::Validate(packet);
          completion(std::vector<OutboundMessage>{});
          return;
        }
        if (packet.opcode == 701U || packet.opcode == 702U ||
            packet.opcode == 709U) {
          if (packet.opcode == 701U) {
            static_cast<void>(
                protocol::DecodeLoadingCompleteRequest(packet.body));
          } else if (packet.opcode == 702U) {
            static_cast<void>(protocol::DecodeMapAssetsRequest(packet.body));
          } else {
            static_cast<void>(
                protocol::DecodePlayerObjectIdentityRequest(packet.body));
          }
          completion(std::vector<OutboundMessage>{});
          return;
        }
        // C2S32840
        // 只在房间执行器核验本人席位并回显时钟，不修改状态或访问数据库；
        // 开局持久化期间也必须响应，否则正常 RTT 探测会触发过载断线。
        const bool native_combat =
            protocol::IsNativeStormOpcode(packet.opcode) ||
            protocol::IsNativeWeaponObjectOpcode(packet.opcode) ||
            protocol::IsNativeActionOpcode(packet.opcode) ||
            protocol::IsNativeHitOpcode(packet.opcode) ||
            protocol::IsNativeLifecycleOpcode(packet.opcode) ||
            protocol::IsNativeRescueOpcode(packet.opcode) ||
            protocol::IsNativeRespawnOpcode(packet.opcode);
        if (native_combat) {
          // 开局或终局数据库回执到达前，客户端仍可能发送上一阶段已经排队的动作。
          // 这些包必须先严格校验再无副作用丢弃；若返回可重试错误，TCP Session
          // 会把正常 的阶段竞态误判为致命协议失败并断开整场玩家。
          protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
          logger_.Log(
              application::LogLevel::kDebug,
              "native combat message ignored while room persistence is pending");
          completion(std::vector<OutboundMessage>{});
          return;
        }
        RejectAndKeepConnection(sender, packet, "room persistence is pending",
                                std::move(completion));
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (packet.opcode == 65073U && packet.body.size() == 28U &&
          options_.ice_leases) {
        const auto room = world_.Rooms().FindRoom(actor.player_id);
        if (room &&
            room->NetworkMode() == protocol::RoomNetworkMode::kP2pPreferred)
          actor.ice_lease = options_.ice_leases->Issue(actor.player_id);
      }
      auto result = world_.Handle(std::move(actor), packet, now);
      if (const auto* error = std::get_if<application::ServiceError>(&result)) {
        if (packet.opcode == 107U && !world_.HasTutorial(sender))
          tutorial_owners_.erase(sender);
        if (error->code == application::ErrorCode::kInvalidState ||
            error->code == application::ErrorCode::kConflict ||
            error->code == application::ErrorCode::kPermissionDenied ||
            error->code == application::ErrorCode::kInvalidInput) {
          RejectAndKeepConnection(sender, packet, error->safe_message,
                                  std::move(completion));
          return;
        }
        completion(*error);
        return;
      }
      if (packet.opcode == 107U) {
        const auto& accepted = std::get<WorldCoordinatorResult>(result);
        if (accepted.tutorial_command) {
          if (const auto* start =
                  std::get_if<application::TutorialSessionStart>(
                      &*accepted.tutorial_command))
            tutorial_owners_.insert_or_assign(
                sender, std::pair{*origin, start->session_id});
        }
      }
      RouteResult(std::get<WorldCoordinatorResult>(std::move(result)), sender,
                  std::move(completion), now);
    } catch (const protocol::ProtocolError& error) {
      // A malformed chat string must not destroy the authenticated game
      // session. Keep strict UTF-16 validation and discard the entire chat
      // message.
      if (packet.opcode == 502U &&
          error.Code() == protocol::ProtocolErrorCode::kInvalidUtf16) {
        RejectAndKeepConnection(actor.player_id, packet, error.what(),
                                std::move(completion));
        return;
      }
      // 仅将结构校验通过后的角色/装备规则拒绝降级；畸形长度、数值等仍由解析器拒绝。
      const bool combat_packet =
          protocol::IsNativeStormOpcode(packet.opcode) ||
          protocol::IsNativeWeaponObjectOpcode(packet.opcode) ||
          protocol::IsNativeActionOpcode(packet.opcode) ||
          protocol::IsNativeHitOpcode(packet.opcode) ||
          protocol::IsNativeLifecycleOpcode(packet.opcode) ||
          protocol::IsNativeRescueOpcode(packet.opcode) ||
          protocol::IsNativeRespawnOpcode(packet.opcode);
      bool structurally_valid = false;
      if (combat_packet &&
          error.Code() == protocol::ProtocolErrorCode::kInvalidValue) {
        try {
          protocol::ValidateNativeCombatMessage(packet.opcode, packet.body);
          structurally_valid = true;
        } catch (const protocol::ProtocolError&) {
          // 保留下面的协议错误诊断和处理。
        }
      }
      if (structurally_valid) {
        RejectAndKeepConnection(actor.player_id, packet, error.what(),
                                std::move(completion));
        return;
      }
      // Outside native combat (whose decoders also report structural damage as
      // an invalid value, handled above) the frame decoded completely but a
      // field holds a value the server refuses: an out-of-range count, flag or
      // identity, or unpaired UTF-16. Nothing is out of step, so discard only
      // this message instead of disconnecting the player. Truncated, trailing
      // or oversized payloads still close below.
      if (!combat_packet &&
          (error.Code() == protocol::ProtocolErrorCode::kInvalidValue ||
           error.Code() == protocol::ProtocolErrorCode::kInvalidUtf16)) {
        RejectAndKeepConnection(
            actor.player_id, packet,
            "protocol " +
                std::string{protocol::ProtocolErrorCodeName(error.Code())} +
                ": " + error.what(),
            std::move(completion));
        return;
      }
      const auto room = world_.Rooms().FindRoom(actor.player_id);
      const auto scene =
          room ? world_.Rooms().Scene(room->RoomId()) : std::nullopt;
      logger_.Log(
          application::LogLevel::kWarning,
          "world protocol rejected: opcode=" + std::to_string(packet.opcode) +
              " body_bytes=" + std::to_string(packet.body.size()) +
              " player_id=" + std::to_string(actor.player_id.Value()) +
              " room_id=" + std::to_string(room ? room->RoomId().Value() : 0U) +
              " mode_id=" +
              std::to_string(scene ? scene->configuration.mode_id : 0U) +
              " reason=" +
              std::string{protocol::ProtocolErrorCodeName(error.Code())} +
              " detail=" + error.what());
      completion(MakeError(application::ErrorCode::kInvalidInput,
                           "世界协议消息字段无效"));
    } catch (const std::exception&) {
      completion(MakeError(application::ErrorCode::kInternal,
                           "世界命令执行时发生未分类错误"));
    } catch (...) {
      completion(MakeError(application::ErrorCode::kInternal,
                           "世界命令执行时发生未知错误"));
    }
  }

  void ExecuteDisconnect(
      domain::PlayerId player_id, WorldSessionPhase phase,
      std::optional<SessionHandle> origin = std::nullopt) noexcept {
    try {
      const auto owner = tutorial_owners_.find(player_id);
      if (origin && owner != tutorial_owners_.end() &&
          owner->second.first != *origin) {
        const auto replacement = sessions_.CurrentPlayerSession(player_id);
        if (replacement && *replacement != *origin)
          return;  // An earlier disconnect must not erase a live replacement.
        if (pending_tutorials_.contains(player_id)) {
          deferred_tutorial_disconnects_.insert_or_assign(player_id, phase);
          return;  // The replacement itself closed while old SQL was still
                   // pending.
        }
        auto interrupted =
            world_.InterruptTutorial(player_id, owner->second.second);
        if (interrupted.tutorial_command) {
          deferred_tutorial_disconnects_.insert_or_assign(player_id, phase);
          BeginTutorialWork(std::move(interrupted), std::nullopt, {});
          return;
        }
        tutorial_owners_.erase(owner);
      }
      if (pending_tutorials_.contains(player_id)) {
        deferred_tutorial_disconnects_.insert_or_assign(player_id, phase);
        return;
      }
      const auto remaining_owner = tutorial_owners_.find(player_id);
      if (remaining_owner != tutorial_owners_.end()) {
        const auto current = sessions_.CurrentPlayerSession(player_id);
        if (current && *current != remaining_owner->second.first) {
          auto result = world_.InterruptTutorial(
              player_id, remaining_owner->second.second);
          if (result.tutorial_command)
            BeginTutorialWork(std::move(result), std::nullopt, {});
          return;  // Do not erase the replacement's online/plaza/party state.
        }
      } else if (origin &&
                 (phase == WorldSessionPhase::kTutorialLoading ||
                  phase == WorldSessionPhase::kTutorialPlaying ||
                  phase == WorldSessionPhase::kSinglePlayWaiting) &&
                 sessions_.CurrentPlayerSession(player_id) &&
                 !sessions_.IsCurrentPlayerSession(player_id, *origin)) {
        return;
      }
      if (options_.equipment_relay)
        options_.equipment_relay->Revoke(player_id, "gateway_disconnected");
      rejection_logs_.erase(player_id);
      if (const auto room = world_.Rooms().FindRoom(player_id);
          room && RoomHasPendingExternalWork(room->RoomId())) {
        deferred_disconnects_[room->RoomId()].push_back(
            {.player_id = player_id, .phase = phase});
        return;
      }
      auto result =
          world_.Disconnect(player_id, phase, std::chrono::steady_clock::now());
      if (const auto* error = std::get_if<application::ServiceError>(&result)) {
        logger_.Log(application::LogLevel::kError,
                    "world disconnect cleanup failed: " + error->safe_message);
        return;
      }
      RouteResult(std::get<WorldCoordinatorResult>(std::move(result)),
                  std::nullopt, {}, std::chrono::steady_clock::now());
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "world disconnect cleanup escaped its exception boundary");
    }
  }

  void RouteResult(WorldCoordinatorResult result,
                   std::optional<domain::PlayerId> sender,
                   WorldCommandCompletion completion,
                   std::chrono::steady_clock::time_point observed_at) {
    if (result.tutorial_command) {
      BeginTutorialWork(std::move(result), sender, std::move(completion));
      return;
    }
    if (result.local_release_owner) {
      const auto [player, identity] = *result.local_release_owner;
      const auto owner = tutorial_owners_.find(player);
      if (owner == tutorial_owners_.end() || owner->second.second != identity) {
        if (completion)
          completion(MakeError(application::ErrorCode::kInvalidState,
                               "单人退出UUID不属于原连接"));
        return;
      }
      const auto handle = owner->second.first;
      auto posted = PublishTutorialResult(player, handle, std::move(result));
      if (const auto current = tutorial_owners_.find(player);
          current != tutorial_owners_.end() &&
          current->second.first == handle && current->second.second == identity)
        tutorial_owners_.erase(current);
      if (completion) {
        if (const auto* error = std::get_if<application::ServiceError>(&posted))
          completion(*error);
        else
          completion(std::vector<OutboundMessage>{});
      }
      return;
    }
    const auto tutorial_target =
        sender ? sender
               : (result.deliveries.empty()
                      ? std::optional<domain::PlayerId>{}
                      : std::optional{result.deliveries.front().recipient});
    if (tutorial_target && world_.HasTutorial(*tutorial_target) &&
        tutorial_owners_.contains(*tutorial_target)) {
      const auto handle = tutorial_owners_.at(*tutorial_target).first;
      auto posted =
          PublishTutorialResult(*tutorial_target, handle, std::move(result));
      if (const auto* error = std::get_if<application::ServiceError>(&posted)) {
        if (completion) completion(*error);
      } else {
        if (completion) completion(std::vector<OutboundMessage>{});
        if (!std::get<bool>(posted))
          ExecuteDisconnect(*tutorial_target,
                            WorldSessionPhase::kTutorialPlaying, handle);
      }
      return;
    }
    if (result.event == RoomEvent::kMatchStarted) {
      BeginMatchStart(std::move(result), sender, std::move(completion),
                      observed_at);
      return;
    }
    if (result.settlement_command.has_value()) {
      BeginSettlement(std::move(result), sender, std::move(completion));
      return;
    }
    auto dispatched = Dispatch(std::move(result), sender);
    if (completion) {
      completion(std::move(dispatched));
    } else if (const auto* error =
                   std::get_if<application::ServiceError>(&dispatched)) {
      logger_.Log(application::LogLevel::kError,
                  "world asynchronous dispatch failed: " + error->safe_message);
    }
  }

  application::ServiceResult<std::vector<OutboundMessage>> Dispatch(
      WorldCoordinatorResult result, std::optional<domain::PlayerId> sender) {
    Schedule(result.active_timers);
    if (result.settlement_command.has_value()) {
      return MakeError(application::ErrorCode::kInternal,
                       "未耐久保存的终局命令越过了 world dispatch 边界");
    }
    if (result.combat_reject_reason.has_value()) {
      logger_.Log(application::LogLevel::kDebug,
                  "combat message rejected by runtime policy");
    }
    if (result.rule_rejection_detail && sender) {
      const auto now = std::chrono::steady_clock::now();
      const auto previous = rejection_logs_.find(*sender);
      if (previous == rejection_logs_.end() ||
          now - previous->second >= std::chrono::seconds{5}) {
        rejection_logs_[*sender] = now;
        logger_.Log(
            application::LogLevel::kWarning,
            "world rule rejected, keeping connection: player_id=" +
                std::to_string(sender->Value()) + " room_id=" +
                std::to_string(result.room_id ? result.room_id->Value() : 0U) +
                " " + *result.rule_rejection_detail);
      }
    }
    if (options_.equipment_relay) {
      for (const auto& update : result.session_updates)
        if (update.phase == WorldSessionPhase::kMenu ||
            update.phase == WorldSessionPhase::kPlaza ||
            update.phase == WorldSessionPhase::kRoomWaiting ||
            update.phase == WorldSessionPhase::kRoomResults)
          options_.equipment_relay->Revoke(update.player_id);
    }
    auto dispatched = sessions_.Dispatch(std::move(result));
    if (const auto* error =
            std::get_if<application::ServiceError>(&dispatched)) {
      return *error;
    }
    const auto& report = std::get<SessionDispatchReport>(dispatched);
    if (sender.has_value() && SenderFailed(report, *sender)) {
      return MakeError(application::ErrorCode::kOverloaded,
                       "世界响应无法投递到当前 Session", true);
    }
    return std::vector<OutboundMessage>{};
  }

  application::ServiceResult<bool> PublishTutorialResult(
      domain::PlayerId player, SessionHandle origin,
      WorldCoordinatorResult result) {
    SessionDispatch batch;
    for (auto& delivery : result.deliveries) {
      if (delivery.recipient != player)
        return MakeError(application::ErrorCode::kInternal,
                         "教程候选消息包含异玩家目标");
      batch.messages.push_back(std::move(delivery.message));
    }
    for (const auto& update : result.session_updates) {
      if (update.player_id != player)
        return MakeError(application::ErrorCode::kInternal,
                         "教程候选阶段包含异玩家目标");
      batch.world_phase = update.phase;
      batch.channel = update.channel;
    }
    return sessions_.DispatchIfCurrent(player, origin, std::move(batch));
  }

  void BeginTutorialWork(WorldCoordinatorResult result,
                         std::optional<domain::PlayerId> sender,
                         WorldCommandCompletion completion) {
    const auto player =
        std::visit([](const auto& value) { return value.player_id; },
                   *result.tutorial_command);
    if (pending_tutorials_.contains(player)) {
      if (completion)
        completion(MakeError(application::ErrorCode::kInvalidState,
                             "教程报告正在提交"));
      return;
    }
    const auto owner = tutorial_owners_.find(player);
    const auto origin = owner == tutorial_owners_.end()
                            ? std::optional<SessionHandle>{}
                            : std::optional{owner->second.first};
    pending_tutorials_.emplace(
        player, PendingTutorialWork{std::move(result), sender,
                                    std::move(completion), false, origin});
    SubmitTutorialWork(player);
  }

  void SubmitTutorialWork(domain::PlayerId player) {
    const auto found = pending_tutorials_.find(player);
    if (found == pending_tutorials_.end() || found->second.in_flight) return;
    found->second.in_flight = true;
    tutorial_retries_.erase(player);
    const auto command = *found->second.result.tutorial_command;
    const auto rejected = background_tasks_.Post([this, player, command] {
      application::ServiceResult<bool> committed =
          MakeError(application::ErrorCode::kStorageUnavailable,
                    "教程报告账本不可用", true);
      try {
        if (options_.tutorial_sessions) {
          if (const auto* start =
                  std::get_if<application::TutorialSessionStart>(&command))
            committed = options_.tutorial_sessions->Begin(*start);
          else
            committed = options_.tutorial_sessions->Finish(
                std::get<application::TutorialSessionFinish>(command));
        }
      } catch (...) {
        committed = MakeError(application::ErrorCode::kStorageUnavailable,
                              "教程报告事务发生存储错误", true);
      }
      static_cast<void>(EnqueueInternal(
          [this, player, committed = std::move(committed)]() mutable {
            FinishTutorialWork(player, std::move(committed));
          }));
    });
    if (rejected) FinishTutorialWork(player, *rejected);
  }

  void FinishTutorialWork(domain::PlayerId player,
                          application::ServiceResult<bool> committed) {
    const auto found = pending_tutorials_.find(player);
    if (found == pending_tutorials_.end()) return;
    found->second.in_flight = false;
    const auto& command = *found->second.result.tutorial_command;
    const bool begin =
        std::holds_alternative<application::TutorialSessionStart>(command);
    const auto identity =
        std::visit([](const auto& value) { return value.session_id; }, command);
    if (const auto* error =
            std::get_if<application::ServiceError>(&committed)) {
      if (!begin || error->correlation_id == "tutorial_begin_uncertain") {
        tutorial_retries_[player] =
            std::chrono::steady_clock::now() + options_.settlement_retry_delay;
        logger_.Log(
            application::LogLevel::kWarning,
            "client-reported tutorial SQL outcome unconfirmed; same UUID retry, no success "
            "published: " +
                error->safe_message);
        return;
      }
      auto failed = std::move(found->second);
      pending_tutorials_.erase(found);
      const bool current =
          failed.origin &&
          sessions_.IsCurrentPlayerSession(player, *failed.origin) &&
          !deferred_tutorial_disconnects_.contains(player);
      world_.TutorialBeginFailed(player, identity, current);
      if (world_.HasSingleWaiting(player) && failed.origin &&
          world_.LocalSessionId(player))
        tutorial_owners_.insert_or_assign(
            player, std::pair{*failed.origin, *world_.LocalSessionId(player)});
      else
        tutorial_owners_.erase(player);
      if (failed.completion) failed.completion(*error);
      DrainTutorialCommands(player, failed.origin);
      return;
    }
    auto saved = std::move(found->second);
    pending_tutorials_.erase(found);
    tutorial_retries_.erase(player);
    const bool current =
        saved.origin &&
        sessions_.IsCurrentPlayerSession(player, *saved.origin) &&
        !deferred_tutorial_disconnects_.contains(player);
    world_.TutorialAcknowledge(player, identity, begin, current);
    saved.result.tutorial_command.reset();
    bool published = false;
    application::ServiceResult<std::vector<OutboundMessage>> sent =
        std::vector<OutboundMessage>{};
    if (current) {
      auto posted =
          PublishTutorialResult(player, *saved.origin, std::move(saved.result));
      if (const auto* error = std::get_if<application::ServiceError>(&posted))
        sent = *error;
      else
        published = std::get<bool>(posted);
    }
    if (saved.completion) saved.completion(std::move(sent));
    if (!begin && !published && world_.HasSingleWaiting(player))
      static_cast<void>(world_.InterruptTutorial(
          player, identity));  // Old unshown Room4 cannot own a replacement.
    if (begin && !published) {
      // The old socket has no scene, but its committed start must still
      // terminate.
      auto interrupted = world_.InterruptTutorial(player, identity);
      if (interrupted.tutorial_command)
        BeginTutorialWork(std::move(interrupted), std::nullopt, {});
    } else if (!begin && !world_.HasSingleWaiting(player)) {
      if (const auto owner = tutorial_owners_.find(player);
          owner != tutorial_owners_.end() && owner->second.second == identity)
        tutorial_owners_.erase(owner);
    }
    DrainTutorialCommands(player, saved.origin);
  }

  void DrainTutorialCommands(domain::PlayerId player,
                             std::optional<SessionHandle> origin) {
    if (!pending_tutorials_.contains(player)) {
      if (const auto disconnected = deferred_tutorial_disconnects_.find(player);
          disconnected != deferred_tutorial_disconnects_.end()) {
        const auto phase = disconnected->second;
        deferred_tutorial_disconnects_.erase(disconnected);
        const auto replacement = sessions_.CurrentPlayerSession(player);
        if (!replacement || (origin && *replacement == *origin))
          ExecuteDisconnect(player, phase, origin);
        // An old disconnect must not erase H2's online/menu/party identity.
      }
    }
    if (const auto found = deferred_tutorial_commands_.find(player);
        found != deferred_tutorial_commands_.end()) {
      auto commands = std::move(found->second);
      deferred_tutorial_commands_.erase(found);
      for (auto& command : commands) command();
    }
  }

  void BeginMatchStart(WorldCoordinatorResult result,
                       std::optional<domain::PlayerId> sender,
                       WorldCommandCompletion completion,
                       std::chrono::steady_clock::time_point started_at) {
    if (!result.room_id.has_value() ||
        pending_match_starts_.contains(*result.room_id)) {
      FailMatchStart(result.room_id, sender, std::move(completion),
                     MakeError(application::ErrorCode::kInternal,
                               "正式开局缺少唯一房间上下文"));
      return;
    }
    const auto room_id = *result.room_id;
    const auto room = world_.Rooms().FindRoom(room_id);
    if (!room) {
      FailMatchStart(room_id, sender, std::move(completion),
                     MakeError(application::ErrorCode::kInvalidState,
                               "正式开局所属房间已不存在"));
      return;
    }

    const auto scene = world_.Rooms().Scene(room_id);
    const bool map_browse = scene && scene->configuration.submode_id == 4U;
    // Map browse and the private training room are practice sessions: no match
    // ID, objectives, formal start, settlement or awards.
    const bool practice = map_browse || room->IsTraining();
    // TestMap still needs authenticated equipment and object identities. Its
    // player-only relay uses ordinary preparation; no mode objectives, formal
    // start or awards are created.
    application::MatchPreparationRequest request{
        .room_id = room_id,
        .mode_id = practice ? 1U : room->ModeId(),
        .participants = {}};
    for (const auto& member : room->Members()) {
      if (!member.object_id.has_value() || member.seat > 255U) {
        FailMatchStart(room_id, sender, std::move(completion),
                       MakeError(application::ErrorCode::kInvalidState,
                                 "正式开局前仍有成员未登记战斗对象"));
        return;
      }
      if (member.IsObserver()) continue;
      request.participants.push_back(
          {.player_id = member.player_id,
           .seat_index = static_cast<std::uint8_t>(member.seat),
           .team_code = member.team,
           .actor_object_id = *member.object_id});
    }
    request.restricted_items = room->RestrictedItems();
    std::optional<domain::MatchId> match_id;
    if (!practice) {
      auto next_id = match_ids_.Next();
      if (const auto* error =
              std::get_if<application::ServiceError>(&next_id)) {
        FailMatchStart(room_id, sender, std::move(completion), *error);
        return;
      }
      match_id = std::get<domain::MatchId>(std::move(next_id));
    }
    const auto user_host_prize = world_.Rooms().UserHostPrize(room_id);
    if (room->UserHosted() && (!user_host_prize || !options_.prize_menus)) {
      FailMatchStart(room_id, sender, std::move(completion),
                     MakeError(application::ErrorCode::kInvalidState,
                               "主办开局缺少冻结奖金或资产读取端口"));
      return;
    }
    const auto utc_started_at = clock_.Now();
    const auto source_channel = world_.RoomChannel(room_id);
    pending_match_starts_.insert(room_id);

    // BackgroundTaskExecutor 在拒绝任务时不返还
    // closure，因此为拒绝分支保留原完成回调；
    // 已受理任务使用独立拥有副本，避免队列边界造成 Session 永久悬挂。
    auto task_result = result;
    auto task_completion = completion;

    const auto post_error = background_tasks_.Post(
        [this, room_id, request = std::move(request), match_id, practice,
         utc_started_at, started_at, source_channel, user_host_prize,
         result = std::move(task_result), sender,
         completion = std::move(task_completion)]() mutable {
          MatchStartWorkResult work_result = MakeError(
              application::ErrorCode::kInternal, "开局准备任务没有产生结果");
          try {
            auto prepared_result =
                match_preparation_.Prepare(std::move(request));
            if (const auto* error =
                    std::get_if<application::ServiceError>(&prepared_result)) {
              work_result = *error;
            } else {
              auto prepared = std::get<application::PreparedMatch>(
                  std::move(prepared_result));
              if (practice) {
                work_result = MatchStartSuccess{.prepared = std::move(prepared),
                                                .persisted = std::nullopt};
              } else {
                application::MatchStartCommand command{
                    .match_id = *match_id,
                    .source_room_id = room_id,
                    .mode_id = prepared.mode_id,
                    .configuration_version_id =
                        prepared.configuration_version_id,
                    .reward_rules_version_id = prepared.reward_rules_version_id,
                    .score_model = prepared.score_model,
                    .started_at = utc_started_at,
                    .source_channel = source_channel,
                    .participants = {},
                    .user_host_prize = user_host_prize};
                command.participants.reserve(prepared.participants.size());
                for (const auto& participant : prepared.participants) {
                  command.participants.push_back(
                      {.player_id = participant.identity.player_id,
                       .seat_index = participant.identity.seat_index,
                       .team_code = participant.identity.team_code});
                }
                auto persisted = match_starts_.Start(std::move(command));
                if (const auto* persistence_error =
                        std::get_if<application::ServiceError>(&persisted)) {
                  if (user_host_prize &&
                      (persistence_error->is_retryable ||
                       persistence_error->code ==
                           application::ErrorCode::kStorageUnavailable))
                    work_result =
                        UncertainPrizeStart{*match_id, *persistence_error};
                  else
                    work_result = *persistence_error;
                } else {
                  work_result = MatchStartSuccess{
                      .prepared = std::move(prepared),
                      .persisted = std::get<application::MatchStartResult>(
                          std::move(persisted))};
                  if (user_host_prize) {
                    // Preserve the committed match ID even if the following
                    // read fails: it must be compensated, never converted to an
                    // unowned start failure.
                    auto& success = std::get<MatchStartSuccess>(work_result);
                    try {
                      auto snapshot = options_.prize_menus->Load(
                          user_host_prize->sponsor_player_id);
                      if (const auto* snapshot_error =
                              std::get_if<application::ServiceError>(&snapshot))
                        success.asset_error = *snapshot_error;
                      else
                        success.sponsor_snapshot =
                            std::get<application::PlayerMenuSnapshot>(
                                std::move(snapshot));
                    } catch (...) {
                      success.asset_error =
                          MakeError(application::ErrorCode::kStorageUnavailable,
                                    "主办资金提交后资产同步失败", true);
                    }
                  }
                }
              }
            }
          } catch (const std::exception&) {
            auto failure = MakeError(application::ErrorCode::kInternal,
                                     "开局数据库任务发生未分类错误");
            if (user_host_prize && match_id)
              work_result = UncertainPrizeStart{*match_id, std::move(failure)};
            else
              work_result = std::move(failure);
          } catch (...) {
            auto failure = MakeError(application::ErrorCode::kInternal,
                                     "开局数据库任务发生未知错误");
            if (user_host_prize && match_id)
              work_result = UncertainPrizeStart{*match_id, std::move(failure)};
            else
              work_result = std::move(failure);
          }
          static_cast<void>(EnqueueInternal(
              [this, room_id, started_at, result = std::move(result), sender,
               completion = std::move(completion),
               work_result = std::move(work_result)]() mutable {
                FinishMatchStart(room_id, started_at, std::move(result), sender,
                                 std::move(completion), std::move(work_result));
              }));
        });
    if (post_error.has_value()) {
      pending_match_starts_.erase(room_id);
      FailMatchStart(room_id, sender, std::move(completion), *post_error);
    }
  }

  void FinishMatchStart(domain::RoomId room_id,
                        std::chrono::steady_clock::time_point started_at,
                        WorldCoordinatorResult result,
                        std::optional<domain::PlayerId> sender,
                        WorldCommandCompletion completion,
                        MatchStartWorkResult work_result,
                        bool equipment_ready = false) {
    if (const auto* uncertain =
            std::get_if<UncertainPrizeStart>(&work_result)) {
      BeginMatchCompensation(room_id, uncertain->match_id, sender,
                             std::move(completion), uncertain->error);
      return;
    }
    if (const auto* error =
            std::get_if<application::ServiceError>(&work_result)) {
      pending_match_starts_.erase(room_id);
      FailMatchStart(room_id, sender, std::move(completion), *error);
      ProcessDeferredDisconnects(room_id);
      return;
    }
    auto success = std::get<MatchStartSuccess>(std::move(work_result));
    if (success.asset_error) {
      BeginMatchCompensation(room_id, success.persisted->match_id, sender,
                             std::move(completion), *success.asset_error);
      return;
    }
    if (options_.equipment_relay && !equipment_ready) {
      BeginEquipmentStart(room_id, started_at, std::move(result), sender,
                          std::move(completion), std::move(success));
      return;
    }
    std::vector<CombatParticipant> participants;
    participants.reserve(success.prepared.participants.size());
    for (auto& participant : success.prepared.participants) {
      participants.push_back(
          {.player_id = participant.identity.player_id,
           .actor_object_id = participant.identity.actor_object_id,
           .team_code = participant.identity.team_code,
           .weapon_templates = participant.weapon_templates,
           .ordinary_respawn_allowed =
               success.prepared.ordinary_respawn_allowed,
           .seat_index = participant.identity.seat_index,
           .lucci_pouch = participant.lucci_pouch,
           .chocolate_box = participant.chocolate_box,
           .fighter = participant.fighter,
           .equipment_items = participant.equipment_items});
    }
    const RoomMatchInstallation installation{
        .match_id = success.persisted
                        ? std::optional{success.persisted->match_id}
                        : std::nullopt,
        .score_model = success.prepared.score_model,
        .started_at = started_at,
        .participants = std::move(participants)};
    auto installed = world_.InstallCombat(room_id, installation);
    if (const auto* error =
            std::get_if<application::ServiceError>(&installed)) {
      if (!success.persisted) {
        pending_match_starts_.erase(room_id);
        FailMatchStart(room_id, sender, std::move(completion), *error);
        ProcessDeferredDisconnects(room_id);
        return;
      }
      logger_.Log(
          application::LogLevel::kCritical,
          "persisted match could not be installed into its unchanged room");
      BeginMatchCompensation(room_id, success.persisted->match_id, sender,
                             std::move(completion), *error);
      return;
    }
    // Combat installation arms the room-level movement tick. Refresh the
    // timer snapshot after the async start gate so the first tick is actually
    // scheduled alongside the match timeout.
    if (const auto runtime = world_.Rooms().FindRoom(room_id))
      result.active_timers = runtime->ActiveTimers();
    if (success.sponsor_snapshot) {
      auto assets =
          world_.AssetsCommitted(std::move(*success.sponsor_snapshot));
      if (const auto* error = std::get_if<application::ServiceError>(&assets)) {
        logger_.Log(application::LogLevel::kError,
                    "主办资产回包编码失败: " + error->safe_message);
      } else {
        RouteResult(std::get<WorldCoordinatorResult>(std::move(assets)),
                    std::nullopt, {}, std::chrono::steady_clock::now());
      }
    }
    pending_match_starts_.erase(room_id);
    auto dispatched = Dispatch(std::move(result), sender);
    auto effects =
        world_.InitialMatchEffects(room_id, std::chrono::steady_clock::now());
    if (const auto* error = std::get_if<application::ServiceError>(&effects)) {
      logger_.Log(application::LogLevel::kError,
                  "initial objective effects failed: " + error->safe_message);
    } else {
      RouteResult(std::get<WorldCoordinatorResult>(std::move(effects)),
                  std::nullopt, {}, std::chrono::steady_clock::now());
    }
    if (completion) {
      completion(std::move(dispatched));
    }
    ProcessDeferredDisconnects(room_id);
  }

  void BeginEquipmentStart(domain::RoomId room_id,
                           std::chrono::steady_clock::time_point started_at,
                           WorldCoordinatorResult result,
                           std::optional<domain::PlayerId> sender,
                           WorldCommandCompletion completion,
                           MatchStartSuccess success) {
    std::vector<EquipmentRelayViewer> viewers;
    const auto room = world_.Rooms().FindRoom(room_id);
    if (room) {
      for (const auto& member : room->Members()) {
        const auto profile = world_.Rooms().PlayerSnapshot(member.player_id);
        if (!profile) {
          viewers.clear();
          break;
        }
        viewers.push_back({member.player_id,
                           static_cast<std::uint8_t>(member.seat),
                           profile->nickname});
      }
    }
    auto issued = options_.equipment_relay->Begin(
        success.prepared, world_.RoomChannel(room_id), viewers);
    pending_equipment_starts_.emplace(
        room_id,
        PendingEquipmentStart{
            started_at, std::move(result), sender, std::move(completion),
            std::move(success),
            std::chrono::steady_clock::now() + std::chrono::seconds{30}});
    if (const auto* error = std::get_if<application::ServiceError>(&issued)) {
      FailEquipmentStart(room_id, *error);
      return;
    }
    auto& pending = pending_equipment_starts_.at(room_id);
    WorldCoordinatorResult bootstrap;
    // A zero countdown combines activation and 704 in one event. All 701 must
    // precede 601, because native 7BB450 silently discards sequences for absent
    // actor objects.
    for (auto& delivery : pending.result.deliveries)
      if (delivery.message.opcode != 704U)
        bootstrap.deliveries.push_back(delivery);
    std::erase_if(pending.result.deliveries, [](const auto& delivery) {
      return delivery.message.opcode != 704U;
    });
    for (const auto& invitation :
         std::get<std::vector<EquipmentRelayInvitation>>(issued))
      bootstrap.deliveries.push_back(
          {invitation.player,
           {601U, protocol::EncodeEquipmentRelayEndpoint(
                      options_.equipment_relay_address,
                      options_.equipment_relay_port, invitation.key)}});
    const auto dispatched = sessions_.Dispatch(std::move(bootstrap));
    if (const auto* error =
            std::get_if<application::ServiceError>(&dispatched)) {
      FailEquipmentStart(room_id, *error);
      return;
    }
    const auto& report = std::get<SessionDispatchReport>(dispatched);
    if (!report.offline_players.empty() || !report.rejected_batches.empty()) {
      FailEquipmentStart(room_id, MakeError(application::ErrorCode::kOverloaded,
                                            "装备初始化无法送达全员"));
      return;
    }
    next_equipment_check_ = std::chrono::steady_clock::now();
    wake_.notify_one();
  }

  void FailEquipmentStart(domain::RoomId room_id,
                          application::ServiceError error) {
    const auto found = pending_equipment_starts_.find(room_id);
    if (found == pending_equipment_starts_.end()) return;
    auto pending = std::move(found->second);
    pending_equipment_starts_.erase(found);
    options_.equipment_relay->RevokeRoom(room_id, "bootstrap_failed");
    if (pending.success.persisted) {
      BeginMatchCompensation(room_id, pending.success.persisted->match_id,
                             pending.sender, std::move(pending.completion),
                             std::move(error));
    } else {
      pending_match_starts_.erase(room_id);
      FailMatchStart(room_id, pending.sender, std::move(pending.completion),
                     error);
      ProcessDeferredDisconnects(room_id);
    }
  }

  void CheckEquipmentStarts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<domain::RoomId> pending_rooms;
    for (const auto& [room, pending] : pending_equipment_starts_) {
      static_cast<void>(pending);
      pending_rooms.push_back(room);
    }
    for (const auto room : pending_rooms) {
      const auto found = pending_equipment_starts_.find(room);
      if (found == pending_equipment_starts_.end()) continue;
      const auto ready = options_.equipment_relay->Readiness(room);
      if (ready == EquipmentRelayReadiness::kFailed ||
          now >= found->second.deadline) {
        FailEquipmentStart(
            room,
            MakeError(application::ErrorCode::kInvalidState,
                      std::string{"原生装备初始化失败或超时 gate_reason="} +
                          (ready == EquipmentRelayReadiness::kFailed
                               ? "participant_failed"
                               : "deadline_elapsed") +
                          options_.equipment_relay->Diagnostic(room)));
      } else if (ready == EquipmentRelayReadiness::kReady) {
        auto pending = std::move(found->second);
        pending_equipment_starts_.erase(found);
        if (const auto runtime = world_.Rooms().FindRoom(room)) {
          runtime->BeginAuthorizedMatchClock(now);
          pending.result.active_timers = runtime->ActiveTimers();
        }
        FinishMatchStart(room, now, std::move(pending.result), pending.sender,
                         std::move(pending.completion),
                         std::move(pending.success), true);
      }
    }
    next_equipment_check_ = now + std::chrono::milliseconds{250};
  }

  void BeginMatchCompensation(domain::RoomId room_id, domain::MatchId match_id,
                              std::optional<domain::PlayerId> sender,
                              WorldCommandCompletion completion,
                              application::ServiceError install_error) {
    const auto inserted = pending_match_compensations_.emplace(
        room_id,
        PendingMatchCompensation{.match_id = match_id,
                                 .interrupted_at = clock_.Now(),
                                 .sender = sender,
                                 .completion = std::move(completion),
                                 .install_error = std::move(install_error),
                                 .failed_attempts = 0U});
    if (!inserted.second) {
      logger_.Log(application::LogLevel::kCritical,
                  "persisted match compensation ownership was duplicated");
      return;
    }
    SubmitMatchCompensation(room_id);
  }

  void SubmitMatchCompensation(domain::RoomId room_id) {
    const auto pending = pending_match_compensations_.find(room_id);
    if (pending == pending_match_compensations_.end()) {
      return;
    }
    const auto match_id = pending->second.match_id;
    const auto interrupted_at = pending->second.interrupted_at;
    const auto prize = world_.Rooms().UserHostPrize(room_id);
    const auto post_error = background_tasks_.Post([this, room_id, match_id,
                                                    interrupted_at, prize] {
      std::optional<application::ServiceError> repaired;
      std::optional<application::PlayerMenuSnapshot> snapshot;
      try {
        repaired =
            match_repairs_.InterruptFailedStart(match_id, interrupted_at);
        if (!repaired && prize && options_.prize_menus) {
          auto loaded = options_.prize_menus->Load(prize->sponsor_player_id);
          if (const auto* error =
                  std::get_if<application::ServiceError>(&loaded))
            repaired = *error;
          else
            snapshot =
                std::get<application::PlayerMenuSnapshot>(std::move(loaded));
        }
      } catch (const std::exception&) {
        repaired = MakeError(application::ErrorCode::kInternal,
                             "开局补偿任务发生未分类错误", true);
      } catch (...) {
        repaired = MakeError(application::ErrorCode::kInternal,
                             "开局补偿任务发生未知错误", true);
      }
      static_cast<void>(
          EnqueueInternal([this, room_id, repaired = std::move(repaired),
                           snapshot = std::move(snapshot)]() mutable {
            if (!repaired && snapshot) {
              auto refreshed = world_.AssetsCommitted(std::move(*snapshot));
              if (const auto* error =
                      std::get_if<application::ServiceError>(&refreshed))
                repaired = *error;
              else
                RouteResult(
                    std::get<WorldCoordinatorResult>(std::move(refreshed)),
                    std::nullopt, {}, std::chrono::steady_clock::now());
            }
            FinishMatchCompensation(room_id, std::move(repaired));
          }));
    });
    if (post_error.has_value()) {
      ScheduleMatchCompensationRetry(room_id, *post_error);
    }
  }

  void FinishMatchCompensation(
      domain::RoomId room_id,
      std::optional<application::ServiceError> repaired) {
    const auto pending = pending_match_compensations_.find(room_id);
    if (pending == pending_match_compensations_.end()) {
      logger_.Log(application::LogLevel::kError,
                  "late match compensation completion had no pending owner");
      return;
    }
    if (repaired.has_value()) {
      ScheduleMatchCompensationRetry(room_id, *repaired);
      return;
    }
    auto completed = std::move(pending->second);
    pending_match_compensations_.erase(pending);
    match_compensation_retries_.erase(room_id);
    pending_match_starts_.erase(room_id);
    FailMatchStart(room_id, completed.sender, std::move(completed.completion),
                   completed.install_error);
    ProcessDeferredDisconnects(room_id);
  }

  void ScheduleMatchCompensationRetry(domain::RoomId room_id,
                                      const application::ServiceError& error) {
    const auto pending = pending_match_compensations_.find(room_id);
    if (pending == pending_match_compensations_.end()) {
      return;
    }
    ++pending->second.failed_attempts;
    match_compensation_retries_.insert_or_assign(
        room_id, std::chrono::steady_clock::now() +
                     options_.start_compensation_retry_delay);
    logger_.Log(
        application::LogLevel::kCritical,
        "persisted match compensation will retry: " + error.safe_message);
    wake_.notify_one();
  }

  void FailMatchStart(std::optional<domain::RoomId> room_id,
                      std::optional<domain::PlayerId> sender,
                      WorldCommandCompletion&& completion,
                      const application::ServiceError& cause) {
    if (room_id && options_.equipment_relay)
      options_.equipment_relay->RevokeRoom(*room_id);
    const auto room =
        room_id.has_value() ? world_.Rooms().FindRoom(*room_id) : nullptr;
    logger_.Log(
        application::LogLevel::kError,
        "match start was returned to waiting: room=" +
            (room_id.has_value() ? std::to_string(room_id->Value()) : "none") +
            " mode=" + (room ? std::to_string(room->ModeId()) : "unknown") +
            " reason=" + cause.safe_message);
    if (!room_id.has_value()) {
      if (completion) {
        completion(cause);
      }
      return;
    }
    auto aborted = world_.AbortFailedMatchStart(
        *room_id, room && !room->RestrictedItems().empty() &&
                          cause.code == application::ErrorCode::kConflict
                      ? cause.safe_message
                      : "对局开局失败，房间已安全恢复，请稍后重试");
    if (const auto* error = std::get_if<application::ServiceError>(&aborted)) {
      if (completion) {
        completion(*error);
      }
      return;
    }
    auto abort_result = std::get<WorldCoordinatorResult>(std::move(aborted));
    if (room && room->UserHosted()) {
      auto status = protocol::UserHostStartStatus::kCannotStart;
      if (cause.code == application::ErrorCode::kInsufficientBalance)
        status = protocol::UserHostStartStatus::kInvalidPrize;
      else if (cause.safe_message.find("赏金卡") != std::string::npos)
        status = protocol::UserHostStartStatus::kMissingTicket;
      abort_result.deliveries.push_back(
          {room->HostPlayerId(),
           {997U, protocol::EncodeUserHostStartResponse(status)}});
    }
    auto dispatched = Dispatch(std::move(abort_result), sender);
    if (completion) {
      completion(std::move(dispatched));
    }
  }

  void BeginSettlement(WorldCoordinatorResult result,
                       std::optional<domain::PlayerId> sender,
                       WorldCommandCompletion completion) {
    if (!result.room_id.has_value() || !result.settlement_command.has_value()) {
      if (completion) {
        completion(MakeError(application::ErrorCode::kInternal,
                             "终局结果缺少房间或结算命令"));
      }
      return;
    }
    const auto room_id = *result.room_id;
    if (pending_settlements_.contains(room_id)) {
      if (completion) {
        completion(MakeError(application::ErrorCode::kConflict,
                             "同一房间已有终局持久化任务"));
      }
      return;
    }
    pending_settlements_.emplace(
        room_id, PendingSettlement{.result = std::move(result),
                                   .sender = sender,
                                   .completion = std::move(completion),
                                   .failed_attempts = 0U});
    SubmitSettlement(room_id);
  }

  void SubmitSettlement(domain::RoomId room_id) {
    const auto pending = pending_settlements_.find(room_id);
    if (pending == pending_settlements_.end() ||
        !pending->second.result.settlement_command.has_value()) {
      return;
    }
    const auto command = *pending->second.result.settlement_command;
    // lambda 函数体已有 catch-all，且日志/入队端口均为 noexcept；LLVM 19
    // 仍把拥有型 command 闭包误判为异常越过 noexcept
    // 边界，因此只抑制这一处已审计误报。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    const auto post_error = background_tasks_.Post([this, room_id,
                                                    command]() noexcept {
      try {
        application::ServiceResult<DurableMatchSettlementSubmission> submitted =
            MakeError(application::ErrorCode::kInternal,
                      "终局耐久任务没有产生结果");
        try {
          submitted = settlements_.Submit(command);
        } catch (const std::exception&) {
          submitted = MakeError(application::ErrorCode::kInternal,
                                "终局耐久任务发生未分类错误");
        } catch (...) {
          submitted = MakeError(application::ErrorCode::kInternal,
                                "终局耐久任务发生未知错误");
        }
        static_cast<void>(EnqueueInternal(
            [this, room_id, submitted = std::move(submitted)]() mutable {
              FinishSettlement(room_id, std::move(submitted));
            }));
      } catch (...) {
        logger_.Log(
            application::LogLevel::kCritical,
            "settlement background task could not retain its completion");
      }
    });
    if (post_error.has_value()) {
      ScheduleSettlementRetry(room_id, *post_error);
    }
  }

  void FinishSettlement(
      domain::RoomId room_id,
      application::ServiceResult<DurableMatchSettlementSubmission> submitted) {
    const auto pending = pending_settlements_.find(room_id);
    if (pending == pending_settlements_.end() ||
        !pending->second.result.settlement_command.has_value()) {
      logger_.Log(application::LogLevel::kError,
                  "late settlement completion had no pending room owner");
      return;
    }
    if (const auto* error =
            std::get_if<application::ServiceError>(&submitted)) {
      ScheduleSettlementRetry(room_id, *error);
      return;
    }
    const auto* settlement_command =
        &*pending->second.result.settlement_command;
    const auto match_id = settlement_command->match_id;
    auto acknowledged = world_.AcknowledgeSettlementStaged(room_id, match_id);
    if (const auto* error =
            std::get_if<application::ServiceError>(&acknowledged)) {
      ScheduleSettlementRetry(room_id, *error);
      return;
    }

    auto completed = std::move(pending->second);
    pending_settlements_.erase(pending);
    settlement_retries_.erase(room_id);
    completed.result.settlement_command.reset();
    auto dispatched = Dispatch(std::move(completed.result), completed.sender);
    if (completed.completion) {
      completed.completion(std::move(dispatched));
    } else if (const auto* error =
                   std::get_if<application::ServiceError>(&dispatched)) {
      logger_.Log(
          application::LogLevel::kError,
          "settled room result dispatch failed: " + error->safe_message);
    }
    if (const auto reward = early_reward_results_.find(room_id);
        reward != early_reward_results_.end()) {
      ApplyRewardResult(reward->second);
      early_reward_results_.erase(reward);
    }
    ProcessDeferredDisconnects(room_id);
  }

  void ScheduleSettlementRetry(domain::RoomId room_id,
                               const application::ServiceError& error) {
    const auto pending = pending_settlements_.find(room_id);
    if (pending == pending_settlements_.end()) {
      return;
    }
    ++pending->second.failed_attempts;
    const auto& command = *pending->second.result.settlement_command;
    const auto diagnostic =
        " room=" + std::to_string(room_id.Value()) +
        " match=" + command.match_id.ToUuidString() +
        " attempt=" + std::to_string(pending->second.failed_attempts) + ": " +
        error.safe_message;
    if (!error.is_retryable) {
      // Keep the immutable command and room gate for investigation; never
      // publish unpersisted results or hammer the journal with a permanent
      // snapshot conflict.
      settlement_retries_.erase(room_id);
      logger_.Log(
          application::LogLevel::kCritical,
          "match settlement stage blocked by permanent failure" + diagnostic);
      if (pending->second.completion) {
        auto completion = std::move(pending->second.completion);
        completion(error);
      }
      return;
    }
    settlement_retries_.insert_or_assign(
        room_id,
        std::chrono::steady_clock::now() + options_.settlement_retry_delay);
    logger_.Log(application::LogLevel::kWarning,
                "match settlement stage will retry" + diagnostic);
    wake_.notify_one();
  }

  void ProcessDeferredDisconnects(domain::RoomId room_id) {
    auto found = deferred_disconnects_.find(room_id);
    if (found == deferred_disconnects_.end()) {
      return;
    }
    auto disconnected = std::move(found->second);
    deferred_disconnects_.erase(found);
    for (const auto& item : disconnected) {
      ExecuteDisconnect(item.player_id, item.phase);
    }
  }

  /** @brief 只在 world worker 上登记计时器；worker
   * 下一轮等待前会重新计算最早截止点。 */
  void Schedule(const std::vector<RoomTimerToken>& timers) {
    for (const auto& timer : timers) {
      timers_.Upsert(timer);
    }
  }

  [[nodiscard]] std::optional<SteadyTime> NextDeadline() const {
    std::optional<SteadyTime> result = next_recovery_;
    if (!pending_equipment_starts_.empty() &&
        (!result || next_equipment_check_ < *result))
      result = next_equipment_check_;
    for (const auto candidate :
         {timers_.NextDeadline(), EarliestRetry(settlement_retries_),
          EarliestRetry(match_compensation_retries_),
          EarliestRetry(tutorial_retries_), world_.TutorialDeadline()}) {
      if (candidate.has_value() &&
          (!result.has_value() || *candidate < *result)) {
        result = candidate;
      }
    }
    return result;
  }

  void RunDueWorkIfDue() {
    const auto deadline = NextDeadline();
    if (!deadline.has_value()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= *deadline) {
      RunDueWork(now);
    }
  }

  /**
   * @brief 执行本轮开始前已经到期的计时器、补偿/结算重试和周期恢复。
   * @note
   * 每类只处理进入本轮时已存在的数量，处理期间新登记的零时长项留到下一轮，避免自旋。
   */
  void RunDueWork(SteadyTime now) {
    for (auto& result : world_.TickTutorials(now))
      RouteResult(std::move(result), std::nullopt, {}, now);
    for (auto remaining = tutorial_retries_.size(); remaining != 0U;
         --remaining) {
      const auto player = PopDueRetry(tutorial_retries_, now);
      if (!player) break;
      SubmitTutorialWork(*player);
    }
    for (auto remaining = timers_.size(); remaining != 0U; --remaining) {
      const auto timer = timers_.PopDue(now);
      if (!timer.has_value()) {
        break;
      }
      ProcessTimer(*timer);
    }
    for (auto remaining = match_compensation_retries_.size(); remaining != 0U;
         --remaining) {
      const auto room_id = PopDueRetry(match_compensation_retries_, now);
      if (!room_id.has_value()) {
        break;
      }
      SubmitMatchCompensation(*room_id);
    }
    for (auto remaining = settlement_retries_.size(); remaining != 0U;
         --remaining) {
      const auto room_id = PopDueRetry(settlement_retries_, now);
      if (!room_id.has_value()) {
        break;
      }
      SubmitSettlement(*room_id);
    }
    if (next_recovery_.has_value() && now >= *next_recovery_) {
      next_recovery_ = now + options_.recovery_interval;
      if (!recovery_in_flight_) {
        recovery_in_flight_ = true;
        BeginRecovery();
      }
    }
    if (!pending_equipment_starts_.empty() && now >= next_equipment_check_) {
      CheckEquipmentStarts();
    }
  }

  /** @brief
   * 每条任务后的轻量轮询：装备就绪门控和扩音器审核拉取保持原有逐任务频率。 */
  void AfterTask() {
    if (!pending_equipment_starts_.empty()) {
      CheckEquipmentStarts();
    }
    if (megaphone_access_ && !moderation_in_flight_ &&
        std::chrono::steady_clock::now() >= next_moderation_poll_) {
      BeginModerationPoll();
    }
  }

  void WorkerLoop() noexcept {
    try {
      WorkerLoopBody();
    } catch (const std::exception&) {
      logger_.Log(application::LogLevel::kCritical,
                  "world worker escaped an exception boundary and stopped");
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "world worker escaped an unknown exception and stopped");
    }
  }

  /**
   * @brief 每轮先处理全部已到期的计时工作，再执行一批有上限的排队任务。
   * @note
   * 旧实现只在队列为空时检查计时器，持续战斗流量会无限推迟加载超时、倒计时和终局。
   *       停止后不再触发计时/重试/恢复，只排空已受理任务。
   */
  void WorkerLoopBody() {
    std::vector<std::function<void()>> batch;
    batch.reserve(kMaxTasksPerTurn);
    while (true) {
      bool stopping = false;
      {
        std::unique_lock lock{mutex_};
        while (tasks_.empty() && !stopping_) {
          const auto deadline = NextDeadline();
          if (!deadline.has_value()) {
            wake_.wait(lock);
          } else if (*deadline > std::chrono::steady_clock::now()) {
            wake_.wait_until(lock, *deadline);
          } else {
            break;
          }
        }
        stopping = stopping_;
        if (stopping && tasks_.empty()) {
          return;
        }
        const auto count = std::min(tasks_.size(), kMaxTasksPerTurn);
        for (std::size_t index = 0U; index < count; ++index) {
          batch.push_back(std::move(tasks_.front()));
          tasks_.pop_front();
        }
      }
      if (!stopping && batch.empty()) {
        RunDueWorkIfDue();
      }
      for (auto& task : batch) {
        // 单个任务可能很慢；每条任务之间都检查截止点，计时误差上限为单任务耗时而非整批耗时。
        if (!stopping) {
          RunDueWorkIfDue();
        }
        try {
          task();
        } catch (...) {
          logger_.Log(application::LogLevel::kError,
                      "world task escaped its completion/error boundary");
        }
        AfterTask();
      }
      if (batch.empty()) {
        AfterTask();
      }
      batch.clear();
      RefreshMetrics();
    }
  }

  void RefreshMetrics() noexcept {
    active_rooms_.store(world_.Rooms().RoomCount(), std::memory_order_relaxed);
    plaza_players_.store(world_.Plaza().MemberCount(),
                         std::memory_order_relaxed);
    pending_match_starts_metric_.store(pending_match_starts_.size(),
                                       std::memory_order_relaxed);
    pending_match_settlements_metric_.store(pending_settlements_.size(),
                                            std::memory_order_relaxed);
    pending_match_compensations_metric_.store(
        pending_match_compensations_.size(), std::memory_order_relaxed);
    scheduled_timers_.store(timers_.size(), std::memory_order_relaxed);
  }

  void ProcessTimer(const RoomTimerToken& token) noexcept {
    try {
      if (pending_equipment_rooms_.contains(token.room_id)) {
        deferred_equipment_commands_[token.room_id].push_back(
            [this, token] { ProcessTimer(token); });
        return;
      }
      if (RoomHasPendingExternalWork(token.room_id)) {
        return;
      }
      auto result = world_.ApplyTimer(token, std::chrono::steady_clock::now());
      if (const auto* error = std::get_if<application::ServiceError>(&result)) {
        logger_.Log(application::LogLevel::kError,
                    "world timer failed: " + error->safe_message);
        return;
      }
      RouteResult(std::get<WorldCoordinatorResult>(std::move(result)),
                  std::nullopt, {}, std::chrono::steady_clock::now());
    } catch (...) {
      logger_.Log(application::LogLevel::kError,
                  "world timer escaped its exception boundary");
    }
  }

  void BeginModerationPoll() {
    next_moderation_poll_ =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    const auto players = sessions_.OnlineSessions();
    if (players.empty()) return;
    moderation_in_flight_ = true;
    const auto error = background_tasks_.Post([this, players] {
      try {
        for (const auto& [player, handle] : players) {
          const auto access = megaphone_access_->Load(player);
          if (const auto* state =
                  std::get_if<application::WorldAccessSnapshot>(&access)) {
            if (state->banned) sessions_.DisconnectIfCurrent(player, handle);
            if (state->muted || state->banned)
              static_cast<void>(EnqueueInternal([this, player, handle] {
                if (sessions_.IsCurrentPlayerSession(player, handle))
                  static_cast<void>(Dispatch(world_.SuppressRecruitment(player),
                                             std::nullopt));
              }));
          }
        }
      } catch (...) {
        logger_.Log(application::LogLevel::kWarning,
                    "moderation refresh failed; retry pending");
      }
      static_cast<void>(
          EnqueueInternal([this] { moderation_in_flight_ = false; }));
    });
    if (error) moderation_in_flight_ = false;
  }

  void BeginRecovery() {
    const auto post_error = background_tasks_.Post([this] {
      // 与结算共享周期唤醒；加载或屏蔽查询失败时保留耐久待办，不广播未验证记录。
      if (megaphones_ && megaphone_access_) {
        try {
          auto pending = megaphones_->PendingMegaphones();
          if (const auto* values =
                  std::get_if<std::vector<application::MegaphoneReceipt>>(
                      &pending)) {
            for (const auto& receipt : *values) {
              auto access = megaphone_access_->Load(receipt.player_id);
              if (const auto* value =
                      std::get_if<application::WorldAccessSnapshot>(&access)) {
                if (value->muted || value->banned ||
                    domain::IsRestrictedMessage(receipt.text)) {
                  static_cast<void>(
                      megaphones_->AcknowledgeMegaphone(receipt.business_id));
                  continue;
                }
                static_cast<void>(EnqueueInternal(
                    [this, receipt, blocked = value->chat_blocked_peers] {
                      DeliverMegaphone(receipt, blocked);
                    }));
              }
            }
          }
        } catch (...) {
          logger_.Log(application::LogLevel::kWarning,
                      "megaphone recovery deferred by exception");
        }
      }
      application::ServiceResult<MatchSettlementRecoveryReport> recovered =
          MakeError(application::ErrorCode::kInternal,
                    "周期结算恢复任务没有产生结果");
      try {
        recovered = settlements_.Recover(options_.recovery_batch_size);
      } catch (const std::exception&) {
        recovered = MakeError(application::ErrorCode::kInternal,
                              "周期结算恢复发生未分类错误");
      } catch (...) {
        recovered = MakeError(application::ErrorCode::kInternal,
                              "周期结算恢复发生未知错误");
      }
      static_cast<void>(
          EnqueueInternal([this, recovered = std::move(recovered)]() mutable {
            recovery_in_flight_ = false;
            PruneMegaphoneCache();
            if (const auto* error =
                    std::get_if<application::ServiceError>(&recovered)) {
              logger_.Log(application::LogLevel::kWarning,
                          "periodic settlement recovery failed: " +
                              error->safe_message);
              return;
            }
            const auto& report =
                std::get<MatchSettlementRecoveryReport>(recovered);
            if (report.loaded != 0U || report.deferred != 0U) {
              logger_.Log(application::LogLevel::kInfo,
                          "periodic settlement recovery loaded=" +
                              std::to_string(report.loaded) +
                              " scheduled=" + std::to_string(report.scheduled) +
                              " deferred=" + std::to_string(report.deferred));
            }
          }));
    });
    if (post_error.has_value()) {
      recovery_in_flight_ = false;
      logger_.Log(
          application::LogLevel::kWarning,
          "periodic settlement recovery could not enter background pool");
    }
  }

  WorldExecutorOptions options_;
  SessionRegistry& sessions_;
  application::BackgroundTaskExecutor& background_tasks_;
  application::MatchPreparationExecutor& match_preparation_;
  application::MatchIdSource& match_ids_;
  application::MatchStartExecutor& match_starts_;
  application::MatchLifecycleRepairExecutor& match_repairs_;
  const application::SystemClock& clock_;
  DurableMatchSettlementFlow& settlements_;
  application::EventLogger& logger_;
  WorldCoordinator world_;
  application::ItemUseExecutor* megaphones_{nullptr};
  application::WorldAccessLoader* megaphone_access_{nullptr};
  std::map<std::string, std::set<domain::PlayerId>> megaphone_sent_;
  std::set<std::string> megaphone_completed_;
  std::set<std::string> megaphone_acknowledged_;
  std::deque<std::string> megaphone_completed_order_;
  std::size_t megaphone_work_in_flight_{0U};
  std::set<domain::RoomId> pending_match_starts_;
  std::map<domain::PlayerId, PendingTutorialWork> pending_tutorials_;
  std::map<domain::PlayerId, std::pair<SessionHandle, domain::MatchId>>
      tutorial_owners_;
  std::map<domain::PlayerId, SessionHandle>
      online_gateway_sessions_;  ///< Distinguish new login from same-handle
                                 ///< asset refresh.
  std::map<domain::PlayerId, SteadyTime> tutorial_retries_;
  std::map<domain::PlayerId, std::deque<std::function<void()>>>
      deferred_tutorial_commands_;
  std::map<domain::PlayerId, WorldSessionPhase> deferred_tutorial_disconnects_;
  std::map<domain::RoomId, PendingEquipmentStart> pending_equipment_starts_;
  std::chrono::steady_clock::time_point next_equipment_check_{};
  std::map<domain::RoomId, PendingMatchCompensation>
      pending_match_compensations_;
  std::map<domain::RoomId, PendingSettlement> pending_settlements_;
  std::set<domain::RoomId>
      pending_equipment_rooms_;  ///< 换装提交与开局之间的串行互斥。
  std::map<domain::RoomId, std::vector<std::function<void()>>>
      deferred_equipment_commands_;
  std::map<domain::RoomId, application::MatchSettlementResult>
      early_reward_results_;
  std::map<domain::RoomId, std::vector<DeferredDisconnect>>
      deferred_disconnects_;
  std::map<domain::PlayerId, std::chrono::steady_clock::time_point>
      rejection_logs_;
  mutable std::mutex mutex_;  ///< 只保护 tasks_ 与启动/停止标志。
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  // 以下计时/重试状态只由 world worker 读写，不需要 mutex_。
  RoomTimerTable timers_;
  std::map<domain::RoomId, SteadyTime> settlement_retries_;
  std::map<domain::RoomId, SteadyTime> match_compensation_retries_;
  std::optional<SteadyTime> next_recovery_;
  std::thread worker_;
  bool started_{false};
  bool accepting_{false};
  bool stopping_{false};
  bool recovery_in_flight_{false};
  bool moderation_in_flight_{false};
  std::chrono::steady_clock::time_point next_moderation_poll_{};
  std::atomic<std::size_t> active_rooms_{0U};
  std::atomic<std::size_t> plaza_players_{0U};
  std::atomic<std::size_t> scheduled_timers_{0U};
  std::atomic<std::size_t> pending_match_starts_metric_{0U};
  std::atomic<std::size_t> pending_match_settlements_metric_{0U};
  std::atomic<std::size_t> pending_match_compensations_metric_{0U};
};

WorldExecutor::WorldExecutor(
    WorldExecutorOptions options, SessionRegistry& sessions,
    application::BackgroundTaskExecutor& background_tasks,
    application::MatchPreparationExecutor& match_preparation,
    application::MatchIdSource& match_ids,
    application::MatchStartExecutor& match_starts,
    application::MatchLifecycleRepairExecutor& match_repairs,
    const application::SystemClock& clock,
    DurableMatchSettlementFlow& settlements, application::EventLogger& logger,
    application::ItemUseExecutor* megaphones,
    application::WorldAccessLoader* megaphone_access)
    : impl_(std::make_unique<Impl>(options, sessions, background_tasks,
                                   match_preparation, match_ids, match_starts,
                                   match_repairs, clock, settlements, logger,
                                   megaphones, megaphone_access)) {}

std::optional<application::ServiceError> WorldExecutor::PostMegaphone(
    application::MegaphoneCommand command, MegaphoneWorldTask work,
    WorldCommandCompletion completion) {
  return impl_->PostMegaphone(std::move(command), std::move(work),
                              std::move(completion));
}

WorldExecutor::~WorldExecutor() = default;

std::optional<application::ServiceError> WorldExecutor::Start() {
  return impl_->Start();
}

std::optional<application::ServiceError> WorldExecutor::Post(
    WorldActorContext actor, protocol::Packet packet,
    WorldCommandCompletion completion) {
  return impl_->Post(std::move(actor), std::move(packet),
                     std::move(completion));
}

std::optional<application::ServiceError> WorldExecutor::PostPlayerOnline(
    application::PlayerMenuSnapshot snapshot,
    std::optional<SessionHandle> origin) {
  return impl_->PostPlayerOnline(std::move(snapshot), origin);
}

std::optional<application::ServiceError> WorldExecutor::PostCoupleProfiles(
    std::vector<application::PlayerMenuSnapshot> snapshots) {
  return impl_->PostCoupleProfiles(std::move(snapshots));
}

std::optional<application::ServiceError> WorldExecutor::PostDisconnect(
    domain::PlayerId player_id, WorldSessionPhase phase,
    std::optional<SessionHandle> origin) {
  return impl_->PostDisconnect(player_id, phase, origin);
}

std::optional<application::ServiceError> WorldExecutor::PostRoomHandoff(
    WorldActorContext actor, bool entering, WorldCommandCompletion completion) {
  return impl_->PostRoomHandoff(std::move(actor), entering,
                                std::move(completion));
}

void WorldExecutor::DrainAndStop() noexcept { impl_->DrainAndStop(); }

bool WorldExecutor::PostSettlementResult(
    application::MatchSettlementResult result) noexcept {
  return impl_->PostSettlementResult(std::move(result));
}

std::optional<application::ServiceError> WorldExecutor::PostInstantRespawn(
    domain::PlayerId player, std::uint32_t object, InstantRespawnWorldTask work,
    WorldCommandCompletion completion) {
  return impl_->PostInstantRespawn(player, object, std::move(work),
                                   std::move(completion));
}

std::optional<application::ServiceError> WorldExecutor::PostEquipmentChange(
    domain::PlayerId player, EquipmentWorldTask work,
    WorldCommandCompletion completion, bool preset_selection,
    std::optional<SessionHandle> origin) {
  return impl_->PostEquipmentChange(
      player, std::move(work), std::move(completion), preset_selection, origin);
}

std::size_t WorldExecutor::QueuedTaskCount() const noexcept {
  return impl_->QueuedTaskCount();
}

WorldExecutorMetrics WorldExecutor::Metrics() const noexcept {
  return impl_->Metrics();
}

}  // namespace bf::runtime
