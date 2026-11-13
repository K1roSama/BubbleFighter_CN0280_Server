#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_STATE_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_STATE_H_

/**
 * @file room_gateway_state.h
 * @brief 定义网关房间目录私有状态及单事件循环编排边界。
 */
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "network/room_gateway.h"
#include "network/room_rpc_client.h"
#include "network/room_value_codec.h"

namespace bf::infrastructure::network {
/** @brief 房间网关的固定线程、节点、成员路由和持久化待办。 */
class RoomGateway::Impl final
    : public std::enable_shared_from_this<RoomGateway::Impl> {
 public:
  using Json = nlohmann::json;
  using Time = std::chrono::steady_clock::time_point;
  using Done = runtime::WorldCommandCompletion;
  using JsonReply = std::function<void(application::ServiceResult<Json>)>;

  /** @brief 一台 Agent 的认证连接及最近一轮容量和租约确认。 */
  struct Node final {
    RoomNodeEndpoint endpoint;
    std::shared_ptr<RoomRpcClient> rpc;
    std::optional<domain::MatchId> id;
    Time valid_until{};
    std::size_t room_count{0U};
    std::size_t available_rooms{0U};
    std::set<domain::MatchId::ByteArray> reservations;
    std::uint64_t cpu{1000U}, memory{0U}, budget{0U}, handles{0U};
    bool connected{false}, registered{false}, polling{false}, retired{false},
        draining{false};
  };

  /** @brief 房间输出的目标会话及用于安全返回大厅的值快照。 */
  struct Member final {
    runtime::SessionHandle origin;
    runtime::WorldActorContext actor;
    std::uint64_t sequence{0U};
    bool handed_off{false};
    bool handing_off{false};
  };

  /** @brief 拥有型任务与入队时间，避免保存无界观测样本。 */
  struct QueuedWork final {
    std::function<void()> execute;
    Time received;
  };

  /** @brief 一个独立 DS 的路由元数据，不保存权威战斗或房间运行时。 */
  struct Room final {
    protocol::internal::RoomIdentity identity;
    std::shared_ptr<Node> node;
    std::shared_ptr<RoomRpcClient> rpc;
    std::map<domain::PlayerId, Member> members;
    std::optional<protocol::SceneInfo> scene;
    std::deque<QueuedWork> queue;
    std::optional<domain::MatchId> match;
    std::optional<application::MatchSettlementResult> early_reward;
    std::optional<Json> terminal;
    std::optional<application::PreparedMatch> prepared;
    std::optional<application::MatchStartCommand> start_command;
    std::vector<runtime::EquipmentRelayViewer> viewers;
    Json activation;
    Time allocation_started{}, lease_until{}, next_retry{},
        equipment_deadline{};
    std::uint64_t generation{0U}, control{0U};
    std::uint64_t timer_p99_us{0U};
    std::uint32_t channel{60U};
    bool busy{false}, blocked{false}, dead{false}, ready{false},
        starting{false};
    bool start_in_flight{false}, terminal_in_flight{false},
        waiting_equipment{false};
    bool terminal_staged{false}, closed{false};
    std::set<domain::PlayerId> returning;
  };

  /** @brief admission 的计数与字节预算，由最后一个持有者归还。 */
  struct Admission final {
    std::weak_ptr<Impl> owner;
    std::pair<bool, std::uint64_t> key;
    std::size_t bytes;
    ~Admission();
  };

  Impl(RoomGatewayOptions options, runtime::SessionRegistry& sessions,
       application::BackgroundTaskExecutor& background,
       application::RoomLeaseStore& leases,
       application::MatchPreparationExecutor& preparation,
       application::MatchIdSource& ids, application::MatchStartExecutor& starts,
       const application::SystemClock& clock,
       runtime::DurableMatchSettlementFlow& settlements,
       application::PlayerMenuSnapshotLoader& menus,
       application::EventLogger& logger);
  void Start(runtime::WorldExecutor& lobby);
  void Stop();
  void Tick();
  void PollNode(const std::shared_ptr<Node>& node);
  void NodeLost(const std::shared_ptr<Node>& node);
  void FinishExternal();
  void Handoff(const std::shared_ptr<Room>& room, domain::PlayerId player,
               bool entering, Done done);
  void ReturnPlayer(const std::shared_ptr<Room>& room, domain::PlayerId player);
  void ForgetPlayer(const std::shared_ptr<Room>& room, domain::PlayerId player);
  void Retire(const std::shared_ptr<Room>& room, bool failed);
  void FailRoom(const std::shared_ptr<Room>& room);
  void Create(runtime::WorldActorContext actor, protocol::Packet packet,
              Done done);
  void Allocate(const std::shared_ptr<Room>& room, domain::PlayerId player,
                protocol::Packet packet, Done done);
  void Join(runtime::WorldActorContext actor, protocol::Packet packet,
            Done done);
  void Packet(const std::shared_ptr<Room>& room,
              runtime::WorldActorContext actor, protocol::Packet packet,
              Done done);
  void Enqueue(const std::shared_ptr<Room>& room, std::function<void()> work,
               Time received = {});
  void Pump(const std::shared_ptr<Room>& room);
  void Finish(const std::shared_ptr<Room>& room);
  void Rpc(const std::shared_ptr<Room>& room, Json value, JsonReply reply,
           protocol::internal::RoomMessageKind kind =
               protocol::internal::RoomMessageKind::kCommand,
           std::optional<domain::PlayerId> player = std::nullopt);
  void Receive(const std::shared_ptr<Room>& room,
               protocol::internal::RoomEnvelope envelope, JsonReply reply = {});
  void Apply(const std::shared_ptr<Room>& room, Json value);
  void Deliver(const std::shared_ptr<Room>& room,
               runtime::RoomCoordinatorResult result);
  void BeginStart(const std::shared_ptr<Room>& room, Json request);
  void PersistStart(const std::shared_ptr<Room>& room);
  void Bootstrap(const std::shared_ptr<Room>& room);
  void Install(const std::shared_ptr<Room>& room);
  void BeginTerminal(const std::shared_ptr<Room>& room);
  void Reward(const std::shared_ptr<Room>& room,
              application::MatchSettlementResult result);
  void Equipment(const std::shared_ptr<Room>& room, domain::PlayerId player,
                 runtime::EquipmentWorldTask work, Done done, bool preset);
  void Respawn(const std::shared_ptr<Room>& room, domain::PlayerId player,
               std::uint32_t object, runtime::InstantRespawnWorldTask work,
               Done done);
  [[nodiscard]] std::shared_ptr<Room> Find(domain::PlayerId player) const;
  [[nodiscard]] std::shared_ptr<Admission> Admit(domain::PlayerId player,
                                                 std::size_t bytes);
  void EntryFailed(domain::PlayerId player, std::uint16_t opcode, Done done,
                   std::string reason);
  [[nodiscard]] bool Current(const runtime::WorldActorContext& actor) const;
  [[nodiscard]] static application::ServiceError Failure(std::string reason);
  [[nodiscard]] static Json EncodePacket(
      const runtime::WorldActorContext& actor, const protocol::Packet& packet);
  [[nodiscard]] static runtime::OutboundMessage Notice(std::string text);
  [[nodiscard]] application::RoomOwnership Owner(
      const std::shared_ptr<Room>& room) const;

  template <typename Work, typename Completion>
  void Database(Work work, Completion complete) {
    using Result = decltype(work());
    ++external;
    const auto rejected =
        background.Post([self = shared_from_this(), work, complete]() mutable {
          Result result = Failure("room background task failed");
          try {
            result = work();
          } catch (...) {
          }
          boost::asio::post(
              self->io, [self, result = std::move(result), complete]() mutable {
                complete(std::move(result));
                self->FinishExternal();
              });
        });
    if (rejected) {
      complete(Result{*rejected});
      FinishExternal();
    }
  }

  RoomGatewayOptions options;
  runtime::SessionRegistry& sessions;
  application::BackgroundTaskExecutor& background;
  application::RoomLeaseStore& leases;
  application::MatchPreparationExecutor& preparation_executor;
  application::MatchIdSource& ids;
  application::MatchStartExecutor& starts;
  const application::SystemClock& clock;
  runtime::DurableMatchSettlementFlow& settlements;
  application::PlayerMenuSnapshotLoader& menus;
  application::EventLogger& logger;
  runtime::WorldExecutor* lobby{nullptr};
  boost::asio::io_context io{1};
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_guard{io.get_executor()};
  boost::asio::steady_timer timer{io};
  std::thread worker;
  std::vector<std::shared_ptr<Node>> nodes;
  std::map<domain::RoomId, std::shared_ptr<Room>> rooms;
  std::size_t external{0U};
  std::size_t allocating{0U};
  Time next_nodes{}, next_recovery{};
  bool stopping{false}, recovering{false};
  bool checking_drain{false};
  mutable std::mutex mutex;
  std::map<domain::PlayerId, std::shared_ptr<Room>> player_rooms;
  std::set<domain::PlayerId> pending_players;
  std::map<std::pair<bool, std::uint64_t>, std::pair<std::size_t, std::size_t>>
      budgets;
  std::map<std::string, std::weak_ptr<Room>> matches;
  RoomGatewayMetrics metrics;
  runtime::LatencyHistogram command_queue_latency, immediate_rpc_latency;
  bool accepting{false}, started{false};
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_STATE_H_
