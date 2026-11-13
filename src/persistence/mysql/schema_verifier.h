#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SCHEMA_VERIFIER_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SCHEMA_VERIFIER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "application/error.h"
#include "features/configuration/mysql_settings.h"

namespace bf::infrastructure::mysql {
struct SchemaVerificationReport {
  std::string server_version;
  std::uint64_t schema_version{};
  std::size_t migration_count{};
};

[[nodiscard]] application::ServiceResult<SchemaVerificationReport> VerifySchema(
    const ConnectionSettings& settings, std::uint64_t expected_version,
    std::uint32_t query_timeout_ms);

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SCHEMA_VERIFIER_H_
