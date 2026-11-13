#include "operations/process/ds_agent.h"

#include <openssl/rand.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "network/room_tls_channel.h"
#include "network/room_value_codec.h"
#include "operations/process/child_process.h"

namespace bf::infrastructure::process {
namespace asio = boost::asio;
using Json = nlohmann::json;
using bf::infrastructure::network::RoomTlsChannel;
using bf::infrastructure::process::ChildProcess;
using bf::protocol::internal::RoomEnvelope;
using bf::protocol::internal::RoomIdentity;
using Clock = std::chrono::steady_clock;

class DsAgent::Impl final {
 public:
  Impl(asio::io_context& io, const Json& config,
       std::filesystem::path config_path)
      : io_(io),
        config_(config),
        config_path_(std::move(config_path)),
        acceptor_(io),
        timer_(io) {
    bf::domain::MatchId::ByteArray random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
      throw std::runtime_error{"node identity generation failed"};
    node_id_ = bf::domain::MatchId::FromBytes(random)->ToUuidString();
    const bf::infrastructure::network::RoomTlsOptions options{
        config.at("certificate"), config.at("private_key"),
        config.at("authority"), config.at("gateway_peer_name")};
    bf::infrastructure::network::ConfigureRoomTls(tls_, options);
    room_executable_ = std::filesystem::absolute(
        config.at("room_executable").get<std::string>());
    max_rooms_ = config.value("max_rooms", 64U);
    prewarm_ = config.value("prewarm", 2U);
    const auto memory_limit_mb = config.value("room_memory_limit_mb", 128ULL);
    const auto memory_budget_mb = config.value("memory_budget_mb", 4096ULL);
    if (memory_limit_mb < 32U || memory_limit_mb > 65536U ||
        memory_budget_mb < 64U || memory_budget_mb > 1048576U)
      throw std::invalid_argument{"invalid DS memory budget"};
    memory_limit_ = memory_limit_mb * 1024U * 1024U;
    memory_budget_ = memory_budget_mb * 1024U * 1024U;
    handle_budget_ = config.value("handle_budget", 65536ULL);
    if (!max_rooms_ || max_rooms_ > 4096U || prewarm_ > max_rooms_ ||
        handle_budget_ < 256U || handle_budget_ > 1'000'000'000U ||
        memory_limit_ < 32U * 1024U * 1024U ||
        memory_limit_ > memory_budget_ * 7U / 10U)
      throw std::invalid_argument{"invalid DS node capacity"};
    if (!std::filesystem::is_regular_file(room_executable_))
      throw std::invalid_argument{"missing room executable"};
    if (config.contains("drain_marker")) {
      const auto marker = config.at("drain_marker").get<std::string>();
      if (marker.empty())
        throw std::invalid_argument{"empty node drain marker"};
      drain_marker_ =
          std::filesystem::absolute(config_path_.parent_path() / marker);
    }
    const auto port = config.value("port", 0U);
    if (port > 65535U) throw std::invalid_argument{"invalid DS agent port"};
    asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address(config.at("bind_address").get<std::string>()),
        static_cast<std::uint16_t>(port)};
    acceptor_.open(endpoint.protocol());
    acceptor_.bind(endpoint);
    acceptor_.listen(8);
  }

  ~Impl() { children_.clear(); }

  std::uint16_t Port() const { return acceptor_.local_endpoint().port(); }

  void Start() {
    Accept();
    Sample();
  }

  void Stop() {
    if (stopped_) return;
    stopped_ = true;
    draining_ = true;
    timer_.cancel();
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    for (auto& channel : channels_) channel->Close();
    channels_.clear();
    children_.clear();
    io_.stop();
  }

 private:
  struct Child final {
    std::unique_ptr<ChildProcess> process;
    Clock::time_point launched;
    std::string startup;
    std::uint16_t port{0U};
    std::optional<RoomIdentity> identity;
    std::uint64_t previous_cpu{0U};
  };

  void Accept() {
    acceptor_.async_accept([this](auto error, auto socket) {
      if (error || stopped_) return;
      std::erase_if(channels_, [](const auto& channel) {
        return channel.use_count() == 1;
      });
      if (channels_.size() >= 8U) {
        boost::system::error_code ignored;
        socket.close(ignored);
      } else {
        auto channel = std::make_shared<RoomTlsChannel>(
            std::move(socket), tls_,
            config_.at("gateway_peer_name").get<std::string>());
        channels_.push_back(channel);
        std::weak_ptr<RoomTlsChannel> weak = channel;
        channel->Start(
            [this, weak](RoomEnvelope message) {
              if (auto target = weak.lock())
                Receive(target, std::move(message));
            },
            {}, [] {});
      }
      Accept();
    });
  }

