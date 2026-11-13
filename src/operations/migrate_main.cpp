#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "application/entry_points.h"
#include "persistence/mysql/migration_runner.h"
#include "persistence/mysql/sql_connection.h"

namespace {
void PrintUsage() {
  std::cerr
      << "Usage: BubbleFighter.Server.exe --mode migrate --config <database.conf> --migrations <directory>\n";
}
}  // namespace

int bf::entry::RunMigrations(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
    PrintUsage();
    return 0;
  }
  std::filesystem::path config;
  std::filesystem::path migrations;
  for (int i = 1; i < argc; i += 2) {
    const std::wstring_view flag = argv[i];
    if (i + 1 == argc) {
      PrintUsage();
      return 2;
    }
    if (flag == L"--config" && config.empty()) {
      config = argv[i + 1];
    } else if (flag == L"--migrations" && migrations.empty()) {
      migrations = argv[i + 1];
    } else {
      PrintUsage();
      return 2;
    }
  }
  if (config.empty() || migrations.empty()) {
    PrintUsage();
    return 2;
  }
  try {
    const auto settings =
        bf::infrastructure::mysql::LoadConnectionSettings(config);
    const auto result =
        bf::infrastructure::mysql::RunMigrations(settings, migrations);
    std::cout << "migration succeeded: previous=" << result.previous_version
              << " current=" << result.current_version
              << " applied=" << result.applied_count << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "MySQL migration failed: " << error.what() << '\n';
    return 3;
  }
}
