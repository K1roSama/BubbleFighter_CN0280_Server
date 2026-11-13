#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <thread>

#include "application/entry_points.h"
#include "crypto/openssl_credential_crypto.h"
#include "crypto/openssl_sha256.h"
#include "features/configuration/server_config.h"
#include "network/launcher_authentication_handler.h"
#include "network/operational_http_server.h"
#include "operations/async_file_logger.h"
#include "operations/database_task_pool.h"
#include "operations/system_utc_clock.h"
#include "operations/utf8_path.h"
#include "persistence/mysql/launcher_authentication_repository.h"
#include "persistence/mysql/schema_verifier.h"

namespace {
using bf::application::ServiceError;
using bf::infrastructure::configuration::ServerConfig;
volatile std::sig_atomic_t stop_signal = 0;

extern "C" void HandleLoginSignal(int value) { stop_signal = value; }

struct CommandLine final {
  std::filesystem::path config_path;
  bool check_config_only{false};
  bool check_database_only{false};
  std::optional<std::uint32_t> run_for_ms;
};

void PrintUsage() {
  std::cerr
      << "Usage: BubbleFighter.Server.exe --mode login --config <path> [--check-config] [--check-database] "
         "[--run-for-ms <ms>]\n";
}

std::variant<CommandLine, ServiceError> ParseCommandLine(int argc,
                                                         char** argv) {
  CommandLine result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--config" && index + 1 < argc) {
      result.config_path = bf::infrastructure::Utf8Path(argv[++index]);
    } else if (argument == "--check-config") {
      result.check_config_only = true;
    } else if (argument == "--check-database") {
      result.check_database_only = true;
    } else if (argument == "--run-for-ms" && index + 1 < argc) {
      const std::string_view value{argv[++index]};
      std::uint32_t duration = 0;
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), duration);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != value.data() + value.size() || duration > 3600000U) {
        return ServiceError{
            .code = bf::application::ErrorCode::kInvalidInput,
            .safe_message =
                "--run-for-ms must be an integer between 0 and 3600000",
            .correlation_id = {},
            .is_retryable = false,
        };
      }
      result.run_for_ms = duration;
    } else {
      return ServiceError{
          .code = bf::application::ErrorCode::kInvalidInput,
          .safe_message = "Unknown command-line option or missing option value",
          .correlation_id = {},
          .is_retryable = false,
      };
    }
  }
  if (result.config_path.empty()) {
    return ServiceError{
        .code = bf::application::ErrorCode::kInvalidInput,
        .safe_message = "Specify a configuration file with --config",
        .correlation_id = {},
        .is_retryable = false,
    };
  }
  if (result.check_config_only && result.check_database_only) {
    return ServiceError{
        .code = bf::application::ErrorCode::kInvalidInput,
        .safe_message =
            "--check-config and --check-database are mutually exclusive",
        .correlation_id = {},
        .is_retryable = false,
    };
  }
  return result;
}

bf::infrastructure::logging::AsyncLoggerOptions LoggerOptions(
    const ServerConfig& config) {
  return bf::infrastructure::logging::AsyncLoggerOptions{
      .path = config.logging.path,
      .minimum_level = config.logging.minimum_level,
      .queue_capacity = config.logging.queue_capacity,
      .rotate_bytes = config.logging.rotate_bytes,
      .retained_files = config.logging.retained_files,
      .echo_console = config.logging.echo_console,
  };
}