  void Receive(const std::shared_ptr<RoomTlsChannel>& channel,
               RoomEnvelope message) {
    try {
      const auto payload =
          bf::infrastructure::network::ParseRoomPayload(message.payload);
      if (!message.request_id ||
          message.kind != bf::protocol::internal::RoomMessageKind::kCommand)
        throw std::invalid_argument{"invalid node control message"};
      auto reply = Execute(message, payload);
      message.kind = bf::protocol::internal::RoomMessageKind::kReply;
      const auto encoded = reply.dump();
      message.payload.assign(encoded.begin(), encoded.end());
      static_cast<void>(channel->Send(std::move(message)));
    } catch (const std::exception&) {
      message.kind = bf::protocol::internal::RoomMessageKind::kError;
      const std::string error =
          R"({"ok":false,"detail":"invalid DS agent request"})";
      message.payload.assign(error.begin(), error.end());
      static_cast<void>(channel->Send(std::move(message)));
    }
  }

  Json Execute(const RoomEnvelope& message, const Json& payload) {
    const auto op = payload.at("op").get<std::string>();
    if (op == "status") {
      Json rooms = Json::array();
      for (const auto& [pid, child] : children_) {
        const auto metrics = child.process->Metrics();
        rooms.push_back(
            {{"pid", pid},
             {"ready", child.port != 0U},
             {"room_id", child.identity ? child.identity->room_id.Value() : 0U},
             {"instance_id", child.identity ? Json(child.identity->instance_id)
                                            : Json(nullptr)},
             {"owner_epoch", child.identity ? child.identity->owner_epoch : 0U},
             {"private_bytes", metrics.private_bytes},
             {"handles", metrics.handles},
             {"cpu_microseconds", metrics.cpu_microseconds}});
      }
      return {{"ok", true},
              {"node_id", node_id_},
              {"draining", draining_},
              {"rooms", rooms},
              {"cpu_per_mille", cpu_per_mille_},
              {"private_bytes", sampled_memory_},
              {"handles", sampled_handles_},
              {"memory_budget_bytes", memory_budget_ * 7U / 10U},
              {"max_rooms", max_rooms_},
              {"available_rooms", AvailableRooms()},
              {"prewarm", prewarm_},
              {"reserved_memory_bytes", children_.size() * memory_limit_},
              {"launched_total", launched_total_},
              {"exited_total", exited_total_},
              {"startup_failures_total", startup_failures_total_},
              {"startup_timeouts_total", startup_timeouts_total_}};
    }
    if (op == "drain") {
      draining_ = true;
      return {{"ok", true}};
    }
    if (op == "allocate") {
      if (!message.identity.IsValid())
        throw std::invalid_argument{"invalid room allocation"};
      for (const auto& [pid, child] : children_) {
        if (child.identity == message.identity &&
            !child.process->Metrics().exit_code)
          return {{"ok", true},
                  {"pid", pid},
                  {"host", config_.at("advertise_address")},
                  {"port", child.port}};
        if (child.identity &&
            child.identity->room_id == message.identity.room_id)
          return {{"ok", false}, {"detail", "room id already allocated"}};
      }
      if (draining_ || cpu_per_mille_ >= 700U ||
          sampled_handles_ >= handle_budget_ * 7U / 10U)
        return {{"ok", false},
                {"detail", "node is draining or overloaded"},
                {"retryable", true}};
      for (auto& [pid, child] : children_) {
        if (child.identity || !child.port || child.process->Metrics().exit_code)
          continue;
        child.identity = message.identity;
        return {{"ok", true},
                {"pid", pid},
                {"host", config_.at("advertise_address")},
                {"port", child.port}};
      }
      if (CanSpawn()) Spawn();
      return {{"ok", false},
              {"detail", "room process warming up or node full"},
              {"retryable", true}};
    }
    if (op == "release") {
      if (!message.identity.IsValid())
        throw std::invalid_argument{"invalid release identity"};
      for (auto iterator = children_.begin(); iterator != children_.end();
           ++iterator)
        if (iterator->second.identity == message.identity) {
          children_.erase(iterator);
          return {{"ok", true}, {"released", true}};
        }
      return {{"ok", true}, {"released", false}};
    }
    throw std::invalid_argument{"unsupported node operation"};
  }

  std::size_t AvailableRooms() const {
    if (draining_ || cpu_per_mille_ >= 700U ||
        sampled_handles_ >= handle_budget_ * 7U / 10U)
      return 0U;
    const auto idle = static_cast<std::size_t>(std::ranges::count_if(
        children_, [](const auto& entry) { return !entry.second.identity; }));
    const auto process_limit = std::min<std::uint64_t>(
        max_rooms_, memory_budget_ * 7U / 10U / memory_limit_);
    const auto spawn_slots = process_limit > children_.size()
                                 ? process_limit - children_.size()
                                 : 0U;
    const auto handle_slots =
        (handle_budget_ * 7U / 10U - 1U - sampled_handles_) / 128U;
    return idle + static_cast<std::size_t>(std::min(spawn_slots, handle_slots));
  }

  bool CanSpawn() const {
    return !draining_ && children_.size() < max_rooms_ &&
           cpu_per_mille_ < 700U &&
           (children_.size() + 1U) * memory_limit_ <=
               memory_budget_ * 7U / 10U &&
           sampled_handles_ + 128U < handle_budget_ * 7U / 10U;
  }

