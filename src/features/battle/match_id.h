#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_ID_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_ID_H_

/**
 * @file match_id.h
 * @brief 定义持久化对局使用的强类型 UUID，防止与购买等操作 UUID 混用。
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bf::domain {

/** @brief 一场对局从开局、终局到结算重试始终不变的 128 位标识。 */
class MatchId final {
 public:
  /** @brief UUID 原始网络无关字节。 */
  using ByteArray = std::array<std::uint8_t, 16U>;

  /** @brief 从非全零 16 字节值创建；全零表示未分配并返回空。 */
  [[nodiscard]] static std::optional<MatchId> FromBytes(
      ByteArray bytes) noexcept;
  /** @brief 解析标准 8-4-4-4-12 UUID 文本；非法或全零时返回空。 */
  [[nodiscard]] static std::optional<MatchId> FromUuidString(
      std::string_view text) noexcept;

  /** @brief 返回数据库 UUID adapter 使用的原始字节。 */
  [[nodiscard]] const ByteArray& Bytes() const noexcept;

  /** @brief 返回小写、带连字符的规范 UUID 文本。 */
  [[nodiscard]] std::string ToUuidString() const;

  /** @brief 按完整 16 字节 UUID 比较。 */
  bool operator==(const MatchId&) const = default;

 private:
  /** @brief 只允许工厂在完成非零检查后构造。 */
  explicit MatchId(ByteArray bytes) noexcept;

  ByteArray bytes_{};  ///< 不可变 UUID 内容。
};

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_ID_H_
