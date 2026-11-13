#ifndef BUBBLEFIGHTER_SERVER_APPLICATION_IDENTIFIER_H_
#define BUBBLEFIGHTER_SERVER_APPLICATION_IDENTIFIER_H_

/**
 * @file identifier.h
 * @brief 定义跨模块使用的强类型业务标识，防止不同 ID 在编译期被误传。
 */

#include <compare>
#include <cstdint>
#include <string>
#include <type_traits>

namespace bf::domain {

/**
 * @brief 保存一种业务实体的无符号 64 位标识。
 * @tparam Tag 仅用于区分实体种类的标签类型。
 * @note 不提供与整数或其他实体标识的隐式转换；协议层与持久化层必须显式取值。
 */
template <typename Tag>
class Identifier final {
 public:
  /** @brief 持久化与协议边界使用的无符号底层数值类型。 */
  using ValueType = std::uint64_t;

  /** @brief 创建无效的零值标识，供反序列化结果初始化使用。 */
  constexpr Identifier() noexcept = default;

  /**
   * @brief 从已经完成范围校验的持久化或协议值创建标识。
   * @param value 原始无符号值；零值保留为“未分配”。
   */
  explicit constexpr Identifier(ValueType value) noexcept : value_(value) {}

  /** @brief 返回边界层需要序列化的原始值。 */
  [[nodiscard]] constexpr ValueType Value() const noexcept { return value_; }

  /** @brief 判断标识是否已经分配了非零值。 */
  [[nodiscard]] constexpr bool IsValid() const noexcept { return value_ != 0U; }

  /** @brief 按同一实体类型的无符号原始值进行全序比较。 */
  auto operator<=>(const Identifier&) const = default;

 private:
  ValueType value_{0};
};

/** @brief 登录账号实体的类型标签。 */
struct AccountIdTag final {};

/** @brief 游戏玩家资料实体的类型标签。 */
struct PlayerIdTag final {};

/** @brief 物品模板实体的类型标签。 */
struct ItemIdTag final {};

/** @brief 商城销售规格实体的类型标签。 */
struct StockIdTag final {};

/** @brief 玩家持有物品实例的类型标签。 */
struct ItemInstanceIdTag final {};

/** @brief 购买订单实体的类型标签。 */
struct OrderIdTag final {};

/** @brief 私聊历史消息实体的类型标签。 */
struct MessageIdTag final {};

/** @brief 房间实体的类型标签。 */
struct RoomIdTag final {};

/** @brief 账号实体的强类型标识。 */
using AccountId = Identifier<AccountIdTag>;
/** @brief 玩家资料实体的强类型标识。 */
using PlayerId = Identifier<PlayerIdTag>;
/** @brief 物品模板实体的强类型标识。 */
using ItemId = Identifier<ItemIdTag>;
/** @brief 商城销售规格的强类型标识。 */
using StockId = Identifier<StockIdTag>;
/** @brief 玩家库存实例的强类型标识。 */
using ItemInstanceId = Identifier<ItemInstanceIdTag>;
/** @brief 购买订单实体的强类型标识。 */
using OrderId = Identifier<OrderIdTag>;
/** @brief 私聊历史消息的强类型标识。 */
using MessageId = Identifier<MessageIdTag>;
/** @brief 进程内房间实体的强类型标识。 */
using RoomId = Identifier<RoomIdTag>;

static_assert(!std::is_same_v<AccountId, PlayerId>);
static_assert(!std::is_convertible_v<AccountId, PlayerId>);

/** @brief 将账号标识转换成不含前缀的十进制文本，主要用于日志元数据。 */
[[nodiscard]] std::string ToString(AccountId value);
/** @brief 将玩家标识转换成不含前缀的十进制文本，主要用于日志元数据。 */
[[nodiscard]] std::string ToString(PlayerId value);
/** @brief 将房间标识转换成不含前缀的十进制文本，主要用于日志元数据。 */
[[nodiscard]] std::string ToString(RoomId value);

}  // namespace bf::domain

#endif  // BUBBLEFIGHTER_SERVER_APPLICATION_IDENTIFIER_H_
