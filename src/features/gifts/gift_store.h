#ifndef BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_STORE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_STORE_H_

/** @file gift_store.h @brief 原生礼物与兑换券的资产事务端口。 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/error.h"
#include "application/identifier.h"
#include "application/operation_id.h"

namespace bf::application {

/** @brief 不可由客户端修改的礼物及成交规格快照。 */
struct GiftView final {
  std::uint32_t id{0U};
  domain::PlayerId sender;
  domain::PlayerId recipient;
  std::string sender_name;
  std::string recipient_name;
  std::uint32_t item{0U};
  std::uint32_t stock{0U};
  std::uint32_t quantity{0U};
  std::optional<std::int64_t> duration_seconds;
  std::int64_t created_at{0};
  std::optional<std::int64_t> processed_at;
  std::uint16_t state{0U};  ///< 0 待领取，1 已领取，2 已丢弃。
  std::string message;
  std::string thanks;
  std::uint16_t thank_kind{0U};
};

/** @brief 稳定倒序分页及当前未领取数量。 */
struct GiftPage final {
  std::uint32_t total{0U};
  std::uint32_t unread{0U};
  std::vector<GiftView> gifts;
};

/** @brief 一次服务端生成 UUID 的付费赠送；客户端重发不具备端到端幂等键。 */
struct SendGiftCommand final {
  domain::OperationId operation;
  domain::PlayerId sender;
  std::string recipient;
  domain::StockId stock;
  std::string message;
};

/** @brief 领取、丢弃、隐藏历史和感谢的独立业务动作。 */
enum class GiftAction : std::uint8_t {
  kClaim,
  kDiscard,
  kHideReceived,
  kHideSent,
  kThank
};

/** @brief 已提交的礼物动作及更新后的未领取数。 */
struct GiftActionResult final {
  GiftView gift;
  std::uint32_t unread{0U};
  bool changed{false};
};

/** @brief 兑换券实际提交的资产；再次兑换返回相同结果但 changed 为 false。 */
struct CouponResult final {
  std::uint32_t currency_amount{0U};
  std::uint32_t item_count{0U};
  bool changed{false};
};

/** @brief 赠送扣款、礼物领取和兑换券核销均须在单个事务内完成。 */
class GiftStore {
 public:
  virtual ~GiftStore() = default;
  [[nodiscard]] virtual ServiceResult<GiftPage> List(domain::PlayerId player,
                                                     bool sent,
                                                     std::uint32_t offset,
                                                     std::uint32_t count) = 0;
  [[nodiscard]] virtual ServiceResult<GiftView> Send(
      const SendGiftCommand& command) = 0;
  [[nodiscard]] virtual ServiceResult<GiftView> Detail(domain::PlayerId player,
                                                       std::uint32_t gift) = 0;
  [[nodiscard]] virtual ServiceResult<GiftActionResult> Act(
      domain::PlayerId player, std::uint32_t gift, GiftAction action,
      const std::string& thanks, std::uint16_t thank_kind = 0U) = 0;
  [[nodiscard]] virtual ServiceResult<CouponResult> Redeem(
      domain::PlayerId player, const std::string& code) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_GIFTS_GIFT_STORE_H_
