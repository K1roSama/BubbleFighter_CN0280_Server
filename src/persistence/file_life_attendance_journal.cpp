/** @file file_life_attendance_journal.cpp
 * @brief 以写入刷新、原子替换和校验过的代次恢复保存出席事实。
 */
#include "persistence/file_life_attendance_journal.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

#include "crypto/openssl_sha256.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace bf::infrastructure::storage {
namespace {
using Observation = application::LifeAttendanceObservation;
using Rows = std::set<Observation>;
constexpr std::size_t kHeader = 96;
constexpr std::size_t kMaximumRows = 1000000;
constexpr std::array<std::uint8_t, 8> kMagic{'B', 'F', 'L', 'I',
                                             'F', 'E', '0', '2'};

application::ServiceError Failure() {
  return {.code = application::ErrorCode::kStorageUnavailable,
          .safe_message =
              "出席耐久日志无法确认，请保留日志文件并检查存储；未继续Life结算",
          .is_retryable = false};
}

void Require(bool value) {
  if (!value) throw std::runtime_error{"attendance journal invalid"};
}

bool ValidEpoch(std::string_view epoch) {
  if (epoch.size() != 36) return false;
  for (std::size_t i = 0; i < epoch.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (epoch[i] != '-') return false;
    } else if (!((epoch[i] >= '0' && epoch[i] <= '9') ||
                 (epoch[i] >= 'a' && epoch[i] <= 'f')))
      return false;
  }
  return true;
}

void Put(std::vector<std::uint8_t>& bytes, std::uint64_t value,
         std::size_t count = 8) {
  for (std::size_t i = 0; i < count; ++i)
    bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

std::uint64_t Get(std::span<const std::uint8_t> bytes, std::size_t offset,
                  std::size_t count = 8) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < count; ++i)
    value |= std::uint64_t{bytes[offset + i]} << (i * 8);
  return value;
}

application::Sha256Digest Hash(std::span<const std::uint8_t> bytes) {
  const auto result = security::OpenSslSha256{}.Hash(bytes);
  Require(std::holds_alternative<application::Sha256Digest>(result));
  return std::get<application::Sha256Digest>(result);
}

struct Snapshot {
  std::uint64_t generation{};
  Rows rows;
};

std::vector<std::uint8_t> Encode(const Snapshot& value, std::string_view epoch,
                                 std::uint32_t region) {
  Require(value.rows.size() <= kMaximumRows);
  std::vector<std::uint8_t> bytes(kMagic.begin(), kMagic.end());
  bytes.insert(bytes.end(), epoch.begin(), epoch.end());
  Put(bytes, region, 4);
  Put(bytes, value.generation);
  Put(bytes, value.rows.size());
  for (const auto& row : value.rows) {
    Require(row.player.IsValid() &&
            row.player.Value() <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) &&
            row.day.time_since_epoch().count() >= 0 &&
            row.day.time_since_epoch().count() <=
                std::numeric_limits<std::int32_t>::max());
    Put(bytes, row.player.Value());
    Put(bytes, static_cast<std::uint64_t>(row.day.time_since_epoch().count()));
  }
  const auto digest = Hash(bytes);
  bytes.insert(bytes.begin() + 64, digest.begin(), digest.end());
  return bytes;
}

Snapshot Read(const std::filesystem::path& path, std::string_view epoch,
              std::uint32_t region) {
  const auto length = std::filesystem::file_size(path);
  Require(length >= kHeader && length <= kHeader + kMaximumRows * 16 &&
          (length - kHeader) % 16 == 0);
  std::ifstream input{path, std::ios::binary};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  Require(static_cast<bool>(
      input.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))));
  Require(std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) &&
          std::equal(epoch.begin(), epoch.end(), bytes.begin() + 8) &&
          Get(bytes, 44, 4) == region &&
          Get(bytes, 56) == (length - kHeader) / 16);
  const std::vector<std::uint8_t> expected(bytes.begin() + 64,
                                           bytes.begin() + 96);
  bytes.erase(bytes.begin() + 64, bytes.begin() + 96);
  const auto digest = Hash(bytes);
  Require(std::equal(expected.begin(), expected.end(), digest.begin()));
  Snapshot value{.generation = Get(bytes, 48)};
  for (std::size_t offset = 64; offset < bytes.size(); offset += 16) {
    const auto player = Get(bytes, offset), day = Get(bytes, offset + 8);
    Require(player > 0 &&
            player <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()) &&
            day <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int32_t>::max()));
    Require(value.rows
                .insert({domain::PlayerId{player},
                         std::chrono::sys_days{std::chrono::days{
                             static_cast<std::int64_t>(day)}}})
                .second);
  }
  return value;
}

// The lock is a separate stable inode: replacing the snapshot never releases
// ownership.
class File final {
 public:
  File(const std::filesystem::path& path, bool lock) {
#ifdef _WIN32
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, lock ? OPEN_ALWAYS : CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle_ != INVALID_HANDLE_VALUE);
#else
    handle_ = ::open(path.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | (lock ? 0 : O_TRUNC), 0600);
    require(handle_ >= 0);
    if (lock && ::flock(handle_, LOCK_EX | LOCK_NB) != 0) {
      ::close(handle_);
      handle_ = -1;
      require(false);
    }
#endif
  }

  ~File() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (handle_ >= 0) ::close(handle_);
