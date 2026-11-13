#include "operations/async_file_logger.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <syncstream>
#include <thread>
#include <utility>

#include "application/event_logger.h"

namespace bf::infrastructure::logging {
namespace {

constexpr std::size_t kMaximumMessageBytes = 8192U;
constexpr std::size_t kMaximumCorrelationIdBytes = 128U;

struct LogRecord final {
  std::chrono::system_clock::time_point timestamp;
  application::LogLevel level;
  std::string message;
  std::string correlation_id;
};

std::string SanitizeText(std::string_view text, std::size_t maximum_bytes) {
  std::string result;
  result.reserve(std::min(text.size(), maximum_bytes));
  for (const unsigned char character : text) {
    if (result.size() >= maximum_bytes) {
      break;
    }
    switch (character) {
      case '\r':
        result += "\\r";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result.push_back(character < 0x20U || character == 0x7FU
                             ? '?'
                             : static_cast<char>(character));
        break;
    }
  }
  if (text.size() > maximum_bytes && result.size() + 3U <= maximum_bytes) {
    result += "...";
  }
  return result;
}

std::string FormatTimestamp(std::chrono::system_clock::time_point timestamp,
                            bool local_time = false) {
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm calendar{};
#ifdef _WIN32
  if (local_time)
    localtime_s(&calendar, &time);
  else
    gmtime_s(&calendar, &time);
#else
  if (local_time)
    localtime_r(&time, &calendar);
  else
    gmtime_r(&time, &calendar);
#endif
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          timestamp.time_since_epoch()) %
      1000;
  std::ostringstream output;
  output << std::put_time(&calendar, local_time ? "%Y-%m-%d %H:%M:%S"
                                                : "%Y-%m-%dT%H:%M:%S")
         << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
  if (!local_time) output << 'Z';
  return output.str();
}

std::string_view ConsoleLevelName(application::LogLevel level) noexcept {
  switch (level) {
    case application::LogLevel::kTrace:
      return "TRACE";
    case application::LogLevel::kDebug:
      return "DEBUG";
    case application::LogLevel::kInfo:
      return "INFO";
    case application::LogLevel::kWarning:
      return "WARN";
    case application::LogLevel::kError:
      return "ERROR";
    case application::LogLevel::kCritical:
      return "CRITICAL";
  }
  return "UNKNOWN";
}

std::string QuoteLogValue(std::string_view text) {
  std::string result{"\""};
  for (const char character : text) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

void WriteConsoleRecord(const LogRecord& record) {
  std::osyncstream output{std::clog};
  output << '[' << FormatTimestamp(record.timestamp, true) << "] ["
         << ConsoleLevelName(record.level) << "] " << record.message;
  if (!record.correlation_id.empty())
    output << " [correlation_id=" << record.correlation_id << ']';
  output << '\n' << std::flush;
}

}  // namespace

void WriteConsoleLog(application::LogLevel level, std::string_view message,
                     std::string_view correlation_id) noexcept {
  try {
    WriteConsoleRecord({.timestamp = std::chrono::system_clock::now(),
                        .level = level,
                        .message = SanitizeText(message, kMaximumMessageBytes),
                        .correlation_id = SanitizeText(
                            correlation_id, kMaximumCorrelationIdBytes)});
  } catch (...) {
    std::fputs("[ERROR] Unable to write the console log.\n", stderr);
  }
}

class AsyncFileLogger::Impl final {
 public:
  explicit Impl(AsyncLoggerOptions options) : options_(std::move(options)) {
    if (options_.path.empty() || options_.queue_capacity == 0U ||
        options_.rotate_bytes == 0U || options_.retained_files == 0U) {
      throw std::invalid_argument("invalid async logger options");
    }
    const auto parent = options_.path.parent_path();
    std::error_code error;
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, error);
      if (error) {
        throw std::runtime_error("cannot create log directory");
      }
    }
    current_path_ = DatedPath(Now());
    OpenFile();
    worker_ = std::jthread([this] { Consume(); });
  }

  ~Impl() { Shutdown(); }

  bool Enqueue(application::LogLevel level, std::string_view message,
               std::string_view correlation_id) {
    if (level < options_.minimum_level) {
      return true;
    }
    LogRecord record{
        .timestamp = Now(),
        .level = level,
        .message = SanitizeText(message, kMaximumMessageBytes),
        .correlation_id =
            SanitizeText(correlation_id, kMaximumCorrelationIdBytes),
    };
    {
      std::lock_guard lock(mutex_);
      if (!accepting_ || queue_.size() >= options_.queue_capacity) {
        return false;
      }
      queue_.push_back(std::move(record));
    }
    condition_.notify_one();
    return true;
  }

  void Shutdown() noexcept {
    try {
      {
        std::lock_guard lock(mutex_);
        accepting_ = false;
      }
      condition_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
      output_.flush();
      output_.close();
    } catch (...) {
      // noexcept
      // 的关闭边界禁止异常进入析构链；写盘/线程失败由后续健康指标扩展承接。
      std::fputs("async logger shutdown failed\n", stderr);
    }
  }

 private:
  std::chrono::system_clock::time_point Now() const noexcept {
    return options_.clock == nullptr ? std::chrono::system_clock::now()
                                     : options_.clock->Now();
  }

  std::filesystem::path DatedPath(
      std::chrono::system_clock::time_point timestamp) const {
    auto filename = options_.path.filename().string();
    const auto marker = filename.find("{date}");
    if (marker == std::string::npos) {
      return options_.path;
    }
    const auto time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream date;
    date << std::put_time(&local, "%Y-%m-%d");
    filename.replace(marker, 6U, date.str());
    return options_.path.parent_path() / filename;
  }

  void OpenFile() {
    output_.open(current_path_, std::ios::binary | std::ios::app);
    if (!output_) {
      throw std::runtime_error("cannot open log file");
    }
    std::error_code error;
    current_size_ = std::filesystem::file_size(current_path_, error);
    if (error) {
      current_size_ = 0;
    }
  }

  void Consume() noexcept {
    while (true) {
      LogRecord record;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock,
                        [this] { return !queue_.empty() || !accepting_; });
        if (queue_.empty() && !accepting_) {
          return;
        }
        record = std::move(queue_.front());
        queue_.pop_front();
      }
      Write(record);
    }
  }

  // 函数体已经兜底全部异常；该检查会把 catch 块中的 iostream
  // 状态复位误判为逃逸。
  void Write(
      const LogRecord& record) noexcept {  // NOLINT(bugprone-exception-escape)
    try {
      std::string line = FormatTimestamp(record.timestamp) + " level=" +
                         std::string{application::LogLevelName(record.level)};
      if (!record.correlation_id.empty()) {
        line +=
            " correlation_id=" +
            (record.correlation_id.find_first_of(" =\"\\") == std::string::npos
                 ? record.correlation_id
                 : QuoteLogValue(record.correlation_id));
      }
      line += " message=" + QuoteLogValue(record.message) + '\n';
      const auto record_path = DatedPath(record.timestamp);
      if (record_path != current_path_) {
        output_.flush();
        output_.close();
        current_path_ = record_path;
        OpenFile();
      }
      if (current_size_ != 0U &&
          current_size_ + line.size() > options_.rotate_bytes) {
        Rotate();
      }
      output_ << line;
      output_.flush();
      current_size_ += line.size();
      if (options_.echo_console) {
        WriteConsoleRecord(record);
      }
    } catch (...) {
      // 日志失败不能穿透到业务线程；P1 通过 dropped_records 暴露入队失败，
      // 写盘故障将在 P5 的运行指标端口中增加独立计数和健康状态。
      output_.clear();
    }
  }

  void Rotate() {
    output_.flush();
    output_.close();
    std::error_code error;
    for (std::size_t index = options_.retained_files; index > 1U; --index) {
      const auto destination =
          current_path_.string() + "." + std::to_string(index);
      const auto source =
          current_path_.string() + "." + std::to_string(index - 1U);
      std::filesystem::remove(destination, error);
      error.clear();
      std::filesystem::rename(source, destination, error);
      error.clear();
    }
    const auto first_history = current_path_.string() + ".1";
    std::filesystem::remove(first_history, error);
    error.clear();
    std::filesystem::rename(current_path_, first_history, error);
    OpenFile();
    current_size_ = 0;
  }

  AsyncLoggerOptions options_;
  std::filesystem::path current_path_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<LogRecord> queue_;
  bool accepting_{true};
  std::jthread worker_;
  std::ofstream output_;
  std::uintmax_t current_size_{0};
};

AsyncFileLogger::AsyncFileLogger(AsyncLoggerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

AsyncFileLogger::~AsyncFileLogger() { Shutdown(); }

void AsyncFileLogger::Log(application::LogLevel level, std::string_view message,
                          std::string_view correlation_id) noexcept {
  try {
    if (!impl_ || !impl_->Enqueue(level, message, correlation_id)) {
      dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    }
  } catch (...) {
    dropped_records_.fetch_add(1U, std::memory_order_relaxed);
  }
}

std::uint64_t AsyncFileLogger::DroppedRecords() const noexcept {
  return dropped_records_.load(std::memory_order_relaxed);
}

void AsyncFileLogger::Shutdown() noexcept {
  if (impl_) {
    impl_->Shutdown();
  }
}

}  // namespace bf::infrastructure::logging
