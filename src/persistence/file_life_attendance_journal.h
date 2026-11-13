#ifndef BUBBLEFIGHTER_SERVER_PERSISTENCE_FILE_LIFE_ATTENDANCE_JOURNAL_H_
#define BUBBLEFIGHTER_SERVER_PERSISTENCE_FILE_LIFE_ATTENDANCE_JOURNAL_H_

/** @file file_life_attendance_journal.h
 * @brief 绑定数据库纪元和大区的原子出席日志，支持进程异常退出恢复。
 */
#include <filesystem>
#include <memory>
#include <string>

#include "features/inventory/life_attendance_journal.h"

namespace bf::infrastructure::storage {
/** @brief 独占文件锁、SHA256校验、双文件代次恢复及耐久原子替换。 */
class FileLifeAttendanceJournal final
    : public application::LifeAttendanceJournal {
 public:
  /** @brief 校验并独占日志；损坏、跨库或跨区文件不会被忽略或清空。 */
  [[nodiscard]] static application::ServiceResult<
      std::unique_ptr<FileLifeAttendanceJournal>>
  Open(const std::filesystem::path& directory, std::string epoch,
       std::uint32_t region);
  ~FileLifeAttendanceJournal();
  FileLifeAttendanceJournal(const FileLifeAttendanceJournal&) = delete;
  FileLifeAttendanceJournal& operator=(const FileLifeAttendanceJournal&) =
      delete;
  [[nodiscard]] application::ServiceResult<
      std::vector<application::LifeAttendanceObservation>>
  Pending() override;
  [[nodiscard]] std::optional<application::ServiceError> Append(
      std::span<const application::LifeAttendanceObservation> rows) override;
  [[nodiscard]] std::optional<application::ServiceError> Acknowledge(
      std::span<const application::LifeAttendanceObservation> rows) override;

 private:
  /** @brief 平台文件句柄与同步状态只由此实例拥有。 */
  class Impl;
  explicit FileLifeAttendanceJournal(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace bf::infrastructure::storage

#endif  // BUBBLEFIGHTER_SERVER_PERSISTENCE_FILE_LIFE_ATTENDANCE_JOURNAL_H_
