#ifndef BUBBLEFIGHTER_SERVER_NETWORK_GIFT_HANDLER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_GIFT_HANDLER_H_

/** @file gift_handler.h @brief 原版礼物与兑换券的严格协议适配和提交后资产同步。
 */
#include "features/gifts/gift_store.h"
#include "sessions/world_executor.h"

namespace bf::infrastructure::network {
/** @brief 经过长度检查的原生请求；身份证明始终取自当前网关会话。 */
struct GiftRequest final {
  std::uint16_t opcode{0U};
  std::uint32_t id{0U};
  std::uint32_t count{0U};
  std::string recipient;
  std::string extra;
  std::string message;
};

/** @brief 返回已确认原版布局的礼物请求集合。 */
[[nodiscard]] bool IsGiftRequest(std::uint16_t opcode) noexcept;
/** @brief 严格解析原生请求，截断、超长及尾随字节抛 ProtocolError。 */
[[nodiscard]] GiftRequest DecodeGiftRequest(const protocol::Packet& packet);
/** @brief 在数据库工作池执行资产事务，随后构造原版回包及实际资产快照。 */
[[nodiscard]] application::ServiceResult<runtime::EquipmentWorldResult>
HandleGiftRequest(const GiftRequest& request, domain::PlayerId player,
                  bool allowed, application::GiftStore& gifts,
                  application::PlayerMenuSnapshotLoader& menus,
                  runtime::PurchaseOperationIdSource& operations,
                  runtime::SessionRegistry& sessions);
}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_GIFT_HANDLER_H_
