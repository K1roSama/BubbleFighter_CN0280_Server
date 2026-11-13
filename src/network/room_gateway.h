#ifndef BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_H_

/**
 * @file room_gateway.h
 * @brief 定义独立房间的目录、节点调度及网关持久化编排。
 */
#include <filesystem>

#include "features/rooms/room_lease.h"
#include "network/room_tls_channel.h"
#include "sessions/latency_histogram.h"
#include "sessions/world_executor.h"

namespace bf::infrastructure::network {
/** @brief 一台可信 Agent 的固定端点及证书身份。 */
struct RoomNodeEndpoint final {
  std::string name;
  std::string host;
  std::uint16_t port{0U};
  std::string peer_name;
};

/** @brief 网关资源预算；独立后端必须在排空房间后才能切换。 */
struct RoomGatewayOptions final {
  RoomTlsOptions tls;
  std::vector<RoomNodeEndpoint> nodes;
  std::uint32_t region_id{1U};
  std::size_t maximum_rooms{1024U};
  std::size_t maximum_commands{4096U};
  std::size_t maximum_queued_bytes{64U * 1024U * 1024U};
  std::uint16_t room_capacity{12U};
  std::filesystem::path drain_marker;
  runtime::EquipmentRelayBroker* equipment_relay{nullptr};
  std::array<std::uint8_t, 4U> equipment_relay_address{};
  std::uint16_t equipment_relay_port{0U};
};

/** @brief 加载独立私有配置，证书相对路径以配置文件目录为根。 */
[[nodiscard]] RoomGatewayOptions LoadRoomGatewayOptions(
    const std::filesystem::path& path);

/** @brief 可从任意网关线程读取的有界运行快照。 */
struct RoomGatewayMetrics final {
  std::size_t rooms{0U};
  std::size_t queued_commands{0U};
  std::size_t queued_bytes{0U};
  std::size_t healthy_nodes{0U};
  std::uint64_t rejected_commands{0U};
  std::uint64_t failed_rooms{0U};
  bool draining{false};
  runtime::LatencySummary command_queue;
  runtime::LatencySummary immediate_rpc;
  std::uint64_t worst_room_timer_p99_us{0U};
};

/** @brief 一个固定事件线程管理通信；阻塞业务仅交给共享有界后台池。 */
class RoomGateway final : public runtime::RoomExecution {
 public:
  RoomGateway(RoomGatewayOptions options, runtime::SessionRegistry& sessions,
              application::BackgroundTaskExecutor& background,
              application::RoomLeaseStore& leases,
              application::MatchPreparationExecutor& preparation,
              application::MatchIdSource& ids,
              application::MatchStartExecutor& starts,
              const application::SystemClock& clock,
              runtime::DurableMatchSettlementFlow& settlements,
              application::PlayerMenuSnapshotLoader& menus,
              application::EventLogger& logger);
  ~RoomGateway();
  RoomGateway(const RoomGateway&) = delete;
  RoomGateway& operator=(const RoomGateway&) = delete;
  /** @brief 绑定已构造但尚未开放客户端监听的大厅执行器。 */
  void Start(runtime::WorldExecutor& lobby);
  /** @brief 停止建房，现有房间继续运行，供排空发布。 */
  void Drain();
  /** @brief 关闭通信、完成已接管回调并等待后台工作回收。 */
  void Stop();
  [[nodiscard]] RoomGatewayMetrics Metrics() const;
  [[nodiscard]] bool Routes(const runtime::WorldActorContext& actor,
                            const protocol::Packet& packet) const override;
  [[nodiscard]] bool Contains(domain::PlayerId player) const override;
  [[nodiscard]] std::optional<application::ServiceError> Post(
      runtime::WorldActorContext actor, protocol::Packet packet,
      runtime::WorldCommandCompletion completion) override;
  void Disconnect(domain::PlayerId player,
                  std::optional<runtime::SessionHandle> origin) override;
  [[nodiscard]] std::optional<application::ServiceError> Equipment(
      domain::PlayerId player, runtime::EquipmentWorldTask work,
      runtime::WorldCommandCompletion completion, bool preset_selection,
      std::optional<runtime::SessionHandle> origin) override;
  [[nodiscard]] std::optional<application::ServiceError> InstantRespawn(
      domain::PlayerId player, std::uint32_t object,
      runtime::InstantRespawnWorldTask work,
      runtime::WorldCommandCompletion completion) override;
  [[nodiscard]] bool SettlementResult(
      application::MatchSettlementResult result) override;

 private:
  /** @brief 隐藏节点连接、房间命令及持久化状态；单线程拥有权威路由目录。 */
  class Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_ROOM_GATEWAY_H_
