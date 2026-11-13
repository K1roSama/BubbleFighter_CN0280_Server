#ifndef BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_CREDENTIAL_CRYPTO_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_CREDENTIAL_CRYPTO_H_

/**
 * @file credential_crypto.h
 * @brief 定义密码派生和密码学随机源的应用端口与固定兼容参数。
 */

#include <array>
#include <cstdint>
#include <string_view>

#include "application/error.h"

namespace bf::application {

/** @brief 当前 Python 启动器兼容的 PBKDF2-HMAC-SHA256 迭代次数。 */
inline constexpr std::uint32_t kPbkdf2Sha256Iterations = 600'000U;
/** @brief MySQL 凭据行允许的最小迭代次数，低于此值视为损坏或过时。 */
inline constexpr std::uint32_t kMinimumAcceptedPbkdf2Iterations = 100'000U;
/** @brief 防止损坏凭据触发无界 CPU 消耗的最大迭代次数。 */
inline constexpr std::uint32_t kMaximumAcceptedPbkdf2Iterations = 10'000'000U;
/** @brief 数据库持久化使用的当前密码算法稳定标识。 */
inline constexpr std::string_view kPbkdf2Sha256Algorithm = "pbkdf2_sha256";

/** @brief 每个账号独立生成的 16 字节密码盐。 */
using CredentialSalt = std::array<std::uint8_t, 16U>;
/** @brief PBKDF2-HMAC-SHA256 的 32 字节派生结果。 */
using PasswordDigest = std::array<std::uint8_t, 32U>;
/** @brief 登录票据正文中编码为 32 个小写十六进制字符的 16 字节随机量。 */
using LoginTicketSecret = std::array<std::uint8_t, 16U>;

/**
 * @brief PBKDF2-HMAC-SHA256 密码端口。
 * @note 正式实现必须使用受维护密码库，并在 verify 中使用恒定时间摘要比较。
 */
class Pbkdf2PasswordHasher {
 public:
  virtual ~Pbkdf2PasswordHasher() = default;

  /** @brief 从 UTF-8 密码、16 字节盐和正迭代次数派生 32 字节摘要。 */
  [[nodiscard]] virtual ServiceResult<PasswordDigest> Derive(
      std::string_view password, const CredentialSalt& salt,
      std::uint32_t iterations) const = 0;

  /** @brief 重新派生并以恒定时间比较已存摘要；返回前清理临时派生结果。 */
  [[nodiscard]] virtual ServiceResult<bool> Verify(
      std::string_view password, const CredentialSalt& salt,
      std::uint32_t iterations, const PasswordDigest& expected) const = 0;
};

/** @brief 操作系统密码学随机源端口；普通伪随机数不得用于密码盐或登录票据。 */
class CryptographicRandomSource {
 public:
  virtual ~CryptographicRandomSource() = default;

  /** @brief 为新密码凭据生成独立的 16 字节随机盐。 */
  [[nodiscard]] virtual ServiceResult<CredentialSalt> GenerateCredentialSalt()
      const = 0;

  /** @brief 为五分钟单次登录票据生成 128 位随机正文。 */
  [[nodiscard]] virtual ServiceResult<LoginTicketSecret>
  GenerateLoginTicketSecret() const = 0;
};

}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_AUTHENTICATION_CREDENTIAL_CRYPTO_H_
