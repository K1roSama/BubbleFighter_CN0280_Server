#ifndef BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_COUPLE_SERVICE_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_COUPLE_SERVICE_H_

/**
 * @file couple_service.h
 * @brief 情侣持久化命令与已核实的原版戒指映射。
 */

#include <chrono>
#include <string>

#include "application/error.h"
#include "application/identifier.h"

namespace bf::application {

// ShopItem(function=17).parameter -> Stock.itemId in original ShopItem2.xml.
[[nodiscard]] constexpr std::uint32_t CoupleDisplayRing(
    std::uint32_t invitation_ring) noexcept {
  switch (invitation_ring) {
    case 100604U:
      return 100605U;
    case 100984U:
      return 100985U;
    case 101034U:
      return 101035U;
    case 101395U:
      return 101396U;
    case 101560U:
      return 101561U;
    default:
      return 0U;
  }
}

/** @brief 事务提交后的双方身份与戒指变化。 */
struct CoupleChange final {
  domain::PlayerId actor;
  domain::PlayerId partner;
  std::string actor_name;
  std::string partner_name;
  std::uint32_t ring{0U};
  bool changed{false};
};

/** @brief 绑定发起者、接收者当前昵称与权威时间的邀请命令。 */
struct CoupleFormCommand final {
  domain::PlayerId actor;
  domain::PlayerId partner;
  std::string actor_name;
  std::string partner_name;
  std::uint32_t invitation_ring{0U};
  std::chrono::system_clock::time_point now;
};

/** @brief 原子建立、解除和变更情侣关系的持久化端口。 */
class CoupleStore {
 public:
  virtual ~CoupleStore() = default;
  [[nodiscard]] virtual std::optional<ServiceError> CheckInvitation(
      domain::PlayerId actor, domain::PlayerId partner) = 0;
  // These calls lock both profiles in ID order, recheck exclusivity/blocks,
  // change both inventories and the relationship in one transaction.
  [[nodiscard]] virtual ServiceResult<CoupleChange> Form(
      const CoupleFormCommand& command) = 0;
  [[nodiscard]] virtual ServiceResult<CoupleChange> Breakup(
      domain::PlayerId actor, domain::PlayerId expected_partner) = 0;
  [[nodiscard]] virtual ServiceResult<CoupleChange> ChangeRing(
      const CoupleFormCommand& command) = 0;
};
}  // namespace bf::application

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_SOCIAL_COUPLE_SERVICE_H_
