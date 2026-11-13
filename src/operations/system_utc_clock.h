#ifndef BUBBLEFIGHTER_SERVER_OPERATIONS_SYSTEM_UTC_CLOCK_H_
#define BUBBLEFIGHTER_SERVER_OPERATIONS_SYSTEM_UTC_CLOCK_H_

/**
 * @file system_utc_clock.h
 * @brief 定义生产进程使用的 UTC 系统时钟适配器。
 */

#include "application/system_clock.h"

namespace bf::infrastructure::system {

/** @brief 直接读取 C++ `system_clock` 的无状态生产时钟。 */
class SystemUtcClock final : public application::SystemClock {
 public:
  /** @brief 返回当前 UTC 时间点，不进行协议精度截断。 */
  [[nodiscard]] std::chrono::system_clock::time_point Now()
      const noexcept override {
    return std::chrono::system_clock::now();
  }
};

}  // namespace bf::infrastructure::system

#endif  // BUBBLEFIGHTER_SERVER_OPERATIONS_SYSTEM_UTC_CLOCK_H_
