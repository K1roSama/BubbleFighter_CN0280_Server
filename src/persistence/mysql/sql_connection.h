#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SQL_CONNECTION_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SQL_CONNECTION_H_

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "features/configuration/mysql_settings.h"

namespace bf::infrastructure::mysql {
ConnectionSettings LoadConnectionSettings(const std::filesystem::path& path);

class SqlError : public std::runtime_error {
 public:
  SqlError(unsigned code, std::string_view sqlstate,
           std::string_view operation);

  unsigned Code() const noexcept { return code_; }

  std::string_view Sqlstate() const noexcept { return sqlstate_; }

 private:
  unsigned code_;
  std::string sqlstate_;
};

class UniqueViolation final : public SqlError {
  using SqlError::SqlError;
};

class CheckViolation final : public SqlError {
  using SqlError::SqlError;
};

class BrokenConnection : public SqlError {
  using SqlError::SqlError;
};

class InDoubtError final : public BrokenConnection {
  using BrokenConnection::BrokenConnection;
};

class ConversionError final : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Bytes = std::vector<std::byte>;
using BytesView = std::span<const std::byte>;

class TransactionBase;

class Parameters {
 public:
  using Value = std::variant<std::nullptr_t, std::int64_t, std::uint64_t,
                             double, std::string, Bytes>;

  Parameters() = default;

  template <typename... Args>
  explicit Parameters(TransactionBase&, const Args&... args) {
    (Append(args), ...);
  }

  template <typename T>
  void Append(const T& value) {
    using Type = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Type, std::nullptr_t>) {
      values_.emplace_back(nullptr);
    } else if constexpr (std::is_same_v<Type, Bytes> ||
                         std::is_same_v<Type, BytesView>) {
      values_.emplace_back(Bytes(value.begin(), value.end()));
    } else if constexpr (std::is_same_v<Type, bool> ||
                         std::is_unsigned_v<Type>) {
      values_.emplace_back(static_cast<std::uint64_t>(value));
    } else if constexpr (std::is_integral_v<Type>) {
      values_.emplace_back(static_cast<std::int64_t>(value));
    } else if constexpr (std::is_floating_point_v<Type>) {
      values_.emplace_back(static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
      values_.emplace_back(std::string(value));
    } else {
      if (value.has_value()) {
        Append(*value);
      } else {
        values_.emplace_back(nullptr);
      }
    }
  }

  const std::vector<Value>& Values() const noexcept { return values_; }

 private:
  std::vector<Value> values_;
};

class Field {
 public:
  Field() = default;

  explicit Field(std::string value) : value_(std::move(value)) {}

  bool IsNull() const noexcept { return !value_.has_value(); }

  const char* CStr() const { return value_ ? value_->c_str() : nullptr; }

  template <typename T>
  T As() const {
    if constexpr (std::is_same_v<T, std::optional<std::uint32_t>>) {
      return value_ ? std::optional{As<std::uint32_t>()} : std::nullopt;
    } else {
      if (!value_) {
        throw ConversionError("Unexpected SQL NULL");
      }
      const std::string& value = *value_;
      if constexpr (std::is_same_v<T, std::string> ||
                    std::is_same_v<T, std::string_view>) {
        return value;
      } else if constexpr (std::is_same_v<T, Bytes>) {
        const auto bytes = std::as_bytes(std::span{value});
        return Bytes(bytes.begin(), bytes.end());
      } else if constexpr (std::is_same_v<T, bool>) {
        if (value == "1" || value == "true" || value == "t") {
          return true;
        }
        if (value == "0" || value == "false" || value == "f") {
          return false;
        }
        throw ConversionError("Invalid SQL boolean");
      } else {
        T parsed{};
        const char* end = value.data() + value.size();
        if constexpr (std::is_integral_v<T>) {
          // The binary result protocol represents exact DECIMALs as text.
          const auto decimal = value.find('.');
          if (decimal != std::string::npos &&
              value.find_first_not_of('0', decimal + 1) == std::string::npos) {
            end = value.data() + decimal;
          }
        }
        const auto conversion = std::from_chars(value.data(), end, parsed);
        if (conversion.ec != std::errc{} || conversion.ptr != end) {
          throw ConversionError("Invalid SQL number");
        }
        return parsed;
      }
    }
  }

  template <typename T>
  T As(T fallback) const {
    return value_ ? As<T>() : std::move(fallback);
  }

 private:
  std::optional<std::string> value_;
};

class Row {
 public:
  std::size_t size() const noexcept { return fields_.size(); }

  const Field& operator[](std::size_t index) const { return fields_.at(index); }

  const Field& operator[](std::string_view name) const;

 private:
  friend class Connection;
  friend class QueryResult;
  std::shared_ptr<const std::vector<std::string>> names_;
  std::vector<Field> fields_;
};

class QueryResult {
 public:
  using Iterator = std::vector<Row>::const_iterator;
  using size_type = std::size_t;

  bool empty() const noexcept { return rows_.empty(); }

  std::size_t size() const noexcept { return rows_.size(); }

  Iterator begin() const noexcept { return rows_.begin(); }

  Iterator end() const noexcept { return rows_.end(); }

  auto rbegin() const noexcept { return rows_.rbegin(); }

  auto rend() const noexcept { return rows_.rend(); }

  const Row& operator[](std::size_t index) const { return rows_.at(index); }

  const Row& front() const { return rows_.front(); }

  Row OneRow() const;
  Field OneField() const;
  void NoRows() const;

  std::uint64_t AffectedRows() const noexcept { return affected_rows_; }

  std::uint64_t InsertId() const noexcept { return insert_id_; }

 private:
  friend class Connection;
  friend class TransactionBase;
  std::vector<Row> rows_;
  std::uint64_t affected_rows_{};
  std::uint64_t insert_id_{};
};

class Connection {
 public:
  explicit Connection(const ConnectionSettings& settings);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  QueryResult Execute(std::string_view query, const Parameters& params = {});
  void ExecuteDdl(std::string_view statement);
  bool IsOpen() const noexcept;
  std::string Dbname() const;
  void Invalidate() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

enum class TransactionMode { kWrite, kReadOnly, kSnapshot, kAutocommit };

class TransactionBase {
 public:
  explicit TransactionBase(Connection& connection, TransactionMode mode);
  ~TransactionBase();

  TransactionBase(const TransactionBase&) = delete;
  TransactionBase& operator=(const TransactionBase&) = delete;

  QueryResult Exec(std::string_view query, const Parameters& params = {});
  void Commit();
  void Abort();

 protected:
  Connection& connection;
  bool active;
};

class WriteTransaction final : public TransactionBase {
 public:
  explicit WriteTransaction(Connection& connection)
      : TransactionBase(connection, TransactionMode::kWrite) {}
};

class ReadTransaction final : public TransactionBase {
 public:
  explicit ReadTransaction(Connection& connection)
      : TransactionBase(connection, TransactionMode::kReadOnly) {}
};

class SnapshotTransaction final : public TransactionBase {
 public:
  explicit SnapshotTransaction(Connection& connection)
      : TransactionBase(connection, TransactionMode::kSnapshot) {}
};

class Autocommit final : public TransactionBase {
 public:
  explicit Autocommit(Connection& connection)
      : TransactionBase(connection, TransactionMode::kAutocommit) {}
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_MYSQL_SQL_CONNECTION_H_