#endif
  }

  File(const File&) = delete;
  File& operator=(const File&) = delete;

  void Write(std::span<const std::uint8_t> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
      DWORD written = 0;
      Require(WriteFile(handle_, data.data() + offset,
                        static_cast<DWORD>(data.size() - offset), &written,
                        nullptr) != 0 &&
              written > 0);
      offset += written;
#else
      const auto written =
          ::write(handle_, data.data() + offset, data.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      require(written > 0);
      offset += static_cast<std::size_t>(written);
#endif
    }
#ifdef _WIN32
    Require(FlushFileBuffers(handle_) != 0);
#else
    require(::fsync(handle_) == 0);
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int handle_{-1};
#endif
};

void Replace(const std::filesystem::path& pending,
             const std::filesystem::path& target) {
#ifdef _WIN32
  Require(MoveFileExW(pending.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
#else
  require(::rename(pending.c_str(), target.c_str()) == 0);
  const int directory =
      ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(directory >= 0);
  const bool synced = ::fsync(directory) == 0;
  ::close(directory);
  require(synced);
#endif
}
}  // namespace

class FileLifeAttendanceJournal::Impl final {
 public:
  Impl(std::filesystem::path directory, std::string epoch, std::uint32_t region)
      : epoch_(std::move(epoch)), region_(region) {
    Require(ValidEpoch(epoch_) && region_ > 0 && !directory.empty());
    std::filesystem::create_directories(directory);
    path_ =
        directory / ("life-" + epoch_ + "-" + std::to_string(region_) + ".bin");
    pending_ = path_;
    pending_ += ".pending";
    auto lock = path_;
    lock += ".lock";
    lock_ = std::make_unique<File>(lock, true);
    const bool existing = std::filesystem::exists(path_);
    if (existing) current_ = Read(path_, epoch_, region_);
    if (std::filesystem::exists(pending_)) {
      auto next = Read(pending_, epoch_, region_);
      Require(!existing || next.generation == current_.generation + 1);
      Replace(pending_, path_);
      current_ = std::move(next);
    } else if (!existing)
      Save(current_);
  }

  std::optional<application::ServiceError> Change(
      std::span<const Observation> rows, bool adding) {
    std::lock_guard lock{mutex_};
    if (poisoned_) return Failure();
    try {
      auto next = current_;
      for (const auto& row : rows) {
        Require(row.player.IsValid() &&
                row.player.Value() <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) &&
                row.day.time_since_epoch().count() >= 0 &&
                row.day.time_since_epoch().count() <=
                    std::numeric_limits<std::int32_t>::max());
        if (adding)
          next.rows.insert(row);
        else
          next.rows.erase(row);
      }
      if (next.rows == current_.rows) return std::nullopt;
      Require(next.generation < std::numeric_limits<std::uint64_t>::max());
      ++next.generation;
      Save(next);
      current_ = std::move(next);
      return std::nullopt;
    } catch (...) {
      poisoned_ = true;
      return Failure();
    }
  }

  application::ServiceResult<std::vector<Observation>> Pending() {
    std::lock_guard lock{mutex_};
    if (poisoned_) return Failure();
    return std::vector<Observation>{current_.rows.begin(), current_.rows.end()};
  }

 private:
  void Save(const Snapshot& value) {
    const auto bytes = Encode(value, epoch_, region_);
    {
      File output{pending_, false};
      output.Write(bytes);
    }
    Replace(pending_, path_);
  }

  std::string epoch_;
  std::uint32_t region_;
  std::filesystem::path path_, pending_;
  std::unique_ptr<File> lock_;
  std::mutex mutex_;
  Snapshot current_;
  bool poisoned_{false};
};

FileLifeAttendanceJournal::FileLifeAttendanceJournal(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

FileLifeAttendanceJournal::~FileLifeAttendanceJournal() = default;

application::ServiceResult<std::unique_ptr<FileLifeAttendanceJournal>>
FileLifeAttendanceJournal::Open(const std::filesystem::path& directory,
                                std::string epoch, std::uint32_t region) {
  try {
    return std::unique_ptr<FileLifeAttendanceJournal>{
        new FileLifeAttendanceJournal{
            std::make_unique<Impl>(directory, std::move(epoch), region)}};
  } catch (...) {
    return Failure();
  }
}

application::ServiceResult<std::vector<Observation>>
FileLifeAttendanceJournal::Pending() {
  return impl_->Pending();
}

std::optional<application::ServiceError> FileLifeAttendanceJournal::Append(
    std::span<const Observation> rows) {
  return impl_->Change(rows, true);
}

std::optional<application::ServiceError> FileLifeAttendanceJournal::Acknowledge(
    std::span<const Observation> rows) {
  return impl_->Change(rows, false);
}
}  // namespace bf::infrastructure::storage
