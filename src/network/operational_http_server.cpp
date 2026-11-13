#include "network/operational_http_server.h"

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace bf::infrastructure::network {
namespace {

using Tcp = boost::asio::ip::tcp;

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

/** @brief 显式消费仅通过 error_code 报错的 Asio 操作返回值。 */
template <typename Result>
void ObserveIoResult(Result&& /*result*/) noexcept {}

std::string Response(int status, std::string_view reason,
                     std::string_view content_type, const std::string& body) {
  std::ostringstream stream;
  stream << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n"
         << body;
  return stream.str();
}

std::string ReadinessResponse(const OperationalSnapshot& snapshot) {
  const auto body = snapshot.ready
                        ? std::string{"{\"status\":\"ready\"}\n"}
                        : std::string{"{\"status\":\"not_ready\"}\n"};
  return Response(snapshot.ready ? 200 : 503,
                  snapshot.ready ? "OK" : "Service Unavailable",
                  "application/json; charset=utf-8", body);
}

void Metric(std::ostringstream& stream, std::string_view name,
            std::uint64_t value) {
  stream << name << ' ' << value << '\n';
}

std::string MetricsResponse(const OperationalSnapshot& snapshot) {
  std::ostringstream body;
  Metric(body, "bf_service_ready", snapshot.ready ? 1U : 0U);
  Metric(body, "bf_gateway_connections", snapshot.gateway_connections);
  Metric(body, "bf_clan_connections", snapshot.clan_connections);
  Metric(body, "bf_database_pool_size", snapshot.database_pool_size);
  Metric(body, "bf_database_pool_available", snapshot.database_pool_available);
  Metric(body, "bf_database_tasks_queued", snapshot.database_tasks_queued);
  Metric(body, "bf_world_tasks_queued", snapshot.world_tasks_queued);
  Metric(body, "bf_ds_healthy_nodes", snapshot.ds_healthy_nodes);
  Metric(body, "bf_ds_queued_commands", snapshot.ds_queued_commands);
  Metric(body, "bf_ds_queued_bytes", snapshot.ds_queued_bytes);
  Metric(body, "bf_ds_rejected_commands_total", snapshot.ds_rejected_commands);
  Metric(body, "bf_ds_failed_rooms_total", snapshot.ds_failed_rooms);
  Metric(body, "bf_ds_command_queue_samples_total", snapshot.ds_queue_samples);
  Metric(body, "bf_ds_command_queue_p99_us", snapshot.ds_queue_p99_us);
  Metric(body, "bf_ds_immediate_rpc_p99_us", snapshot.ds_immediate_rpc_p99_us);
  Metric(body, "bf_ds_worst_room_timer_p99_us",
         snapshot.ds_worst_room_timer_p99_us);
  Metric(body, "bf_ds_draining", snapshot.ds_draining ? 1U : 0U);
  Metric(body, "bf_active_rooms", snapshot.active_rooms);
  Metric(body, "bf_plaza_players", snapshot.plaza_players);
  Metric(body, "bf_world_timers", snapshot.world_timers);
  Metric(body, "bf_pending_match_starts", snapshot.pending_match_starts);
  Metric(body, "bf_pending_match_settlements",
         snapshot.pending_match_settlements);
  Metric(body, "bf_pending_match_compensations",
         snapshot.pending_match_compensations);
  Metric(body, "bf_native_movement_coalesced_total",
         snapshot.movement_coalesced_total);
  Metric(body, "bf_native_movement_dropped_total",
         snapshot.movement_dropped_total);
  Metric(body, "bf_settlements_accepted_total", snapshot.settlements_accepted);
  Metric(body, "bf_settlements_rejected_overload_total",
         snapshot.settlements_rejected_overload);
  Metric(body, "bf_settlements_completed_total",
         snapshot.settlements_completed);
  Metric(body, "bf_settlements_failed_total", snapshot.settlements_failed);
  Metric(body, "bf_settlements_queued", snapshot.settlements_queued);
  Metric(body, "bf_settlements_active", snapshot.settlements_active);
  return Response(200, "OK", "text/plain; version=0.0.4; charset=utf-8",
                  body.str());
}

}  // namespace

class OperationalHttpServer::Impl final {
 public:
  Impl(OperationalHttpServerOptions options,
       OperationalSnapshotProvider snapshot_provider,
       application::EventLogger& logger)
      : options_(std::move(options)),
        snapshot_provider_(std::move(snapshot_provider)),
        logger_(logger),
        acceptor_(io_) {}

  ~Impl() { Stop(std::chrono::milliseconds::zero()); }

  std::optional<application::ServiceError> Start() {
    std::scoped_lock lock{lifecycle_mutex_};
    if (started_) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "运维 HTTP 端点只能启动一次");
    }
    if (options_.bind_address.empty() || options_.maximum_connections == 0U ||
        options_.maximum_connections > 1'024U ||
        options_.maximum_request_bytes < 64U ||
        options_.maximum_request_bytes > 65'536U || !snapshot_provider_) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "运维 HTTP 端点配置无效");
    }

    boost::system::error_code error;
    const auto address =
        boost::asio::ip::make_address(options_.bind_address, error);
    if (error) {
      return MakeError(application::ErrorCode::kInvalidInput,
                       "运维 HTTP 监听地址无效");
    }
    const Tcp::endpoint endpoint{address, options_.port};
    ObserveIoResult(acceptor_.open(endpoint.protocol(), error));
    if (!error) {
      ObserveIoResult(
          acceptor_.set_option(Tcp::acceptor::reuse_address{true}, error));
    }
    if (!error) {
      ObserveIoResult(acceptor_.bind(endpoint, error));
    }
    if (!error) {
      ObserveIoResult(acceptor_.listen(
          boost::asio::socket_base::max_listen_connections, error));
    }
    if (error) {
      boost::system::error_code ignored;
      ObserveIoResult(acceptor_.close(ignored));
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "无法绑定运维 HTTP 监听端口", true);
    }
    local_port_.store(acceptor_.local_endpoint().port(),
                      std::memory_order_release);
    work_guard_.emplace(boost::asio::make_work_guard(io_));
    stopping_.store(false, std::memory_order_release);
    started_ = true;
    DoAccept();
    try {
      worker_ = std::thread{[this] { RunLoop(); }};
    } catch (...) {
      boost::system::error_code ignored;
      ObserveIoResult(acceptor_.close(ignored));
      work_guard_.reset();
      started_ = false;
      local_port_.store(0U, std::memory_order_release);
      return MakeError(application::ErrorCode::kStorageUnavailable,
                       "无法创建运维 HTTP I/O 线程", true);
    }
    logger_.Log(
        application::LogLevel::kDebug,
        "operational HTTP endpoint started on port=" +
            std::to_string(local_port_.load(std::memory_order_acquire)));
    return std::nullopt;
  }

  void Stop(std::chrono::milliseconds /*grace*/) noexcept {
    try {
      StopImpl();
    } catch (...) {
      ForceStopAfterException();
    }
  }

  [[nodiscard]] std::uint16_t LocalPort() const noexcept {
    return local_port_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t ActiveSessionCount() const noexcept {
    return active_sessions_.load(std::memory_order_acquire);
  }

 private:
  class Session final : public std::enable_shared_from_this<Session> {
   public:
    Session(std::uint64_t id, Tcp::socket socket,
            std::size_t maximum_request_bytes, Impl& owner)
        : id_(id),
          socket_(std::move(socket)),
          request_(maximum_request_bytes),
          owner_(owner) {}

    void Start() {
      auto self = shared_from_this();
      boost::asio::async_read_until(
          socket_, request_, "\r\n\r\n",
          [self](const boost::system::error_code& error, std::size_t bytes) {
            self->OnRead(error, bytes);
          });
    }

    void Stop() noexcept {
      boost::system::error_code ignored;
      ObserveIoResult(socket_.cancel(ignored));
      ObserveIoResult(socket_.shutdown(Tcp::socket::shutdown_both, ignored));
      ObserveIoResult(socket_.close(ignored));
    }

   private:
    void OnRead(const boost::system::error_code& error, std::size_t bytes) {
      if (error || bytes == 0U) {
        Finish();
        return;
      }
      const auto data = request_.data();
      const std::string text{boost::asio::buffers_begin(data),
                             boost::asio::buffers_begin(data) +
                                 static_cast<std::ptrdiff_t>(bytes)};
      response_ = owner_.HandleRequest(text);
      auto self = shared_from_this();
      boost::asio::async_write(socket_, boost::asio::buffer(response_),
                               [self](const boost::system::error_code&,
                                      std::size_t) { self->Finish(); });
    }

    void Finish() noexcept {
      Stop();
      owner_.Remove(id_);
    }

    std::uint64_t id_;
    Tcp::socket socket_;
    boost::asio::streambuf request_;
    Impl& owner_;
    std::string response_;
  };

  void RunLoop() noexcept {
    try {
      io_.run();
    } catch (const std::exception&) {
      logger_.Log(application::LogLevel::kCritical,
                  "operational HTTP I/O thread escaped an exception boundary");
    } catch (...) {
      logger_.Log(application::LogLevel::kCritical,
                  "operational HTTP I/O thread escaped an unknown exception");
    }
  }

  /** @brief 执行可能抛异常的正常停机步骤；由 noexcept 外壳统一兜底。 */
  void StopImpl() {
    {
      std::scoped_lock lock{lifecycle_mutex_};
      if (!started_) {
        return;
      }
      started_ = false;
      stopping_.store(true, std::memory_order_release);
    }
    try {
      boost::asio::post(io_, [this] {
        boost::system::error_code ignored;
        ObserveIoResult(acceptor_.cancel(ignored));
        ObserveIoResult(acceptor_.close(ignored));
        for (auto& [id, session] : sessions_) {
          static_cast<void>(id);
          session->Stop();
        }
        sessions_.clear();
        active_sessions_.store(0U, std::memory_order_release);
        work_guard_.reset();
      });
    } catch (...) {
      io_.stop();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    local_port_.store(0U, std::memory_order_release);
    logger_.Log(application::LogLevel::kDebug,
                "operational HTTP endpoint stopped");
  }

  /** @brief 正常停机异常时强制停止 I/O；join 再失败表示违反非 I/O 调用契约。 */
  void ForceStopAfterException() noexcept {
    try {
      io_.stop();
      if (worker_.joinable()) {
        worker_.join();
      }
    } catch (...) {
      std::terminate();
    }
  }

  void DoAccept() {
    acceptor_.async_accept([this](const boost::system::error_code& error,
                                  Tcp::socket socket) {
      if (!error && !stopping_.load(std::memory_order_acquire)) {
        if (sessions_.size() >= options_.maximum_connections) {
          boost::system::error_code ignored;
          ObserveIoResult(socket.close(ignored));
        } else {
          const auto id = next_session_id_++;
          auto session = std::make_shared<Session>(
              id, std::move(socket), options_.maximum_request_bytes, *this);
          sessions_.emplace(id, session);
          active_sessions_.store(sessions_.size(), std::memory_order_release);
          session->Start();
        }
      }
      if (!stopping_.load(std::memory_order_acquire) && acceptor_.is_open()) {
        DoAccept();
      }
    });
  }

  [[nodiscard]] std::string HandleRequest(std::string_view request) const {
    const auto line_end = request.find("\r\n");
    const auto line = request.substr(0U, line_end);
    const auto first_space = line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos ||
        line.substr(0U, first_space) != "GET" ||
        !(line.substr(second_space + 1U) == "HTTP/1.0" ||
          line.substr(second_space + 1U) == "HTTP/1.1")) {
      return Response(400, "Bad Request", "application/json; charset=utf-8",
                      "{\"error\":\"bad_request\"}\n");
    }
    const auto target =
        line.substr(first_space + 1U, second_space - first_space - 1U);
    if (target == "/live") {
      return Response(200, "OK", "application/json; charset=utf-8",
                      "{\"status\":\"live\"}\n");
    }
    if (target != "/ready" && target != "/metrics") {
      return Response(404, "Not Found", "application/json; charset=utf-8",
                      "{\"error\":\"not_found\"}\n");
    }
    try {
      const auto snapshot = snapshot_provider_();
      return target == "/ready" ? ReadinessResponse(snapshot)
                                : MetricsResponse(snapshot);
    } catch (...) {
      return Response(500, "Internal Server Error",
                      "application/json; charset=utf-8",
                      "{\"error\":\"snapshot_failed\"}\n");
    }
  }

  void Remove(std::uint64_t id) noexcept {
    sessions_.erase(id);
    active_sessions_.store(sessions_.size(), std::memory_order_release);
  }

  OperationalHttpServerOptions options_;
  OperationalSnapshotProvider snapshot_provider_;
  application::EventLogger& logger_;
  boost::asio::io_context io_;
  Tcp::acceptor acceptor_;
  std::optional<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_guard_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::mutex lifecycle_mutex_;
  std::thread worker_;
  std::atomic<std::uint16_t> local_port_{0U};
  std::atomic<std::size_t> active_sessions_{0U};
  std::atomic<bool> stopping_{false};
  std::uint64_t next_session_id_{1U};
  bool started_{false};
};

OperationalHttpServer::OperationalHttpServer(
    OperationalHttpServerOptions options,
    OperationalSnapshotProvider snapshot_provider,
    application::EventLogger& logger)
    : impl_(std::make_unique<Impl>(std::move(options),
                                   std::move(snapshot_provider), logger)) {}

OperationalHttpServer::~OperationalHttpServer() = default;

std::optional<application::ServiceError> OperationalHttpServer::Start() {
  return impl_->Start();
}

void OperationalHttpServer::Stop(std::chrono::milliseconds grace) noexcept {
  impl_->Stop(grace);
}

std::uint16_t OperationalHttpServer::LocalPort() const noexcept {
  return impl_->LocalPort();
}

std::size_t OperationalHttpServer::ActiveSessionCount() const noexcept {
  return impl_->ActiveSessionCount();
}

}  // namespace bf::infrastructure::network