int RunLogin(const CommandLine& command_line, const ServerConfig& config) {
  bf::infrastructure::logging::AsyncFileLogger logger(LoggerOptions(config));
  const auto report_error = [&](std::string_view message,
                                std::string_view correlation_id = {}) {
    logger.Log(bf::application::LogLevel::kError, message, correlation_id);
    if (!config.logging.echo_console)
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, message, correlation_id);
  };
  if (!command_line.check_database_only)
    logger.Log(bf::application::LogLevel::kInfo,
               "[Server] Starting the standalone login service.");
  const auto schema = bf::infrastructure::mysql::VerifySchema(
      config.database.connection, config.database.expected_schema_version,
      config.database.connection.query_timeout_ms);
  if (const auto* error = std::get_if<ServiceError>(&schema)) {
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  if (command_line.check_database_only) {
    std::cout << "login database valid\n";
    return 0;
  }
  logger.Log(
      bf::application::LogLevel::kInfo,
      "[Database] Login database connection and schema validation completed.");
  auto pool_result = bf::infrastructure::mysql::MySqlConnectionPool::Create(
      {.connection = config.database.connection,
       .size = config.database.pool_size,
       .acquire_timeout =
           std::chrono::milliseconds{config.database.acquire_timeout_ms},
       .query_timeout_ms = config.database.connection.query_timeout_ms});
  if (const auto* error = std::get_if<ServiceError>(&pool_result)) {
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  auto pool =
      std::get<std::unique_ptr<bf::infrastructure::mysql::MySqlConnectionPool>>(
          std::move(pool_result));
  bf::infrastructure::mysql::DatabaseTaskPool tasks{
      config.database.pool_size, config.database.task_queue_capacity, logger};
  bf::infrastructure::system::SystemUtcClock clock;
  bf::infrastructure::security::OpenSslCredentialCrypto crypto;
  bf::infrastructure::security::OpenSslSha256 sha;
  bf::infrastructure::mysql::MySqlRegisterAccountUnitOfWork registration_uow{
      *pool};
  bf::application::RegisterAccountService registration{crypto, crypto,
                                                       registration_uow};
  bf::infrastructure::mysql::MySqlAccountCredentialReader credentials{*pool};
  bf::infrastructure::mysql::MySqlIssueLoginTicketUnitOfWork tickets{
      *pool, config.service.region_id};
  bf::application::LoginWithPasswordService login{crypto, crypto,      sha,
                                                  clock,  credentials, tickets};

  // 异常退出时先销毁网络对象，再排空任务，最后销毁任务借用的认证服务。
  struct TaskDrainGuard final {
    bf::infrastructure::mysql::DatabaseTaskPool& tasks;

    ~TaskDrainGuard() { tasks.DrainAndStop(); }
  } task_drain{tasks};

  bf::runtime::SessionRegistry sessions{config.network.maximum_connections};
  bf::infrastructure::network::LoginSessionHandlerFactory handlers{
      tasks,
      registration,
      login,
      {.region_id = config.service.region_id,
       .host = config.service.game_address,
       .port = config.service.game_port,
       .clan_port = config.service.game_clan_port}};
  bf::infrastructure::network::TcpServer tcp{
      {.bind_address = config.network.bind_address,
       .port = config.network.port,
       .io_threads = config.network.io_threads,
       .maximum_connections = config.network.maximum_connections,
       .protocol_limits = config.network.protocol_limits},
      sessions,
      handlers,
      logger};
  std::atomic<bool> ready{false};
  bf::infrastructure::network::OperationalHttpServer operations{
      {.bind_address = config.operations.bind_address,
       .port = config.operations.port,
       .maximum_connections = config.operations.maximum_connections,
       .maximum_request_bytes = config.operations.maximum_request_bytes},
      [&] {
        return bf::infrastructure::network::OperationalSnapshot{
            .ready = ready.load(),
            .gateway_connections = tcp.ActiveSessionCount(),
            .database_pool_size = pool->Size(),
            .database_pool_available = pool->Available(),
            .database_tasks_queued = tasks.QueuedTaskCount()};
      },
      logger};
  if (const auto error = tcp.Start()) {
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  if (const auto error = operations.Start()) {
    tcp.Stop(std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    tasks.DrainAndStop();
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  ready.store(true);
  logger.Log(bf::application::LogLevel::kInfo,
             "[Network] Login service ready; listening=" +
                 config.network.bind_address + ":" +
                 std::to_string(tcp.LocalPort()) +
                 "; region=" + std::to_string(config.service.region_id));
  logger.Log(bf::application::LogLevel::kInfo,
             "[Operations] HTTP service ready; listening=" +
                 config.operations.bind_address + ":" +
                 std::to_string(operations.LocalPort()));
  logger.Log(
      bf::application::LogLevel::kInfo,
      "[Server] Login service startup completed. Accepting client connections.");
  const auto began = std::chrono::steady_clock::now();
  while (stop_signal == 0) {
    if (command_line.run_for_ms &&
        std::chrono::steady_clock::now() - began >=
            std::chrono::milliseconds{*command_line.run_for_ms})
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  logger.Log(bf::application::LogLevel::kInfo,
             "[Server] Shutting down the login service gracefully.");
  ready.store(false);
  operations.Stop(std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  tcp.Stop(std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  tasks.DrainAndStop();
  pool->Stop();
  logger.Log(bf::application::LogLevel::kInfo,
             "[Server] Login service stopped.");
  logger.Shutdown();
  return 0;
}
}  // namespace

int bf::entry::RunLogin(int argc, char** argv) {
  try {
    const auto parsed = ParseCommandLine(argc, argv);
    if (const auto* error = std::get_if<ServiceError>(&parsed)) {
      PrintUsage();
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, error->safe_message,
          error->correlation_id);
      return 2;
    }
    const auto& command = std::get<CommandLine>(parsed);
    const auto loaded = bf::infrastructure::configuration::LoadServerConfig(
        command.config_path);
    if (const auto* error = std::get_if<ServiceError>(&loaded)) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, error->safe_message,
          error->correlation_id);
      return 2;
    }
    const auto& config = std::get<ServerConfig>(loaded);
    if (config.service.role != "login") {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError,
          "[Configuration] Standalone login mode requires service.role=login.");
      return 2;
    }
    if (command.check_config_only) {
      std::cout << "login configuration valid\n";
      return 0;
    }
    std::signal(SIGINT, HandleLoginSignal);
    std::signal(SIGTERM, HandleLoginSignal);
#ifdef _WIN32
    std::signal(SIGBREAK, HandleLoginSignal);
#endif
    return RunLogin(command, config);
  } catch (...) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Startup] Login service startup failed. Check the configuration and server logs.");
    return 1;
  }
}
