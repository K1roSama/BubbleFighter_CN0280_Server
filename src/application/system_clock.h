#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_SYSTEM_CLOCK_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_SYSTEM_CLOCK_H_

/**
 * @file system_clock.h
 * @brief 定义有时效业务共用、可替换测试的 UTC 系统时钟端口。
 */

#include <chrono>

namespace bf::application {

/** @brief 为票据、库存期限和事务时间提供 UTC 时间的同步端口。 */
class SystemClock {
 public:
  virtual ~SystemClock() = default;

  /** @brief 返回当前 UTC 时间点；实现不得使用会回绕的协议计时器。 */
  [[nodiscard]] virtual std::chrono::system_clock::time_point Now()
      const noexcept = 0;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_SYSTEM_CLOCK_H_
