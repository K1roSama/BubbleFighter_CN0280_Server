#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_ERROR_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_ERROR_H_

/**
 * @file error.h
 * @brief 定义跨用例、运行时和协议边界传递的稳定错误模型。
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace bf::application {

/**
 * @brief 业务和运行错误的稳定分类。
 * @note
 * 具体协议错误码在协议适配层集中映射，领域和数据库代码不得写客户端魔法数字。
 */
enum class ErrorCode : std::uint8_t {
  kInvalidInput,
  kUnauthenticated,
  kPermissionDenied,
  kInvalidState,
  kInsufficientBalance,
  kConflict,
  kOverloaded,
  kStorageUnavailable,
  kInternal,
  kUnsupportedOpcode,
};

/**
 * @brief 可安全跨模块返回的错误值。
 * @note safe_message
 * 允许写入普通日志或返回给本地管理端，但不得包含密码、票据或数据库 URI。
 */
struct ServiceError final {
  ErrorCode code{ErrorCode::kInternal};  ///< 稳定错误分类。
  std::string safe_message;              ///< 已脱敏、可展示的诊断文本。
  std::string correlation_id;            ///< 可选关联标识，不是业务幂等键。
  bool is_retryable{false};  ///< 调用方能否在不改变请求语义的前提下重试。
};

/** @brief 带业务错误的值返回类型；失败值不得与正常业务结果混用。 */
template <typename Value>
using ServiceResult = std::variant<Value, ServiceError>;

/** @brief 返回用于日志、指标和测试断言的稳定英文错误名。 */
[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

/**
 * @brief 将服务错误映射成进程退出码。
 * @note 配置输入错误返回 2，存储不可用返回 3，其余失败返回
 * 1；正常退出由入口返回 0。
 */
[[nodiscard]] int ProcessExitCode(const ServiceError& error) noexcept;

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_ERROR_H_
