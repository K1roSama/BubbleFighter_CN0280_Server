#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MIGRATION_RUNNER_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MIGRATION_RUNNER_H_

#include <cstdint>
#include <filesystem>

#include "features/configuration/mysql_settings.h"

namespace bf::infrastructure::mysql {
inline constexpr std::uint64_t kSchemaVersion = 2U;

struct MigrationReport {
  std::uint64_t previous_version{};
  std::uint64_t current_version{};
  std::size_t applied_count{};
};

MigrationReport RunMigrations(const ConnectionSettings& settings,
                              const std::filesystem::path& directory);

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_MIGRATION_RUNNER_H_
