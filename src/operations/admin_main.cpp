#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "application/entry_points.h"
#include "application/error.h"
#include "operations/admin/moderation.h"
#include "operations/utf8_path.h"
#include "persistence/mysql/admin_maintenance.h"
#include "persistence/mysql/connection_pool.h"
#include "persistence/mysql/sql_connection.h"

namespace {

struct CommandLine final {
  bf::infrastructure::mysql::ExecuteClanDisbandsCommand command;
  std::filesystem::path database_config{"config/admin.conf"};
};

void PrintUsage() {
  PrintModerationUsage();
  std::cerr
      << "Usage: BubbleFighter.Server.exe --mode admin execute-clan-disbands --actor <operator> --reason <reason> "
         "--business-id <uuid> [--limit <1..1000>] "
         "--config <database.conf>\n";
}

bf::application::ServiceError InvalidArgument(std::string message) {
  return {.code = bf::application::ErrorCode::kInvalidInput,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = false};
}

std::optional<std::size_t> ParseLimit(std::string_view text) noexcept {
  std::size_t value = 0U;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bf::application::ServiceResult<CommandLine> ParseCommandLine(int argc,
                                                             char** argv) {
  if (argc < 2 || std::string_view{argv[1]} != "execute-clan-disbands") {
    return InvalidArgument("必须指定 execute-clan-disbands 维护动作");
  }
  CommandLine result;
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc) {
      return InvalidArgument("维护命令参数缺少值");
    }
    const std::string value{argv[++index]};
    if (argument == "--actor") {
      result.command.actor = value;
    } else if (argument == "--reason") {
      result.command.reason = value;
    } else if (argument == "--business-id") {
      result.command.business_id = value;
    } else if (argument == "--limit") {
      const auto limit = ParseLimit(value);
      if (!limit.has_value()) {
        return InvalidArgument("--limit 必须是十进制非负整数");
      }
      result.command.limit = *limit;
    } else if (argument == "--config") {
      result.database_config = bf::infrastructure::Utf8Path(value);
    } else {
      return InvalidArgument("维护命令包含未知参数");
    }
  }
  if (result.database_config.empty()) {
    return InvalidArgument("数据库配置路径不能为空");
  }
  return result;
}

int RunApplication(int argc, char** argv) {
  if (argc >= 2 && IsModerationCommand(argv[1]))
    return RunModerationCommand(argc, argv);
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    PrintUsage();
    return 0;
  }
  const auto parsed = ParseCommandLine(argc, argv);
  if (const auto* parse_error =
          std::get_if<bf::application::ServiceError>(&parsed)) {
    PrintUsage();
    std::cerr << parse_error->safe_message << '\n';
    return bf::application::ProcessExitCode(*parse_error);
  }
  const auto& options = std::get<CommandLine>(parsed);
  auto pool_result = bf::infrastructure::mysql::MySqlConnectionPool::Create(
      {.connection = bf::infrastructure::mysql::LoadConnectionSettings(
           options.database_config),
       .size = 1U,
       .acquire_timeout = std::chrono::seconds{2},
       .query_timeout_ms = 30'000U});
  auto* pool = std::get_if<
      std::unique_ptr<bf::infrastructure::mysql::MySqlConnectionPool>>(
      &pool_result);
  if (pool == nullptr || !*pool) {
    const auto& pool_error =
        std::get<bf::application::ServiceError>(pool_result);
    std::cerr << pool_error.safe_message << '\n';
    return bf::application::ProcessExitCode(pool_error);
  }
  const auto executed = bf::infrastructure::mysql::ExecuteDueClanDisbands(
      **pool, options.command);
  if (const auto* execute_error =
          std::get_if<bf::application::ServiceError>(&executed)) {
    std::cerr << execute_error->safe_message << '\n';
    return bf::application::ProcessExitCode(*execute_error);
  }
  const auto& report =
      std::get<bf::infrastructure::mysql::ExecuteClanDisbandsReport>(executed);
  std::cout
      << "maintenance succeeded: action=execute_clan_disbands business_id="
      << options.command.business_id
      << " already_applied=" << (report.already_applied ? "true" : "false")
      << " affected_clans=" << report.clan_ids.size();
  for (const auto clan_id : report.clan_ids) {
    std::cout << ' ' << clan_id;
  }
  std::cout << '\n';
  return 0;
}

}  // namespace

// 管理命令顶层将第三方异常转为脱敏退出码，禁止连接 URI 或 SQL
// 文本泄漏到控制台。
int bf::entry::RunAdmin(int argc, char** argv) {
  try {
    return RunApplication(argc, argv);
  } catch (const std::exception&) {
    std::cerr << "maintenance failed: unexpected process error\n";
    return 1;
  } catch (...) {
    std::cerr << "maintenance failed: unknown process error\n";
    return 1;
  }
}