  void Spawn() {
    auto process = ChildProcess::Launch(
        room_executable_, {"--mode", "room", "--config", config_path_.string()},
        memory_limit_);
    const auto pid = process->Metrics().process_id;
    children_.emplace(pid, Child{std::move(process), Clock::now()});
    ++launched_total_;
  }

  void Sample() {
    if (stopped_) return;
    if (!drain_marker_.empty()) {
      std::error_code error;
      if (std::filesystem::exists(drain_marker_, error)) draining_ = true;
      if (error) draining_ = true;
    }
    sampled_memory_ = 0U;
    sampled_handles_ = 0U;
    std::size_t idle = 0U;
    for (auto iterator = children_.begin(); iterator != children_.end();) {
      auto& child = iterator->second;
      const auto metrics = child.process->Metrics();
      if (metrics.exit_code) {
        ++exited_total_;
        if (!child.port) ++startup_failures_total_;
        iterator = children_.erase(iterator);
        continue;
      }
      if (child.port == 0U &&
          Clock::now() - child.launched > std::chrono::seconds{10}) {
        ++startup_timeouts_total_;
        iterator = children_.erase(iterator);
        continue;
      }
      if (metrics.cpu_microseconds >= child.previous_cpu)
        sampled_cpu_delta_ += metrics.cpu_microseconds - child.previous_cpu;
      child.previous_cpu = metrics.cpu_microseconds;
      sampled_memory_ += metrics.private_bytes;
      sampled_handles_ += metrics.handles;
      if (!child.port) {
        child.startup += child.process->ReadOutput();
        if (child.startup.size() > 4096U) {
          ++startup_failures_total_;
          iterator = children_.erase(iterator);
          continue;
        }
        if (const auto newline = child.startup.find('\n');
            newline != std::string::npos) {
          try {
            const auto ready = Json::parse(child.startup.substr(0U, newline));
            const auto port = ready.at("port").get<unsigned>();
            if (!ready.at("ready").get<bool>() || port == 0U || port > 65535U)
              throw std::runtime_error{"invalid child readiness"};
            child.port = static_cast<std::uint16_t>(port);
            child.startup.clear();
          } catch (...) {
            ++startup_failures_total_;
            iterator = children_.erase(iterator);
            continue;
          }
        }
      }
      if (!child.identity) ++idle;
      ++iterator;
    }
    const auto now = Clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(now - sampled_at_)
            .count();
    if (elapsed >= 1'000'000) {
      const auto cores = std::max(1U, std::thread::hardware_concurrency());
      cpu_per_mille_ = sampled_cpu_delta_ * 1000U /
                       (static_cast<std::uint64_t>(elapsed) * cores);
      sampled_at_ = now;
      sampled_cpu_delta_ = 0U;
    }
    while (idle < prewarm_ && CanSpawn()) {
      try {
        Spawn();
        ++idle;
      } catch (const std::exception&) {
        draining_ = true;
        break;
      }
    }
    timer_.expires_after(std::chrono::milliseconds{100});
    timer_.async_wait([this](auto error) {
      if (!error) Sample();
    });
  }

  asio::io_context& io_;
  Json config_;
  std::filesystem::path config_path_;
  std::filesystem::path room_executable_;
  std::filesystem::path drain_marker_;
  std::string node_id_;
  asio::ssl::context tls_{asio::ssl::context::tls_server};
  asio::ip::tcp::acceptor acceptor_;
  asio::steady_timer timer_;
  std::map<std::uint64_t, Child> children_;
  std::vector<std::shared_ptr<RoomTlsChannel>> channels_;
  std::size_t max_rooms_{0U};
  std::size_t prewarm_{0U};
  std::uint64_t memory_limit_{0U};
  std::uint64_t memory_budget_{0U};
  std::uint64_t handle_budget_{0U};
  std::uint64_t sampled_memory_{0U};
  std::uint64_t sampled_handles_{0U};
  std::uint64_t sampled_cpu_delta_{0U};
  std::uint64_t cpu_per_mille_{0U};
  std::uint64_t launched_total_{0U}, exited_total_{0U};
  std::uint64_t startup_failures_total_{0U}, startup_timeouts_total_{0U};
  Clock::time_point sampled_at_{Clock::now()};
  bool draining_{false};
  bool stopped_{false};
};

DsAgent::DsAgent(asio::io_context& io,
                 const std::filesystem::path& config_path) {
  const auto path = std::filesystem::absolute(config_path);
  if (std::filesystem::file_size(path) > 65536U)
    throw std::invalid_argument{"node config size exceeds limit"};
  std::ifstream input{path, std::ios::binary};
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input},
                                        {}};
  auto config = bf::infrastructure::network::ParseRoomPayload(bytes);
  for (const auto* key :
       {"certificate", "private_key", "authority", "room_executable"})
    config[key] = std::filesystem::absolute(path.parent_path() /
                                            config.at(key).get<std::string>())
                      .string();
  impl_ = std::make_unique<Impl>(io, config, path);
}

DsAgent::~DsAgent() = default;

std::uint16_t DsAgent::Port() const { return impl_->Port(); }

void DsAgent::Start() { impl_->Start(); }

void DsAgent::Stop() { impl_->Stop(); }
}  // namespace bf::infrastructure::process
