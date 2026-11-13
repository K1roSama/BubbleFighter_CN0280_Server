#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_OPERATION_ID_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_OPERATION_ID_H_

/**
 * @file operation_id.h
 * @brief 定义资产事务内部重试使用的强类型 UUID 操作标识。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace bf::domain {

/**
 * @brief 一次具名业务操作的 128 位稳定标识。
 * @note 此标识支持服务端内部重试和提交结果查询；在 C2S809
 * 没有端到端键的情况下，不承诺 客户端断线后重新发送能复用同一标识。
 */
class OperationId final {
 public:
  /** @brief UUID 原始网络无关字节。 */
  using ByteArray = std::array<std::uint8_t, 16U>;

  /** @brief 从非全零的 16 字节值创建操作标识；全零保留为无效并返回空。 */
  [[nodiscard]] static std::optional<OperationId> FromBytes(
      ByteArray bytes) noexcept;

  /** @brief 返回数据库 UUID adapter 需要的 16 字节值。 */
  [[nodiscard]] const ByteArray& Bytes() const noexcept;

  /** @brief 返回小写、带连字符的规范 UUID 文本，供 SQL 参数和审计元数据使用。
   */
  [[nodiscard]] std::string ToUuidString() const;

  /** @brief 按完整 16 字节 UUID 比较。 */
  bool operator==(const OperationId&) const = default;

 private:
  explicit OperationId(ByteArray bytes) noexcept;

  ByteArray bytes_{};
};

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_OPERATION_ID_H_
