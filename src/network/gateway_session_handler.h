#ifndef BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_SESSION_HANDLER_H_
#define BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_SESSION_HANDLER_H_

/**
 * @file gateway_session_handler.h
 * @brief 定义把同步网关用例安全调度到有界数据库工作池的 Session handler 工厂。
 */

#include "application/background_task_executor.h"
#include "application/system_clock.h"
#include "features/authentication/launcher_authentication.h"
#include "features/authentication/world_access.h"
#include "features/characters/character_creation.h"
#include "features/characters/level_icon.h"
#include "features/characters/native_title.h"
#include "features/chat/chat_blocks.h"
#include "features/clans/clan_service.h"
#include "features/gifts/gift_store.h"
#include "features/inventory/equipment_service.h"
#include "features/inventory/item_use_service.h"
#include "features/inventory/key_map.h"
#include "features/inventory/life_attendance_tracker.h"
#include "features/lobby/lobby_minigame.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/shop/mystery_shop_service.h"
#include "features/shop/purchase_service.h"
#include "features/shop/shop_catalog.h"
#include "features/shop/treasure_service.h"
#include "features/shop/workshop_service.h"
#include "features/social/social_query.h"
#include "features/social/social_service.h"
#include "network/tcp_server.h"
#include "sessions/clan_ticket_broker.h"
#include "sessions/couple_flow.h"
#include "sessions/gateway_login_flow.h"
#include "sessions/session_registry.h"
#include "sessions/social_spaces_flow.h"
#include "sessions/world_executor.h"

namespace bf::infrastructure::network {

/** @brief 在进程内按来源地址限制启动器密码认证尝试频率。 */
class LauncherAuthenticationRateLimiter;

/**
 * @brief 为每个 TCP Session 创建独立 `GatewayLoginFlow` 并隔离阻塞数据库调用。
 * @note 注入的服务和工作池由 composition root 拥有，必须晚于 TCP server
 * 停止和任务池排空。
 */
class GatewaySessionHandlerFactory final : public SessionPacketHandlerFactory {
 public:
  /** @brief 保存共享、线程安全的应用服务引用和当前网关广告端点。 */
  GatewaySessionHandlerFactory(
      application::BackgroundTaskExecutor& database_tasks,
      application::LoginTicketConsumer& ticket_consumer,
      application::RegisterAccountService& account_registration,
      application::LoginWithPasswordService& password_login,
      application::CharacterCreationUseCases& character_creation,
      application::PlayerMenuSnapshotLoader& menu_snapshots,
      application::ShopCatalogLoader& shop_catalog,
      application::PurchaseExecutor& purchases,
      application::EquipmentChanger& equipment,
      application::SocialSnapshotLoader& social_snapshots,
      application::FriendRelationshipChanger& social_relationships,
      application::PrivateMessageSender& private_messages,
      application::ServiceRulesLoader& service_rules,
      application::TreasureExecutor& treasure,
      application::MysteryShopExecutor& mystery_shop,
      application::WorkshopExecutor& workshop, application::ClanExecutor& clans,
      application::ItemUseExecutor& item_use,
      application::WorldAccessLoader& world_access,
      runtime::WorldExecutor& world_executor,
      runtime::PurchaseOperationIdSource& purchase_operation_ids,
      runtime::SessionRegistry& session_registry,
      runtime::ClanTicketBroker& clan_tickets,
      const application::SystemClock& clock, application::EventLogger& logger,
      protocol::GatewayEndpoint gateway_endpoint,
      application::DailyLoginRewardGranter* daily_rewards = nullptr,
      bool allow_launcher_authentication = true,
      application::KeyMapStore* key_maps = nullptr,
      application::GiftStore* gifts = nullptr,
      application::LevelIconStore* level_icons = nullptr,
      runtime::CoupleFlow* couples = nullptr,
      runtime::SocialSpacesFlow* social_spaces = nullptr,
      application::ChatBlockStore* chat_blocks = nullptr,
      runtime::LobbyMinigameService* lobby_minigames = nullptr,
      application::NativeTitleStore* titles = nullptr,
      runtime::LifeAttendanceTracker* life_attendance = nullptr) noexcept;

  /** @brief 接管 Session 专属在线 registry 并创建非空异步处理器。 */
  [[nodiscard]] application::ServiceResult<
      std::unique_ptr<SessionPacketHandler>>
  Create(SessionHandlerContext context) override;

 private:
  application::BackgroundTaskExecutor& database_tasks_;
  application::LoginTicketConsumer& ticket_consumer_;
  application::DailyLoginRewardGranter* daily_rewards_;
  application::RegisterAccountService& account_registration_;
  application::LoginWithPasswordService& password_login_;
  application::CharacterCreationUseCases& character_creation_;
  application::PlayerMenuSnapshotLoader& menu_snapshots_;
  application::ShopCatalogLoader& shop_catalog_;
  application::PurchaseExecutor& purchases_;
  application::EquipmentChanger& equipment_;
  application::SocialSnapshotLoader& social_snapshots_;
  application::FriendRelationshipChanger& social_relationships_;
  application::PrivateMessageSender& private_messages_;
  application::ServiceRulesLoader& service_rules_;
  application::TreasureExecutor& treasure_;
  application::MysteryShopExecutor& mystery_shop_;
  application::WorkshopExecutor& workshop_;
  application::ClanExecutor& clans_;
  application::ItemUseExecutor& item_use_;
  application::WorldAccessLoader& world_access_;
  runtime::WorldExecutor& world_executor_;
  runtime::PurchaseOperationIdSource& purchase_operation_ids_;
  runtime::SessionRegistry& session_registry_;
  runtime::ClanTicketBroker& clan_tickets_;
  const application::SystemClock& clock_;
  application::EventLogger& logger_;
  protocol::GatewayEndpoint gateway_endpoint_;
  std::shared_ptr<LauncherAuthenticationRateLimiter> launcher_rate_limiter_;
  application::GiftStore* gifts_;
  application::LevelIconStore* level_icons_;
  runtime::CoupleFlow* couples_;
  runtime::SocialSpacesFlow* social_spaces_;
  application::ChatBlockStore*
      chat_blocks_;  ///< 可选：独立聊天屏蔽名单（147/148/65030）。
  runtime::LobbyMinigameService*
      lobby_minigames_;  ///< 可选：广场钓鱼/投篮/占卜（571/572）。
  application::NativeTitleStore* titles_;  ///< 原931兑换及932本人称号穿戴事务。
  application::KeyMapStore*
      key_maps_;  ///< 可选以保留测试工厂兼容；生产组合根始终注入。
  runtime::LifeAttendanceTracker*
      life_attendance_;  ///< 生产组合根总是注入真实Life出席结算。
};

}  // namespace bf::infrastructure::network

#endif  // BUBBLEFIGHTER_SERVER_NETWORK_GATEWAY_SESSION_HANDLER_H_
