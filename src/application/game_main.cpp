#include "application/entry_points.h"
#include "application/error.h"
#include "features/configuration/server_config.h"
#include "operations/async_file_logger.h"
#include "operations/utf8_path.h"
#include "persistence/mysql/couple_repository.h"
#include "persistence/mysql/social_spaces_repository.h"
#include "sessions/couple_flow.h"
#include "sessions/service_runtime.h"

#ifdef BF_EXTERNAL_DEPENDENCIES
#include "crypto/openssl_credential_crypto.h"
#include "crypto/openssl_match_id_source.h"
#include "crypto/openssl_operation_id_source.h"
#include "crypto/openssl_sha256.h"
#include "crypto/openssl_treasure_random_source.h"
#include "crypto/openssl_workshop_random_source.h"
#include "features/authentication/authentication.h"
#include "features/authentication/launcher_authentication.h"
#include "features/authentication/world_access.h"
#include "features/battle/durable_match_settlement_flow.h"
#include "features/battle/match_lifecycle_repair.h"
#include "features/battle/match_preparation.h"
#include "features/battle/match_settlement.h"
#include "features/battle/match_settlement_dispatcher.h"
#include "features/battle/match_start.h"
#include "features/characters/character_creation.h"
#include "features/clans/clan_service.h"
#include "features/configuration/service_rules.h"
#include "features/inventory/equipment_service.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/scenes/hide_seek_scene_assets.h"
#include "features/shop/mystery_shop_service.h"
#include "features/shop/purchase_service.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/treasure_service.h"
#include "features/shop/workshop_service.h"
#include "features/social/social_query.h"
#include "features/social/social_service.h"
#include "network/clan_tcp_server.h"
#include "network/equipment_relay_handler.h"
#include "network/gateway_session_handler.h"
#include "network/ice_network_service.h"
#include "network/launcher_authentication_handler.h"
#include "network/operational_http_server.h"
#include "network/tcp_server.h"
#include "operations/database_task_pool.h"
#include "operations/system_utc_clock.h"
#include "persistence/file_life_attendance_journal.h"
#include "persistence/mysql/admin_maintenance.h"
#include "persistence/mysql/character_creation_repository.h"
#include "persistence/mysql/chat_block_store.h"
#include "persistence/mysql/clan_repository.h"
#include "persistence/mysql/connection_pool.h"
#include "persistence/mysql/equipment_repository.h"
#include "persistence/mysql/gift_repository.h"
#include "persistence/mysql/item_use_repository.h"
#include "persistence/mysql/key_map_store.h"
#include "persistence/mysql/launcher_authentication_repository.h"
#include "persistence/mysql/level_icon_store.h"
#include "persistence/mysql/life_attendance_repository.h"
#include "persistence/mysql/lobby_minigame_store.h"
#include "persistence/mysql/login_ticket_repository.h"
#include "persistence/mysql/match_preparation_repository.h"
#include "persistence/mysql/match_repository.h"
#include "persistence/mysql/mystery_shop_repository.h"
#include "persistence/mysql/native_title_repository.h"
#include "persistence/mysql/player_menu_repository.h"
#include "persistence/mysql/purchase_repository.h"
#include "persistence/mysql/schema_verifier.h"
#include "persistence/mysql/service_rules_repository.h"
#include "persistence/mysql/shop_catalog_repository.h"
#include "persistence/mysql/social_repository.h"
#include "persistence/mysql/treasure_repository.h"
#include "persistence/mysql/tutorial_session_repository.h"
#include "persistence/mysql/workshop_repository.h"
#include "persistence/mysql/world_access_repository.h"
#include "persistence/mysql/world_process_guard.h"
#include "sessions/clan_ticket_broker.h"
#include "sessions/session_registry.h"
#include "sessions/world_executor.h"
#ifdef BF_HAS_ROOM_DS
#include "network/room_gateway.h"
#include "persistence/mysql/room_lease_repository.h"
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace {

using bf::application::ServiceError;
using bf::infrastructure::configuration::ServerConfig;

volatile std::sig_atomic_t stop_signal = 0;

extern "C" void HandleGameSignal(int signal_number) {
  stop_signal = signal_number;
}

struct CommandLine final {
  std::filesystem::path config_path;
  std::filesystem::path login_config_path;
  std::filesystem::path room_ds_config_path;
  bool check_config_only{false};
  bool check_database_only{false};
  std::optional<std::uint32_t> run_for_ms;
};

void PrintUsage() {
  std::cerr
      << "Usage: BubbleFighter.Server.exe [--mode combined|game] --config <path> [--check-config] [--check-database] "
         "[--run-for-ms <ms>] [--login-config <path>] [--room-ds-config <private-json>]\n";
}

std::variant<CommandLine, ServiceError> ParseCommandLine(int argc,
                                                         char** argv) {
  CommandLine result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--config" && index + 1 < argc) {
      result.config_path = bf::infrastructure::Utf8Path(argv[++index]);
    } else if (argument == "--login-config" && index + 1 < argc) {
      result.login_config_path = bf::infrastructure::Utf8Path(argv[++index]);
    } else if (argument == "--room-ds-config" && index + 1 < argc) {
      result.room_ds_config_path = bf::infrastructure::Utf8Path(argv[++index]);
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

#ifdef BF_EXTERNAL_DEPENDENCIES

/**
 * @brief 把结算完成状态写入脱敏运维日志；失败命令仍由 MySQL pending 日志持有。
 * @note 本 sink
 * 不重试数据库事务，避免工作线程内形成无界循环；启动恢复会重新投递未成功记录。
 */
class DurableSettlementCompletionLogger final
    : public bf::runtime::MatchSettlementCompletionSink {
 public:
  /** @brief 注入生命周期覆盖调度器的日志端口。 */
  explicit DurableSettlementCompletionLogger(
      bf::application::EventLogger& logger) noexcept
      : logger_(logger) {}

  /** @brief 在绑定守卫存活期内允许把成功回执移交世界执行器。 */
  void BindWorld(bf::runtime::WorldExecutor* world) {
    std::scoped_lock lock{mutex_};
    world_ = world;
  }

  /** @brief 记录成功，或记录仍由耐久日志拥有的失败分类。 */
  void Publish(
      bf::runtime::MatchSettlementCompletion completion) noexcept override {
    if (const auto* error = std::get_if<ServiceError>(&completion.result)) {
      logger_.Log(
          bf::application::LogLevel::kError,
          "match settlement failed and remains durable for recovery: match=" +
              completion.command.match_id.ToUuidString() + " code=" +
              std::string{bf::application::ErrorCodeName(error->code)});
      return;
    }
    logger_.Log(bf::application::LogLevel::kInfo,
                "match settlement committed: match=" +
                    completion.command.match_id.ToUuidString());
    try {
      std::scoped_lock lock{mutex_};
      if (world_ != nullptr &&
          !world_->PostSettlementResult(
              std::get<bf::application::MatchSettlementResult>(
                  std::move(completion.result)))) {
        logger_.Log(
            bf::application::LogLevel::kWarning,
            "committed reward display skipped while world executor is stopping");
      }
    } catch (...) {
      logger_.Log(bf::application::LogLevel::kError,
                  "committed reward display handoff failed");
    }
  }

 private:
  bf::application::EventLogger&
      logger_;        ///< 脱敏结构化日志端口，不由本对象拥有。
  std::mutex mutex_;  ///< 绑定和投递互斥，防止关闭路径访问已销毁的世界执行器。
  bf::runtime::WorldExecutor* world_{
      nullptr};  ///< 只在下方作用域守卫存活期借用。
};

/** @brief 保证所有正常及错误退出路径先解除回执绑定，再销毁世界执行器。 */
class SettlementWorldBinding final {
 public:
  /** @brief 绑定生命周期更长的完成端口与当前世界执行器。 */
  SettlementWorldBinding(DurableSettlementCompletionLogger& sink,
                         bf::runtime::WorldExecutor& world)
      : sink_(sink) {
    sink_.BindWorld(&world);
  }

  /** @brief 在世界执行器析构前同步解除所有数据库线程的借用。 */
  ~SettlementWorldBinding() { sink_.BindWorld(nullptr); }

  SettlementWorldBinding(const SettlementWorldBinding&) = delete;
  SettlementWorldBinding& operator=(const SettlementWorldBinding&) = delete;

 private:
  DurableSettlementCompletionLogger&
      sink_;  ///< 比守卫和世界执行器存活更久的回执端口。
};
#ifdef BF_HAS_ROOM_DS
/** @brief 异常退出时在大厅和数据库工作池销毁前停止独立房间后端。 */
class RoomGatewayBinding final {
 public:
  explicit RoomGatewayBinding(bf::infrastructure::network::RoomGateway* gateway)
      : gateway_(gateway) {}

  ~RoomGatewayBinding() {
    if (gateway_) gateway_->Stop();
  }

 private:
  bf::infrastructure::network::RoomGateway* gateway_;
};
#endif
#endif

int RunServer(const CommandLine& command_line, const ServerConfig& config,
              const ServerConfig* login_config) {
  bf::infrastructure::logging::AsyncFileLogger logger(LoggerOptions(config));
  const auto report_error = [&](std::string_view message,
                                std::string_view correlation_id = {}) {
    logger.Log(bf::application::LogLevel::kError, message, correlation_id);
    if (!config.logging.echo_console)
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, message, correlation_id);
  };
  bf::runtime::ServiceRuntime runtime(logger);
  if (const auto error = runtime.Start()) {
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }

#ifdef BF_EXTERNAL_DEPENDENCIES
  const auto schema_result = bf::infrastructure::mysql::VerifySchema(
      config.database.connection, config.database.expected_schema_version,
      config.database.connection.query_timeout_ms);
  if (const auto* error = std::get_if<ServiceError>(&schema_result)) {
    report_error(
        "[Database] Connection or schema validation failed: " +
            error->safe_message +
            ". Check the database configuration, account permissions and schema initialization.",
        error->correlation_id);
    runtime.RequestStop("schema-verification-failed");
    runtime.CompleteStop();
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  const auto& schema =
      std::get<bf::infrastructure::mysql::SchemaVerificationReport>(
          schema_result);
  logger.Log(bf::application::LogLevel::kInfo,
             "[Database] Connection and schema validation completed; MySQL=" +
                 schema.server_version +
                 "; schema=" + std::to_string(schema.schema_version));
  auto pool_result = bf::infrastructure::mysql::MySqlConnectionPool::Create(
      {.connection = config.database.connection,
       .size = config.database.pool_size,
       .acquire_timeout =
           std::chrono::milliseconds{config.database.acquire_timeout_ms},
       .query_timeout_ms = config.database.connection.query_timeout_ms});
  if (const auto* error = std::get_if<ServiceError>(&pool_result)) {
    runtime.RequestStop("connection-pool-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  auto pool =
      std::get<std::unique_ptr<bf::infrastructure::mysql::MySqlConnectionPool>>(
          std::move(pool_result));
  const auto advertised_ipv4 =
      bf::infrastructure::configuration::AdvertisedIpv4(config.network);
  if (!advertised_ipv4.has_value()) {
    pool->Stop();
    runtime.RequestStop("invalid-advertised-address");
    runtime.CompleteStop();
    report_error(
        "[Configuration] network.advertised_address (or bind_address when omitted) must be a client-accessible numeric unicast IPv4 address.");
    logger.Shutdown();
    return 2;
  }

  auto ownership_result = bf::infrastructure::mysql::WorldProcessGuard::Acquire(
      config.database.connection, config.database.connection.query_timeout_ms);
  if (const auto* error = std::get_if<ServiceError>(&ownership_result)) {
    report_error(error->safe_message, error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  auto ownership =
      std::get<std::unique_ptr<bf::infrastructure::mysql::WorldProcessGuard>>(
          std::move(ownership_result));
  bf::infrastructure::mysql::DatabaseTaskPool database_tasks{
      config.database.pool_size, config.database.task_queue_capacity, logger};
  bf::infrastructure::system::SystemUtcClock clock;
  bf::infrastructure::security::OpenSslSha256 sha256;
  bf::infrastructure::security::OpenSslCredentialCrypto credential_crypto;
  bf::infrastructure::security::OpenSslOperationIdSource operation_ids;
  // 原家族确认文案约定下一次维护。进程归属锁已经取得，所有玩家端口尚未开放；
  // 只消费本人原生新申请，保留旧导入、未到期和失去会长授权的请求供显式复核。
  constexpr std::size_t k_disband_batch_limit = 1000U;
  for (;;) {
    auto operation = operation_ids.Next();
    if (const auto* error = std::get_if<ServiceError>(&operation)) {
      runtime.RequestStop("clan-startup-maintenance-id-failed");
      runtime.CompleteStop();
      report_error(error->safe_message, error->correlation_id);
      return bf::application::ProcessExitCode(*error);
    }
    auto maintenance = bf::infrastructure::mysql::ExecuteDueClanDisbands(
        *pool, {.actor = "game-server-startup",
                .reason = "native clan disband confirmed for next maintenance",
                .business_id =
                    std::get<bf::domain::OperationId>(operation).ToUuidString(),
                .limit = k_disband_batch_limit,
                .native_requests_only = true});
    if (const auto* error = std::get_if<ServiceError>(&maintenance)) {
      runtime.RequestStop("clan-startup-maintenance-failed");
      runtime.CompleteStop();
      report_error(error->safe_message, error->correlation_id);
      return bf::application::ProcessExitCode(*error);
    }
    const auto& completed =
        std::get<bf::infrastructure::mysql::ExecuteClanDisbandsReport>(
            maintenance);
    if (!completed.clan_ids.empty()) {
      logger.Log(
          bf::application::LogLevel::kInfo,
          "[Clan] Startup maintenance completed; pending disband requests have been processed.");
    }
    if (completed.clan_ids.size() < k_disband_batch_limit) {
      break;
    }
  }
  bf::infrastructure::mysql::MySqlRegisterAccountUnitOfWork registration_uow{
      *pool};
  bf::application::RegisterAccountService account_registration{
      credential_crypto, credential_crypto, registration_uow};
  bf::infrastructure::mysql::MySqlAccountCredentialReader credential_reader{
      *pool};
  bf::infrastructure::mysql::MySqlIssueLoginTicketUnitOfWork issue_ticket_uow{
      *pool, config.service.region_id};
  bf::application::LoginWithPasswordService password_login{
      credential_crypto, credential_crypto, sha256, clock,
      credential_reader, issue_ticket_uow};
  bf::infrastructure::mysql::MySqlLoginTicketUnitOfWork login_uow{
      *pool, config.service.region_id};
  bf::application::LoginTicketService login{sha256, clock, login_uow};
  bf::infrastructure::mysql::MySqlDailyLoginRewardGranter daily_rewards{
      *pool, &logger};
  bf::infrastructure::mysql::MySqlLifeAttendanceStore life_store{*pool};
  const auto life_epoch = life_store.JournalEpoch();
  if (const auto* error = std::get_if<ServiceError>(&life_epoch))
    throw std::runtime_error{error->safe_message};
  auto life_directory = config.life_attendance_journal_directory;
  if (life_directory.empty()) life_directory = "life-attendance";
  if (life_directory.is_relative())
    life_directory =
        std::filesystem::absolute(command_line.config_path).parent_path() /
        life_directory;
  auto life_journal_result =
      bf::infrastructure::storage::FileLifeAttendanceJournal::Open(
          life_directory, std::get<std::string>(life_epoch),
          config.service.region_id);
  if (const auto* error = std::get_if<ServiceError>(&life_journal_result))
    throw std::runtime_error{error->safe_message};
  auto life_journal = std::get<
      std::unique_ptr<bf::infrastructure::storage::FileLifeAttendanceJournal>>(
      std::move(life_journal_result));
  bf::runtime::LifeAttendanceTracker life_attendance{life_store, clock, logger,
                                                     life_journal.get()};
  if (const auto error = life_attendance.Recover())
    throw std::runtime_error{error->safe_message};
  bf::infrastructure::mysql::MySqlCharacterCreationUnitOfWork character_uow{
      *pool};
  bf::application::CharacterCreationService characters{character_uow};
  bf::infrastructure::mysql::MySqlPlayerMenuSnapshotReader menu_reader{*pool};
  bf::application::PlayerMenuSnapshotService menus{clock, menu_reader};
  bf::infrastructure::mysql::MySqlShopCatalogReader catalog_reader{*pool};
  bf::application::ShopCatalogService catalog{catalog_reader};
  bf::infrastructure::mysql::MySqlServiceRulesReader service_rules_reader{
      *pool};
  bf::application::ServiceRulesService service_rules{service_rules_reader};
  bf::infrastructure::security::OpenSslTreasureRandomSource treasure_random;
  bf::infrastructure::mysql::MySqlTreasureRepository treasure_repository{*pool};
  bf::application::TreasureService treasure{
      clock, service_rules, treasure_random, treasure_repository};
  bf::infrastructure::mysql::MySqlMysteryShopRepository mystery_repository{
      *pool};
  bf::application::MysteryShopService mystery_shop{clock, service_rules,
                                                   mystery_repository};
  bf::infrastructure::security::OpenSslWorkshopRandomSource workshop_random;
  bf::infrastructure::mysql::MySqlWorkshopRepository workshop_repository{*pool};
  bf::application::WorkshopService workshop{
      clock, service_rules, workshop_random, workshop_repository};
  bf::infrastructure::mysql::MySqlPurchaseUnitOfWork purchase_uow{*pool};
  bf::application::PurchaseService purchases{clock, purchase_uow};
  bf::infrastructure::mysql::MySqlChangeEquipmentUnitOfWork equipment_uow{
      *pool};
  bf::application::ChangeEquipmentService equipment{clock, equipment_uow};
  bf::infrastructure::mysql::MySqlItemUseRepository item_use_repository{*pool};
  bf::application::ItemUseService item_use{clock, item_use_repository};
  bf::infrastructure::mysql::MySqlSocialRepository social_repository{*pool};
  bf::application::SocialQueryService social_queries{social_repository};
  bf::application::FriendRelationshipService social_relationships{
      social_repository};
  bf::application::PrivateMessageService private_messages{social_repository};
  bf::infrastructure::mysql::MySqlWorldAccessReader world_access_reader{*pool};
  bf::application::WorldAccessService world_access{world_access_reader};
  bf::infrastructure::mysql::MySqlMatchPreparationReader
      match_preparation_reader{*pool};
  bf::application::MatchPreparationService match_preparation{
      match_preparation_reader};
  bf::infrastructure::security::OpenSslMatchIdSource match_ids;
  bf::infrastructure::mysql::MySqlMatchStartUnitOfWork match_start_uow{*pool};
  bf::application::MatchStartService match_starts{match_start_uow};
  bf::infrastructure::mysql::MySqlMatchLifecycleRepairRepository
      match_repair_repository{*pool};
  bf::application::MatchLifecycleRepairService match_repairs{
      match_repair_repository};

  // 仅在取得数据库唯一世界所有权后恢复遗留对局；独立登录进程不执行此路径。
  const auto active_match_recovery =
      match_repairs.RecoverAbandoned(clock.Now());
  if (const auto* error = std::get_if<ServiceError>(&active_match_recovery)) {
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("active-match-recovery-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  const auto& active_match_report =
      std::get<bf::application::MatchLifecycleRecoveryReport>(
          active_match_recovery);
  logger.Log(
      bf::application::LogLevel::kInfo,
      "[Recovery] Active match recovery completed; interrupted_matches=" +
          std::to_string(active_match_report.match_count) + "; participants=" +
          std::to_string(active_match_report.participant_count));

  // 在开放监听前验证目录确实已发布且可完整编码，避免首个玩家才发现部署缺件。
  const auto catalog_result = catalog.Load();
  if (const auto* error = std::get_if<ServiceError>(&catalog_result)) {
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("catalog-readiness-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  const auto service_rules_result = service_rules.Load();
  if (const auto* error = std::get_if<ServiceError>(&service_rules_result)) {
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("service-rules-readiness-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  logger.Log(bf::application::LogLevel::kInfo,
             "[Resources] Shop catalog and service rules loaded successfully.");

  bf::infrastructure::mysql::MySqlMatchSettlementJournal settlement_journal{
      *pool};
  bf::infrastructure::mysql::MySqlMatchSettlementUnitOfWork settlement_uow{
      *pool};
  bf::application::MatchSettlementService settlement_service{settlement_uow};
  DurableSettlementCompletionLogger settlement_completions{logger};
  const auto settlement_workers = std::max<std::size_t>(
      1U, std::min<std::size_t>(config.database.pool_size, 2U));
  const auto settlement_queue =
      std::max(settlement_workers, config.database.task_queue_capacity);
  bf::runtime::MatchSettlementDispatcher settlement_dispatcher{
      {.worker_count = settlement_workers, .queue_capacity = settlement_queue},
      settlement_service,
      settlement_completions,
      logger};
  if (const auto error = settlement_dispatcher.Start()) {
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("settlement-dispatcher-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  bf::runtime::DurableMatchSettlementFlow settlement_flow{
      settlement_journal, settlement_dispatcher, logger};
  const auto recovery_limit = std::min<std::size_t>(settlement_queue, 4'096U);
  const auto recovery_result = settlement_flow.Recover(recovery_limit);
  if (const auto* error = std::get_if<ServiceError>(&recovery_result)) {
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("settlement-recovery-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  const auto& recovery =
      std::get<bf::runtime::MatchSettlementRecoveryReport>(recovery_result);
  logger.Log(bf::application::LogLevel::kInfo,
             "[Recovery] Match settlement recovery completed; loaded=" +
                 std::to_string(recovery.loaded) +
                 "; scheduled=" + std::to_string(recovery.scheduled) +
                 "; deferred=" + std::to_string(recovery.deferred));

  bf::runtime::SessionRegistry sessions{config.network.maximum_connections};
  bf::runtime::ClanTicketBroker clan_tickets{
      clock, credential_crypto, sessions, config.network.maximum_connections};
  bf::infrastructure::mysql::MySqlClanRepository clan_repository{*pool};
  bf::application::ClanService clans{clock, clan_repository};
  bf::runtime::EquipmentRelayBroker equipment_relay{
      clock, credential_crypto, sessions, config.network.maximum_connections,
      &logger};
  bf::infrastructure::mysql::MySqlTutorialSessionRepository tutorial_sessions{
      *pool};
  auto ice_config = config.ice;
  if (ice_config.port && ice_config.advertised_address.empty())
    ice_config.advertised_address = config.network.advertised_address.empty()
                                        ? config.network.bind_address
                                        : config.network.advertised_address;
  bf::infrastructure::network::IceNetworkService ice_network{
      std::move(ice_config)};
#ifdef BF_HAS_ROOM_DS
  bf::infrastructure::mysql::MySqlRoomLeaseRepository room_leases{*pool};
  std::unique_ptr<bf::infrastructure::network::RoomGateway> room_gateway;
  if (!command_line.room_ds_config_path.empty()) {
    auto room_options = bf::infrastructure::network::LoadRoomGatewayOptions(
        command_line.room_ds_config_path);
    room_options.region_id = config.service.region_id;
    room_options.equipment_relay = &equipment_relay;
    room_options.equipment_relay_address = *advertised_ipv4;
    room_options.equipment_relay_port = config.network.equipment_relay_port;
    room_gateway = std::make_unique<bf::infrastructure::network::RoomGateway>(
        std::move(room_options), sessions, database_tasks, room_leases,
        match_preparation, match_ids, match_starts, clock, settlement_flow,
        menus, logger);
  }
#endif
  const auto stop_room_gateway = [&] {
#ifdef BF_HAS_ROOM_DS
    if (room_gateway) room_gateway->Stop();
#endif
  };
  bf::runtime::WorldExecutor world_executor{
      {.queue_capacity = std::max(config.database.task_queue_capacity,
                                  config.network.maximum_connections),
       .match_timing =
           {.loading_timeout =
                std::chrono::milliseconds{
                    config.match_timing.loading_timeout_ms},
            .countdown =
                std::chrono::milliseconds{config.match_timing.countdown_ms},
            .match_duration =
                std::chrono::milliseconds{
                    config.match_timing.match_duration_ms},
            .result_transition =
                std::chrono::milliseconds{
                    config.match_timing.result_transition_ms},
            .result_duration =
                std::chrono::milliseconds{
                    config.match_timing.result_duration_ms},
            .use_room_duration = config.match_timing.use_room_duration,
            .weapon_master = config.weapon_master,
            .user_host_prize = config.user_host_prize,
            .hide_seek_scene = config.hide_seek_asset_directory.empty()
                                   ? nullptr
                                   : bf::runtime::LoadHideSeekSceneAssets(
                                         config.hide_seek_asset_directory,
                                         config.hide_seek_development)},
       .equipment_relay = &equipment_relay,
       .equipment_relay_address = *advertised_ipv4,
       .prize_menus = &menus,
       .equipment_relay_port = config.network.equipment_relay_port,
       .tutorial_sessions = &tutorial_sessions,
       .ice_leases = &ice_network
#ifdef BF_HAS_ROOM_DS
       ,
       .room_execution = room_gateway.get()
#endif
      },
      sessions,
      database_tasks,
      match_preparation,
      match_ids,
      match_starts,
      match_repairs,
      clock,
      settlement_flow,
      logger,
      &item_use,
      &world_access};
  if (const auto error = world_executor.Start()) {
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("world-executor-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  SettlementWorldBinding settlement_world_binding{settlement_completions,
                                                  world_executor};
#ifdef BF_HAS_ROOM_DS
  RoomGatewayBinding room_gateway_binding{room_gateway.get()};
  if (room_gateway) room_gateway->Start(world_executor);
#endif
  const bf::protocol::GatewayEndpoint endpoint{.first_value = 1U,
                                               .second_value = 1U,
                                               .third_value = 1U,
                                               .ipv4_address = *advertised_ipv4,
                                               .port = config.network.port};
  bf::infrastructure::mysql::MySqlKeyMapStore key_maps{*pool};
  bf::infrastructure::mysql::MySqlLevelIconStore level_icons{*pool};
  bf::infrastructure::mysql::MySqlChatBlockStore chat_blocks{*pool};
  bf::infrastructure::mysql::MySqlLobbyMinigameStore minigame_store{*pool};
  std::mutex minigame_random_lock;
  std::mt19937 minigame_random{std::random_device{}()};
  bf::runtime::LobbyMinigameService lobby_minigames{
      minigame_store, config.lobby_minigame, [&](std::uint32_t bound) {
        const std::lock_guard lock{minigame_random_lock};
        return std::uniform_int_distribution<std::uint32_t>{
            0U, bound - 1U}(minigame_random);
      }};
  bf::infrastructure::mysql::MySqlCoupleRepository couple_store{*pool};
  bf::runtime::CoupleFlow couple_flow{couple_store, menus, social_queries,
                                      sessions};
  bf::infrastructure::mysql::MySqlSocialSpacesRepository spaces_store{*pool};
  bf::runtime::SocialSpacesFlow spaces_flow{spaces_store, sessions};
  bf::infrastructure::mysql::MySqlGiftStore gifts{*pool};
  bf::infrastructure::mysql::MySqlNativeTitleRepository titles{*pool, clock};

  bf::infrastructure::network::GatewaySessionHandlerFactory handlers{
      database_tasks,
      login,
      account_registration,
      password_login,
      characters,
      menus,
      catalog,
      purchases,
      equipment,
      social_queries,
      social_relationships,
      private_messages,
      service_rules,
      treasure,
      mystery_shop,
      workshop,
      clans,
      item_use,
      world_access,
      world_executor,
      operation_ids,
      sessions,
      clan_tickets,
      clock,
      logger,
      endpoint,
      &daily_rewards,
      config.service.role == "standalone",
      &key_maps,
      &gifts,
      &level_icons,
      &couple_flow,
      &spaces_flow,
      &chat_blocks,
      config.lobby_minigame_enabled ? &lobby_minigames : nullptr,
      &titles,
      &life_attendance};
  bf::infrastructure::network::TcpServer tcp_server{
      {.bind_address = config.network.bind_address,
       .port = config.network.port,
       .io_threads = config.network.io_threads,
       .maximum_connections = config.network.maximum_connections,
       .protocol_limits = config.network.protocol_limits},
      sessions,
      handlers,
      logger};
  bf::runtime::SessionRegistry login_sessions{
      login_config ? login_config->network.maximum_connections : 1U};
  bf::infrastructure::network::LoginSessionHandlerFactory login_handlers{
      database_tasks,
      account_registration,
      password_login,
      {.region_id = config.service.region_id,
       .host = config.network.advertised_address.empty()
                   ? config.network.bind_address
                   : config.network.advertised_address,
       .port = config.network.port,
       .clan_port = config.network.clan_port}};
  std::unique_ptr<bf::infrastructure::network::TcpServer> login_server;
  if (login_config) {
    login_server = std::make_unique<bf::infrastructure::network::TcpServer>(
        bf::infrastructure::network::TcpServerOptions{
            .bind_address = login_config->network.bind_address,
            .port = login_config->network.port,
            .io_threads = login_config->network.io_threads,
            .maximum_connections = login_config->network.maximum_connections,
            .protocol_limits = login_config->network.protocol_limits},
        login_sessions, login_handlers, logger);
  }
  bf::runtime::SessionRegistry equipment_sessions{
      config.network.maximum_connections};
  bf::infrastructure::network::EquipmentRelayHandlerFactory equipment_handlers{
      equipment_relay};
  bf::infrastructure::network::TcpServer equipment_server{
      {.bind_address = config.network.bind_address,
       .port = config.network.equipment_relay_port,
       .io_threads = config.network.io_threads,
       .maximum_connections = config.network.maximum_connections,
       .protocol_limits = config.network.protocol_limits,
       .frame_namespace = bf::protocol::FrameNamespace::kRelay},
      equipment_sessions,
      equipment_handlers,
      logger};
  bf::infrastructure::network::ClanTcpServer clan_server{
      {.bind_address = config.network.bind_address,
       .port = config.network.clan_port,
       .io_threads = config.network.io_threads,
       .maximum_connections = config.network.maximum_connections,
       .maximum_send_queue_bytes =
           config.network.protocol_limits.maximum_send_queue_bytes},
      database_tasks,
      clans,
      menus,
      clan_tickets,
      sessions,
      clock,
      logger};
  bf::infrastructure::network::OperationalHttpServer operations_server{
      {.bind_address = config.operations.bind_address,
       .port = config.operations.port,
       .maximum_connections = config.operations.maximum_connections,
       .maximum_request_bytes = config.operations.maximum_request_bytes},
      [&] {
        const auto world = world_executor.Metrics();
        const auto settlements = settlement_dispatcher.Metrics();
#ifdef BF_HAS_ROOM_DS
        const auto room_metrics =
            room_gateway ? room_gateway->Metrics()
                         : bf::infrastructure::network::RoomGatewayMetrics{};
#endif
        return bf::infrastructure::network::OperationalSnapshot{
            .ready = runtime.IsReady(),
            .gateway_connections =
                tcp_server.ActiveSessionCount() +
                (login_server ? login_server->ActiveSessionCount() : 0U),
            .clan_connections = clan_server.ActiveSessionCount(),
            .database_pool_size = pool->Size(),
            .database_pool_available = pool->Available(),
            .database_tasks_queued = database_tasks.QueuedTaskCount(),
            .world_tasks_queued = world.queued_tasks,
            .active_rooms = world.active_rooms
#ifdef BF_HAS_ROOM_DS
                            + room_metrics.rooms
#endif
            ,
            .plaza_players = world.plaza_players,
            .world_timers = world.scheduled_timers,
            .pending_match_starts = world.pending_match_starts,
            .pending_match_settlements = world.pending_match_settlements,
            .pending_match_compensations = world.pending_match_compensations,
            .movement_coalesced_total = tcp_server.MovementCoalescedTotal(),
            .movement_dropped_total = tcp_server.MovementDroppedTotal(),
            .settlements_accepted = settlements.accepted,
            .settlements_rejected_overload = settlements.rejected_overload,
            .settlements_completed = settlements.completed,
            .settlements_failed = settlements.failed,
            .settlements_queued = settlements.queued,
            .settlements_active = settlements.active,
#ifdef BF_HAS_ROOM_DS
            .ds_healthy_nodes = room_metrics.healthy_nodes,
            .ds_queued_commands = room_metrics.queued_commands,
            .ds_queued_bytes = room_metrics.queued_bytes,
            .ds_rejected_commands = room_metrics.rejected_commands,
            .ds_failed_rooms = room_metrics.failed_rooms,
            .ds_queue_samples = room_metrics.command_queue.samples,
            .ds_queue_p99_us = room_metrics.command_queue.p99_us,
            .ds_immediate_rpc_p99_us = room_metrics.immediate_rpc.p99_us,
            .ds_worst_room_timer_p99_us = room_metrics.worst_room_timer_p99_us,
            .ds_draining = room_metrics.draining,
#endif
        };
      },
      logger};
  if (const auto error = tcp_server.Start()) {
    stop_room_gateway();
    world_executor.DrainAndStop();
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("tcp-listener-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  if (const auto error = equipment_server.Start()) {
    tcp_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    stop_room_gateway();
    world_executor.DrainAndStop();
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("equipment-listener-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  if (const auto error = clan_server.Start()) {
    equipment_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    tcp_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    stop_room_gateway();
    world_executor.DrainAndStop();
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("clan-listener-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  if (const auto error = operations_server.Start()) {
    equipment_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    clan_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    tcp_server.Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
    stop_room_gateway();
    world_executor.DrainAndStop();
    settlement_dispatcher.Shutdown();
    database_tasks.DrainAndStop();
    pool->Stop();
    runtime.RequestStop("operations-listener-start-failed");
    runtime.CompleteStop();
    report_error(error->safe_message, error->correlation_id);
    logger.Shutdown();
    return bf::application::ProcessExitCode(*error);
  }
  if (login_server) {
    if (const auto error = login_server->Start()) {
      operations_server.Stop(
          std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
      equipment_server.Stop(
          std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
      clan_server.Stop(
          std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
      tcp_server.Stop(
          std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
      stop_room_gateway();
      world_executor.DrainAndStop();
      settlement_dispatcher.Shutdown();
      database_tasks.DrainAndStop();
      pool->Stop();
      runtime.RequestStop("login-listener-start-failed");
      runtime.CompleteStop();
      report_error(error->safe_message, error->correlation_id);
      logger.Shutdown();
      return bf::application::ProcessExitCode(*error);
    }
    logger.Log(bf::application::LogLevel::kInfo,
               "[Network] Additional login endpoint ready; listening=" +
                   login_config->network.bind_address + ":" +
                   std::to_string(login_server->LocalPort()));
  }
  logger.Log(bf::application::LogLevel::kInfo,
             "[Network] Game and authentication gateway ready; listening=" +
                 config.network.bind_address + ":" +
                 std::to_string(tcp_server.LocalPort()));
  logger.Log(
      bf::application::LogLevel::kInfo,
      "[Network] Clan service ready; listening=" + config.network.bind_address +
          ":" + std::to_string(clan_server.LocalPort()));
  logger.Log(bf::application::LogLevel::kInfo,
             "[Network] Equipment relay ready; listening=" +
                 config.network.bind_address + ":" +
                 std::to_string(equipment_server.LocalPort()));
  logger.Log(bf::application::LogLevel::kInfo,
             "[Operations] HTTP service ready; listening=" +
                 config.operations.bind_address + ":" +
                 std::to_string(operations_server.LocalPort()));
#else
  logger.Log(
      bf::application::LogLevel::kInfo,
      "Foundation-only build started without external network/database adapters");
#endif
  logger.Log(bf::application::LogLevel::kInfo,
             "[Configuration] Environment=" +
                 std::string{bf::infrastructure::configuration::EnvironmentName(
                     config.environment)} +
                 "; advertised_endpoint=" +
                 (config.network.advertised_address.empty()
                      ? config.network.bind_address
                      : config.network.advertised_address) +
                 ":" + std::to_string(config.network.port));
  logger.Log(bf::application::LogLevel::kInfo,
             "[Server] Startup completed. Accepting client connections.");

  const auto started_at = std::chrono::steady_clock::now();
#ifdef BF_EXTERNAL_DEPENDENCIES
  auto next_ownership_check = started_at;
  auto next_attendance_check = started_at;
#endif
  bool ownership_lost = false;
  bool attendance_storage_failed = false;
  while (stop_signal == 0) {
#ifdef BF_EXTERNAL_DEPENDENCIES
    if (std::chrono::steady_clock::now() >= next_attendance_check) {
      life_attendance.Poll(database_tasks);
      if (!life_attendance.Healthy()) {
        attendance_storage_failed = true;
        break;
      }
      next_attendance_check =
          std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
    if (std::chrono::steady_clock::now() >= next_ownership_check) {
      if (!ownership->Healthy()) {
        ownership_lost = true;
        break;
      }
      next_ownership_check =
          std::chrono::steady_clock::now() + std::chrono::seconds{1};
    }
#endif
    if (command_line.run_for_ms) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started_at);
      if (elapsed >= std::chrono::milliseconds{*command_line.run_for_ms}) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  runtime.RequestStop(attendance_storage_failed ? "attendance-storage-failed"
                      : ownership_lost
                          ? "world-ownership-lost"
                          : (stop_signal == 0 ? "bounded-run-complete"
                                              : "operating-system-signal"));
#ifdef BF_EXTERNAL_DEPENDENCIES
  if (login_server)
    login_server->Stop(
        std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  operations_server.Stop(
      std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  equipment_server.Stop(
      std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  clan_server.Stop(std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  tcp_server.Stop(std::chrono::milliseconds{config.shutdown.grace_timeout_ms});
  stop_room_gateway();
  world_executor.DrainAndStop();
  settlement_dispatcher.Shutdown();
  database_tasks.DrainAndStop();
  life_attendance.Flush();
  attendance_storage_failed =
      attendance_storage_failed || !life_attendance.Healthy();
  pool->Stop();
#endif
  runtime.CompleteStop();
  logger.Shutdown();
  return ownership_lost || attendance_storage_failed ? 3 : 0;
}

}  // namespace

int RunApplication(int argc, char** argv) {
  const auto command_line_result = ParseCommandLine(argc, argv);
  if (const auto* error = std::get_if<ServiceError>(&command_line_result)) {
    PrintUsage();
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError, error->safe_message,
        error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  const auto& command_line = std::get<CommandLine>(command_line_result);

  const auto configuration_result =
      bf::infrastructure::configuration::LoadServerConfig(
          command_line.config_path);
  if (const auto* error = std::get_if<ServiceError>(&configuration_result)) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError, error->safe_message,
        error->correlation_id);
    return bf::application::ProcessExitCode(*error);
  }
  const auto& config = std::get<ServerConfig>(configuration_result);
  if (config.service.role == "login") {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Configuration] service.role=login requires BubbleFighter.Server.exe --mode login.");
    return 2;
  }
  std::optional<ServerConfig> login_config;
  if (!command_line.login_config_path.empty()) {
    auto loaded = bf::infrastructure::configuration::LoadServerConfig(
        command_line.login_config_path);
    if (const auto* error = std::get_if<ServiceError>(&loaded)) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, error->safe_message,
          error->correlation_id);
      return bf::application::ProcessExitCode(*error);
    }
    login_config = std::get<ServerConfig>(std::move(loaded));
    const auto& connection = config.database.connection;
    const auto& other = login_config->database.connection;
    const auto advertised = config.network.advertised_address.empty()
                                ? config.network.bind_address
                                : config.network.advertised_address;
    if (login_config->service.role != "login" ||
        login_config->service.region_id != config.service.region_id ||
        connection.host != other.host || connection.port != other.port ||
        connection.name != other.name ||
        login_config->database.expected_schema_version !=
            config.database.expected_schema_version ||
        login_config->service.game_address != advertised ||
        login_config->service.game_port != config.network.port ||
        login_config->service.game_clan_port != config.network.clan_port) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError,
          "[Configuration] The in-process login service must use the same database, region and game endpoint.");
      return 2;
    }
    const auto port = login_config->network.port;
    if (port == config.network.port || port == config.network.clan_port ||
        port == config.network.equipment_relay_port ||
        port == config.operations.port ||
        (config.ice.port != 0U && port == config.ice.port)) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError,
          "[Configuration] The in-process login port conflicts with another service endpoint.");
      return 2;
    }
  }
  if (!command_line.room_ds_config_path.empty()) {
#ifdef BF_HAS_ROOM_DS
    const auto options = bf::infrastructure::network::LoadRoomGatewayOptions(
        command_line.room_ds_config_path);
    if (options.nodes.empty()) {
      std::cerr << "independent rooms require at least one configured node\n";
      return 2;
    }
#else
    std::cerr
        << "independent rooms require BF_BUILD_ROOM_DS=ON and external dependencies\n";
    return 2;
#endif
  }
  if (command_line.check_config_only) {
    std::cout << "configuration valid: environment="
              << bf::infrastructure::configuration::EnvironmentName(
                     config.environment)
              << '\n';
    return 0;
  }
  if (command_line.check_database_only) {
#ifdef BF_EXTERNAL_DEPENDENCIES
    const auto verification = bf::infrastructure::mysql::VerifySchema(
        config.database.connection, config.database.expected_schema_version,
        config.database.connection.query_timeout_ms);
    if (const auto* error = std::get_if<ServiceError>(&verification)) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, error->safe_message,
          error->correlation_id);
      return bf::application::ProcessExitCode(*error);
    }
    const auto& report =
        std::get<bf::infrastructure::mysql::SchemaVerificationReport>(
            verification);
    auto pool_result = bf::infrastructure::mysql::MySqlConnectionPool::Create(
        {.connection = config.database.connection,
         .size = config.database.pool_size,
         .acquire_timeout =
             std::chrono::milliseconds{config.database.acquire_timeout_ms},
         .query_timeout_ms = config.database.connection.query_timeout_ms});
    if (const auto* error = std::get_if<ServiceError>(&pool_result)) {
      bf::infrastructure::logging::WriteConsoleLog(
          bf::application::LogLevel::kError, error->safe_message,
          error->correlation_id);
      return bf::application::ProcessExitCode(*error);
    }
    const auto& pool = std::get<
        std::unique_ptr<bf::infrastructure::mysql::MySqlConnectionPool>>(
        pool_result);
    std::cout << "database valid: mysql=" << report.server_version
              << " schema=" << report.schema_version
              << " migrations=" << report.migration_count
              << " pool=" << pool->Size() << '\n';
    return 0;
#else
    std::cerr
        << "MySQL production dependencies are not enabled in this build\n";
    return 2;
#endif
  }

  std::signal(SIGINT, HandleGameSignal);
  std::signal(SIGTERM, HandleGameSignal);
#ifdef _WIN32
  std::signal(SIGBREAK, HandleGameSignal);
#endif
  return RunServer(command_line, config,
                   login_config ? &*login_config : nullptr);
}

// 已在函数体捕获标准与未知异常；clang-tidy 对 main 的特殊退出边界仍会产生误报。
int bf::entry::RunGame(int argc, char** argv) {
  try {
    return RunApplication(argc, argv);
  } catch (const std::exception& error) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Startup] Server startup failed: " + std::string{error.what()});
    return 1;
  } catch (...) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kCritical,
        "[Startup] The server stopped due to an unexpected exception. Check the configuration and server logs.");
    return 1;
  }
}
