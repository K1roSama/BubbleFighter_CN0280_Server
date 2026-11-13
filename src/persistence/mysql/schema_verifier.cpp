#include "persistence/mysql/schema_verifier.h"

#include "persistence/mysql/migration_runner.h"
#include "persistence/mysql/sql_connection.h"

namespace bf::infrastructure::mysql {
application::ServiceResult<SchemaVerificationReport> VerifySchema(
    const ConnectionSettings& settings, std::uint64_t expected_version,
    std::uint32_t query_timeout_ms) {
  try {
    if (expected_version != kSchemaVersion) {
      return application::ServiceError{
          .code = application::ErrorCode::kInvalidInput,
          .safe_message =
              "The configured MySQL schema version must match this server version",
          .correlation_id = {}};
    }
    auto connection_settings = settings;
    connection_settings.query_timeout_ms = query_timeout_ms;
    Connection connection(connection_settings);
    const auto version =
        connection.Execute("SELECT VERSION()").OneField().As<std::string>();
    const auto versions = connection.Execute(
        "SELECT MigrationVersion, ScriptDigest FROM SchemaMigrations ORDER BY MigrationVersion");
    bool valid_history = versions.size() == expected_version;
    for (std::size_t index = 0; index < versions.size(); ++index) {
      valid_history = valid_history &&
                      versions[index][0].As<std::uint64_t>() == index + 1U &&
                      versions[index][1].As<std::string_view>().size() == 64U;
    }
    if (!valid_history) {
      return application::ServiceError{
          .code = application::ErrorCode::kInvalidState,
          .safe_message = "MySQL schema version mismatch",
          .correlation_id = {}};
    }
    return SchemaVerificationReport{.server_version = version,
                                    .schema_version = expected_version,
                                    .migration_count = versions.size()};
  } catch (const std::exception&) {
    return application::ServiceError{
        .code = application::ErrorCode::kStorageUnavailable,
        .safe_message = "Unable to verify the MySQL schema version",
        .correlation_id = {},
        .is_retryable = true};
  }
}

}  // namespace bf::infrastructure::mysql
