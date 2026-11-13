#include "persistence/mysql/sql_connection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

#include "operations/utf8_path.h"

namespace bf::infrastructure::mysql {
namespace {
class Library {
 public:
  Library() {
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
      throw BrokenConnection(CR_UNKNOWN_ERROR, "HY000",
                             "initialize client library");
    }
  }

  ~Library() { mysql_library_end(); }
};

class ThreadSession {
 public:
  ThreadSession() {
    if (mysql_thread_init()) {
      throw BrokenConnection(CR_UNKNOWN_ERROR, "HY000",
                             "initialize client thread");
    }
  }

  ~ThreadSession() { mysql_thread_end(); }
};

void InitializeClient() {
  static const Library kLibrary;
  thread_local const ThreadSession kThread;
  (void)kLibrary;
  (void)kThread;
}

[[noreturn]] void ThrowSql(unsigned code, const char* state,
                           std::string_view operation) {
  if (code == CR_SERVER_GONE_ERROR || code == CR_SERVER_LOST ||
      code == CR_CONNECTION_ERROR || code == CR_CONN_HOST_ERROR) {
    throw BrokenConnection(code, state, operation);
  }
  if (code == ER_DUP_ENTRY || code == ER_DUP_KEY) {
    throw UniqueViolation(code, state, operation);
  }
  if (code == ER_CHECK_CONSTRAINT_VIOLATED || code == ER_SIGNAL_EXCEPTION) {
    throw CheckViolation(code, state, operation);
  }
  throw SqlError(code, state, operation);
}

unsigned long Length(std::size_t length) {
  if (length > std::numeric_limits<unsigned long>::max()) {
    throw std::length_error("MySQL value is too long");
  }
  return static_cast<unsigned long>(length);
}

struct CompiledQuery {
  std::string sql;
  std::vector<std::size_t> order;
};

CompiledQuery CompileQuery(std::string_view query,
                           std::size_t parameter_count) {
  CompiledQuery compiled;
  compiled.sql.reserve(query.size());
  char quote{};
  std::size_t positional{};
  bool numbered{};
  for (std::size_t i = 0; i < query.size();) {
    const char token = query[i];
    if (quote) {
      compiled.sql.push_back(token);
      ++i;
      if (token == quote) {
        if (i < query.size() && query[i] == quote) {
          compiled.sql.push_back(query[i++]);
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (token == '\'' || token == '"' || token == 96) {
      quote = token;
    }
    if (token == '?') {
      compiled.order.push_back(positional++);
    } else if (token == '$' && i + 1 < query.size() && query[i + 1] >= '1' &&
               query[i + 1] <= '9') {
      std::size_t index{};
      const auto parsed = std::from_chars(query.data() + i + 1,
                                          query.data() + query.size(), index);
      if (parsed.ec != std::errc{} || index == 0 || index > parameter_count) {
        throw ConversionError("Invalid SQL parameter index");
      }
      numbered = true;
      compiled.sql.push_back('?');
      compiled.order.push_back(index - 1);
      i = static_cast<std::size_t>(parsed.ptr - query.data());
      continue;
    }
    compiled.sql.push_back(token);
    ++i;
  }
  if ((numbered && positional != 0) ||
      (!numbered && positional != parameter_count)) {
    throw ConversionError("SQL parameter count mismatch");
  }
  return compiled;
}

using Statement = std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)>;
using MySqlResult = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;

void Check(MYSQL_STMT* stmt, int status, std::string_view operation) {
  if (status != 0) {
    ThrowSql(mysql_stmt_errno(stmt), mysql_stmt_sqlstate(stmt), operation);
  }
}

std::string EnvironmentPassword(const std::string& name) {
  if (name.empty()) {
    return {};
  }
  char* buffer = nullptr;
  std::size_t length{};
  if (_dupenv_s(&buffer, &length, name.c_str()) != 0 || !buffer ||
      length <= 1) {
    std::free(buffer);
    throw std::invalid_argument(
        "MySQL password environment variable is missing");
  }
  const std::unique_ptr<char, decltype(&std::free)> password(buffer,
                                                             &std::free);
  return std::string(password.get(), length - 1);
}

std::string ConnectionPassword(const ConnectionSettings& settings) {
  if (settings.password_file.empty())
    return EnvironmentPassword(settings.password_environment);
  std::ifstream input(settings.password_file, std::ios::binary);
  if (!input) throw std::invalid_argument("Cannot open MySQL password file");
  std::array<char, 4097U> bytes{};
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  const auto length = input.gcount();
  if (!input.eof() || length > 4096)
    throw std::invalid_argument("MySQL password file exceeds its size limit");
  std::string password(bytes.data(), static_cast<std::size_t>(length));
  if (password.starts_with("\xEF\xBB\xBF")) password.erase(0, 3);
  if (password.ends_with("\r\n"))
    password.resize(password.size() - 2);
  else if (password.ends_with('\n'))
    password.pop_back();
  if (password.empty() ||
      password.find_first_of(std::string{"\0\r\n", 3}) != std::string::npos)
    throw std::invalid_argument(
        "MySQL password file must contain one nonempty line");
  if (password == "CHANGE_ME")
    throw std::invalid_argument(
        "Set the MySQL password in config/mysql.password before starting");
  return password;
}

std::string_view Trim(std::string_view text) {
  const auto start = text.find_first_not_of(" \t\r");
  if (start == std::string_view::npos) {
    return {};
  }
  return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
}

void Validate(const ConnectionSettings& settings) {
  if (settings.host.empty() || settings.name.empty() || settings.user.empty() ||
      settings.port == 0 || settings.query_timeout_ms == 0 ||
      !std::all_of(
          settings.name.begin(), settings.name.end(),
          [](unsigned char c) { return c == '_' || std::isalnum(c); })) {
    throw std::invalid_argument("Invalid MySQL connection configuration");
  }
  if (!settings.password_environment.empty() && !settings.password_file.empty())
    throw std::invalid_argument(
        "MySQL password environment and file are mutually exclusive");
  const bool loopback = settings.host == "127.0.0.1" ||
                        settings.host == "::1" || settings.host == "localhost";
  if (settings.tls == "verify_identity" && !settings.ca_file.empty()) {
    return;
  }
  if (settings.tls != "required" || !loopback) {
    throw std::invalid_argument(
        "MySQL TLS must verify the identity of a remote server");
  }
}

template <typename T>
T ParseNumber(std::string_view value) {
  T result{};
  const auto end = value.data() + value.size();
  const auto status = std::from_chars(value.data(), end, result);
  if (status.ec != std::errc{} || status.ptr != end) {
    throw std::invalid_argument("Invalid numeric MySQL option");
  }
  return result;
}
}  // namespace

ConnectionSettings LoadConnectionSettings(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::invalid_argument("Cannot open MySQL configuration");
  }
  ConnectionSettings config;
  std::unordered_set<std::string> seen;
  std::string line;
  while (std::getline(input, line)) {
    const auto record = Trim(line);
    if (record.empty() || record.front() == '#') {
      continue;
    }
    const auto split = record.find('=');
    if (split == std::string_view::npos) {
      throw std::invalid_argument("Malformed MySQL configuration");
    }
    const auto key = Trim(record.substr(0, split));
    const auto value = Trim(record.substr(split + 1));
    if (!seen.emplace(key).second) {
      throw std::invalid_argument("Duplicate MySQL configuration key");
    }
    if (key == "database.host") {
      config.host = value;
    } else if (key == "database.port") {
      config.port = ParseNumber<std::uint16_t>(value);
    } else if (key == "database.name") {
      config.name = value;
    } else if (key == "database.user") {
      config.user = value;
    } else if (key == "database.password_env") {
      config.password_environment = value;
    } else if (key == "database.password_file") {
      if (!value.empty())
        config.password_file = std::filesystem::absolute(path).parent_path() /
                               bf::infrastructure::Utf8Path(value);
    } else if (key == "database.tls") {
      config.tls = value;
    } else if (key == "database.ca_file") {
      if (!value.empty()) {
        config.ca_file =
            path.parent_path() / bf::infrastructure::Utf8Path(value);
      }
    } else if (key == "database.query_timeout_ms") {
      config.query_timeout_ms = ParseNumber<std::uint32_t>(value);
    } else {
      throw std::invalid_argument("Unknown MySQL configuration key");
    }
  }
  Validate(config);
  return config;
}

SqlError::SqlError(unsigned code, std::string_view sqlstate,
                   std::string_view operation)
    : std::runtime_error(std::string(operation) + " (MySQL " +
                         std::to_string(code) + ")"),
      code_(code),
      sqlstate_(sqlstate) {}

const Field& Row::operator[](std::string_view name) const {
  const auto found = std::find(names_->begin(), names_->end(), name);
  if (found == names_->end()) {
    throw ConversionError("Unknown MySQL result column");
  }
  return fields_.at(static_cast<std::size_t>(found - names_->begin()));
}

Row QueryResult::OneRow() const {
  if (rows_.size() != 1) {
    throw ConversionError("Expected one MySQL row");
  }
  return rows_.front();
}

Field QueryResult::OneField() const {
  if (rows_.size() != 1 || rows_.front().fields_.size() != 1) {
    throw ConversionError("Expected one MySQL field");
  }
  return rows_.front()[0];
}

void QueryResult::NoRows() const {
  if (!empty()) {
    throw ConversionError("Unexpected MySQL result rows");
  }
}

class Connection::Impl {
 public:
  explicit Impl(const ConnectionSettings& settings)
      : handle(mysql_init(nullptr), &mysql_close), name(settings.name) {
    if (!handle) {
      throw BrokenConnection(CR_OUT_OF_MEMORY, "HY000",
                             "create MySQL connection");
    }
    auto option = [this](mysql_option key, const void* value) {
      if (mysql_options(handle.get(), key, value) != 0) {
        ThrowSql(mysql_errno(handle.get()), mysql_sqlstate(handle.get()),
                 "set MySQL option");
      }
    };
    const unsigned timeout =
        std::max(1U, (settings.query_timeout_ms + 999U) / 1000U);
    const unsigned protocol = MYSQL_PROTOCOL_TCP;
    const unsigned tls = settings.tls == "verify_identity"
                             ? SSL_MODE_VERIFY_IDENTITY
                             : SSL_MODE_REQUIRED;
    const unsigned local_infile = 0;
    option(MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    option(MYSQL_OPT_READ_TIMEOUT, &timeout);
    option(MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    option(MYSQL_OPT_PROTOCOL, &protocol);
    option(MYSQL_OPT_SSL_MODE, &tls);
    option(MYSQL_OPT_LOCAL_INFILE, &local_infile);
    option(MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!settings.ca_file.empty()) {
      const auto ca_file = settings.ca_file.string();
      option(MYSQL_OPT_SSL_CA, ca_file.c_str());
    }
    const auto password = ConnectionPassword(settings);
    if (!mysql_real_connect(handle.get(), settings.host.c_str(),
                            settings.user.c_str(), password.c_str(),
                            settings.name.c_str(), settings.port, nullptr,
                            CLIENT_FOUND_ROWS)) {
      ThrowSql(mysql_errno(handle.get()), mysql_sqlstate(handle.get()),
               "connect to MySQL");
    }
  }

  std::unique_ptr<MYSQL, decltype(&mysql_close)> handle;
  std::string name;
};

Connection::Connection(const ConnectionSettings& settings) {
  Validate(settings);
  InitializeClient();
  impl_ = std::make_unique<Impl>(settings);
  // Preserve six fractional places when dividing integral epoch microseconds.
  ExecuteDdl(
      "SET SESSION time_zone='+00:00', collation_connection='utf8mb4_bin', "
      "sql_mode='STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO,NO_ZERO_DATE,"
      "NO_ZERO_IN_DATE,NO_ENGINE_SUBSTITUTION,NO_BACKSLASH_ESCAPES,ONLY_FULL_GROUP_BY', "
      "div_precision_increment=6,sort_buffer_size=8388608,"
      "group_concat_max_len=8192,max_execution_time=" +
      std::to_string(settings.query_timeout_ms) +
      ", innodb_lock_wait_timeout=" +
      std::to_string(std::max(1U, (settings.query_timeout_ms + 999U) / 1000U)));
  ExecuteDdl("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
  impl_->name = Execute("SELECT DATABASE()").OneField().As<std::string>();
}

Connection::~Connection() = default;

bool Connection::IsOpen() const noexcept { return impl_ != nullptr; }

std::string Connection::Dbname() const {
  return impl_ ? impl_->name : std::string{};
}

void Connection::Invalidate() noexcept { impl_.reset(); }

void Connection::ExecuteDdl(std::string_view query) {
  InitializeClient();
  if (!impl_) {
    throw BrokenConnection(CR_SERVER_GONE_ERROR, "HY000",
                           "execute MySQL statement");
  }
  auto* handle = impl_->handle.get();
  try {
    if (mysql_real_query(handle, query.data(), Length(query.size())) != 0) {
      ThrowSql(mysql_errno(handle), mysql_sqlstate(handle),
               "execute MySQL statement");
    }
    if (MySqlResult result(mysql_store_result(handle), &mysql_free_result);
        !result && mysql_field_count(handle) != 0) {
      ThrowSql(mysql_errno(handle), mysql_sqlstate(handle),
               "read MySQL result");
    }
  } catch (const BrokenConnection&) {
    Invalidate();
    throw;
  }
}

QueryResult Connection::Execute(std::string_view query,
                                const Parameters& params) {
  InitializeClient();
  if (!impl_) {
    throw BrokenConnection(CR_SERVER_GONE_ERROR, "HY000",
                           "prepare MySQL statement");
  }
  try {
    auto* handle = impl_->handle.get();
    const auto compiled = CompileQuery(query, params.Values().size());
    const Statement stmt(mysql_stmt_init(handle), &mysql_stmt_close);
    if (!stmt) {
      ThrowSql(mysql_errno(handle), mysql_sqlstate(handle),
               "create MySQL statement");
    }
    Check(stmt.get(),
          mysql_stmt_prepare(stmt.get(), compiled.sql.data(),
                             Length(compiled.sql.size())),
          "prepare MySQL statement");
    if (mysql_stmt_param_count(stmt.get()) != compiled.order.size()) {
      throw ConversionError("MySQL parameter count mismatch");
    }
    std::vector<MYSQL_BIND> binds(compiled.order.size());
    for (std::size_t i = 0; i < compiled.order.size(); ++i) {
      auto& bind = binds[i];
      const auto& parameter = params.Values().at(compiled.order[i]);
      std::visit(
          [&bind](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, std::nullptr_t>) {
              bind.buffer_type = MYSQL_TYPE_NULL;
            } else if constexpr (std::is_same_v<Type, std::string> ||
                                 std::is_same_v<Type, Bytes>) {
              bind.buffer_type = std::is_same_v<Type, Bytes>
                                     ? MYSQL_TYPE_BLOB
                                     : MYSQL_TYPE_STRING;
              bind.buffer =
                  const_cast<void*>(static_cast<const void*>(value.data()));
              bind.buffer_length = Length(value.size());
            } else {
              bind.buffer_type = std::is_same_v<Type, double>
                                     ? MYSQL_TYPE_DOUBLE
                                     : MYSQL_TYPE_LONGLONG;
              bind.buffer = const_cast<void*>(static_cast<const void*>(&value));
              bind.is_unsigned = std::is_same_v<Type, std::uint64_t>;
            }
          },
          parameter);
    }
    if (!binds.empty()) {
      Check(stmt.get(), mysql_stmt_bind_param(stmt.get(), binds.data()),
            "bind MySQL parameters");
    }
    Check(stmt.get(), mysql_stmt_execute(stmt.get()),
          "execute MySQL statement");
    QueryResult result;
    result.affected_rows_ = mysql_stmt_affected_rows(stmt.get());
    result.insert_id_ = mysql_stmt_insert_id(stmt.get());
    const auto count = mysql_stmt_field_count(stmt.get());
    if (count == 0) {
      return result;
    }
    const MySqlResult metadata(mysql_stmt_result_metadata(stmt.get()),
                               &mysql_free_result);
    if (!metadata) {
      ThrowSql(mysql_stmt_errno(stmt.get()), mysql_stmt_sqlstate(stmt.get()),
               "read result metadata");
    }
    auto names = std::make_shared<std::vector<std::string>>();
    const auto* fields = mysql_fetch_fields(metadata.get());
    for (unsigned i = 0; i < count; ++i) {
      names->emplace_back(fields[i].name, fields[i].name_length);
    }

    struct Buffer {
      std::array<char, 64> data;
      unsigned long size{};
      bool is_null{};
    };

    std::vector<Buffer> buffers(count);
    std::vector<MYSQL_BIND> columns(count);
    for (unsigned i = 0; i < count; ++i) {
      auto& buffer = buffers[i];
      auto& column = columns[i];
      column.buffer_type =
          fields[i].charsetnr == 63 && (fields[i].type == MYSQL_TYPE_BLOB ||
                                        fields[i].type == MYSQL_TYPE_STRING ||
                                        fields[i].type == MYSQL_TYPE_VAR_STRING)
              ? MYSQL_TYPE_BLOB
              : MYSQL_TYPE_STRING;
      column.buffer = buffer.data.data();
      column.buffer_length = Length(buffer.data.size());
      column.length = &buffer.size;
      column.is_null = &buffer.is_null;
    }
    Check(stmt.get(), mysql_stmt_bind_result(stmt.get(), columns.data()),
          "bind MySQL result");
    for (;;) {
      const int fetched = mysql_stmt_fetch(stmt.get());
      if (fetched == MYSQL_NO_DATA) {
        break;
      }
      if (fetched != MYSQL_DATA_TRUNCATED) {
        Check(stmt.get(), fetched, "fetch MySQL row");
      }
      Row row;
      row.names_ = names;
      row.fields_.reserve(count);
      for (unsigned i = 0; i < count; ++i) {
        const auto& buffer = buffers[i];
        if (buffer.is_null) {
          row.fields_.emplace_back();
        } else if (buffer.size <= buffer.data.size()) {
          row.fields_.emplace_back(
              std::string(buffer.data.data(), buffer.size));
        } else {
          std::string large(buffer.size, '\0');
          MYSQL_BIND column = columns[i];
          column.buffer = large.data();
          column.buffer_length = buffer.size;
          Check(stmt.get(), mysql_stmt_fetch_column(stmt.get(), &column, i, 0),
                "fetch MySQL column");
          row.fields_.emplace_back(std::move(large));
        }
      }
      result.rows_.push_back(std::move(row));
    }
    result.affected_rows_ = result.rows_.size();
    return result;
  } catch (const BrokenConnection&) {
    Invalidate();
    throw;
  }
}

TransactionBase::TransactionBase(Connection& connection, TransactionMode mode)
    : connection(connection), active(mode != TransactionMode::kAutocommit) {
  if (mode == TransactionMode::kSnapshot) {
    connection.ExecuteDdl("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    connection.ExecuteDdl(
        "START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY");
  } else if (mode == TransactionMode::kReadOnly) {
    connection.ExecuteDdl("START TRANSACTION READ ONLY");
  } else if (mode == TransactionMode::kWrite) {
    connection.ExecuteDdl("START TRANSACTION");
  }
}

TransactionBase::~TransactionBase() {
  if (!active) {
    return;
  }
  try {
    Abort();
  } catch (...) {
    connection.Invalidate();
  }
}

void TransactionBase::Commit() {
  if (!active) {
    return;
  }
  try {
    connection.ExecuteDdl("COMMIT");
    active = false;
  } catch (const BrokenConnection& error) {
    active = false;
    throw InDoubtError(error.Code(), error.Sqlstate(),
                       "commit result is unknown");
  }
}

void TransactionBase::Abort() {
  if (active) {
    connection.ExecuteDdl("ROLLBACK");
    active = false;
  }
}
}  // namespace bf::infrastructure::mysql
