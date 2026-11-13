#include "application/event_logger.h"

namespace bf::application {

std::string_view LogLevelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return "trace";
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarning:
      return "warning";
    case LogLevel::kError:
      return "error";
    case LogLevel::kCritical:
      return "critical";
  }
  return "unknown";
}

}  // namespace bf::application
