#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_EVENT_LOGGER_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_EVENT_LOGGER_H_

/**
 * @file event_logger.h
 * @brief 定义业务和运行时可依赖的日志端口，避免反向依赖具体日志库。
 */

#include <cstdint>
#include <string_view>

namespace bf::application {

/** @brief 日志严重级别；顺序也用于最低级别过滤。 */
enum class LogLevel : std::uint8_t {
  kTrace,
  kDebug,
  kInfo,
  kWarning,
  kError,
  kCritical,
};

/**
 * @brief 接收已经过调用方语义筛选的服务事件。
 * @note 实现必须是线程安全且 noexcept；队列过载时允许丢弃普通日志并暴露计数。
 * @warning message 和 correlation_id 不得包含密码、登录票据或数据库连接 URI。
 */
class EventLogger {
 public:
  virtual ~EventLogger() = default;

  /**
   * @brief 记录一条结构受限的事件。
   * @param level 严重级别。
   * @param message 不含换行控制字符和秘密的功能描述；实现仍会处理日志注入字符。
   * @param correlation_id 可选请求关联标识，不得用认证票据代替。
   */
  virtual void Log(LogLevel level, std::string_view message,
                   std::string_view correlation_id = {}) noexcept = 0;
};

/** @brief 将日志级别转换为稳定英文名称。 */
[[nodiscard]] std::string_view LogLevelName(LogLevel level) noexcept;

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_EVENT_LOGGER_H_
