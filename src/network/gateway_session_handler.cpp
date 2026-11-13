#include "network/gateway_session_handler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <variant>

#include "features/authentication/account_credentials.h"
#include "features/inventory/clothing_set.h"
#include "network/gateway_opcode_routes.h"
#include "network/gift_handler.h"
#include "network/launcher_authentication_handler.h"
#include "protocol/battle/combat_messages.h"
#include "protocol/battle/native_progression_messages.h"
#include "protocol/battle/special_mode_messages.h"
#include "protocol/characters/fighter_ranking_messages.h"
#include "protocol/chat/chat_block_messages.h"
#include "protocol/chat/chat_messages.h"
#include "protocol/clans/clan_messages.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/inventory/key_map_messages.h"
#include "protocol/lobby/lobby_minigame_messages.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/login/gateway_messages.h"
#include "protocol/rooms/room_messages.h"
#include "protocol/shop/mystery_shop_messages.h"
#include "protocol/shop/treasure_messages.h"
#include "protocol/shop/workshop_messages.h"

namespace bf::infrastructure::network {

namespace {

constexpr std::uint16_t kLauncherAuthenticationRequestOpcode = 65020U;

bool IsTreasureOpcode(std::uint16_t opcode) noexcept {
  return opcode >= 917U && opcode <= 920U;
}

bool IsWorkshopActionOpcode(std::uint16_t opcode) noexcept {
  return opcode >= 132U && opcode <= 137U;
}

protocol::EventZoneConfig ClosedEventZoneConfiguration() {
  const auto now = std::chrono::system_clock::now();
  const auto local_days =
      std::chrono::floor<std::chrono::days>(now + std::chrono::hours{8})
          .time_since_epoch()
          .count();
  const double today = static_cast<double>(local_days) + 25569.0;
  return {.open = false,
          .wait_ms = 3000U,
          .reserved = 0U,
          .day = today,
          .start = today,
          .end = today,
          .daily_open = 0U,
          .daily_close = 0U};
}

std::vector<std::uint8_t> EventZoneDynamicSection(
    std::uint16_t requested_mask) {
  if ((requested_mask & (1U << 6U)) == 0U) return {};
  return protocol::EncodeEventZoneConfig(ClosedEventZoneConfiguration());
}

application::ServiceResult<std::vector<std::uint8_t>> WorkshopScienceSection(
    application::WorkshopExecutor& workshop) {
  auto configuration = workshop.CompoundConfiguration();
  if (const auto* error =
          std::get_if<application::ServiceError>(&configuration)) {
    return *error;
  }
  return protocol::EncodeWorkshopScienceState(
      std::get<application::WorkshopCompoundConfiguration>(configuration));
}

bool IsRoomPhase(runtime::WorldSessionPhase phase) noexcept {
  switch (phase) {
    case runtime::WorldSessionPhase::kRoomWaiting:
    case runtime::WorldSessionPhase::kRoomLoading:
    case runtime::WorldSessionPhase::kRoomCountdown:
    case runtime::WorldSessionPhase::kRoomPlaying:
    case runtime::WorldSessionPhase::kRoomResults:
      return true;
    case runtime::WorldSessionPhase::kMenu:
    case runtime::WorldSessionPhase::kPlaza:
      return false;
  }
  return false;
}

application::ServiceError MakeError(application::ErrorCode code,
                                    std::string message,
                                    bool retryable = false) {
  return {.code = code,
          .safe_message = std::move(message),
          .correlation_id = {},
          .is_retryable = retryable};
}

bool IsClientVisibleTreasureError(application::ErrorCode code) noexcept {
  return code == application::ErrorCode::kInvalidInput ||
         code == application::ErrorCode::kInsufficientBalance ||
         code == application::ErrorCode::kConflict ||
         code == application::ErrorCode::kPermissionDenied;
}

runtime::OutboundMessage Notice(std::string_view message) {
  return {
      .opcode = 109U,
      .body = protocol::EncodeNoticeResponse(protocol::Utf8ToUtf16(message))};
}

application::ServiceResult<std::vector<runtime::OutboundMessage>>
TreasureStateMessages(domain::PlayerId player_id,
                      application::TreasureExecutor& treasure,
                      application::ServiceRulesLoader& service_rules,
                      application::ShopCatalogLoader& shop_catalog) {
  auto state_result = treasure.State(player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&state_result)) {
    return *error;
  }
  auto rules_result = service_rules.Load();
  if (const auto* error =
          std::get_if<application::ServiceError>(&rules_result)) {
    return *error;
  }
  auto catalog_result = shop_catalog.Load();
  if (const auto* error =
          std::get_if<application::ServiceError>(&catalog_result)) {
    return *error;
  }
  auto state =
      std::get<application::TreasureStateSnapshot>(std::move(state_result));
  const auto& rules =
      std::get<application::PublishedServiceRules>(rules_result);
  const auto& catalog =
      std::get<application::PublishedShopCatalog>(catalog_result);
  const auto standard_card =
      std::ranges::find(catalog.standard_cards, domain::ItemId{100382U},
                        &application::ShopCardView::item_id);
  const auto cash_card =
      std::ranges::find(catalog.cash_cards, domain::ItemId{100382U},
                        &application::ShopCardView::item_id);
  const application::ShopCardView* needle_card = nullptr;
  if (standard_card != catalog.standard_cards.end()) {
    needle_card = &*standard_card;
  } else if (cash_card != catalog.cash_cards.end()) {
    needle_card = &*cash_card;
  }
  if (needle_card == nullptr) {
    return MakeError(application::ErrorCode::kInternal,
                     "已发布目录缺少泡泡面板所需魔法针卡片");
  }
  std::vector<runtime::OutboundMessage> messages{
      {.opcode = 918U,
       .body = protocol::EncodeTreasureStateResponse(state, rules.bubbles,
                                                     *needle_card)}};
  if (state.pending_draw.has_value()) {
    messages.push_back(
        {.opcode = 919U,
         .body = protocol::EncodeTreasureRevealResponse(*state.pending_draw)});
  }
  return messages;
}

class GatewaySessionHandler final : public SessionPacketHandler {
 private:
  struct SharedState final {
    SharedState(SessionHandlerContext context,
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
                application::WorkshopExecutor& workshop,
                application::ClanExecutor& clans,
                application::ItemUseExecutor& item_use,
                application::WorldAccessLoader& world_access,
                runtime::WorldExecutor& world_executor,
                runtime::PurchaseOperationIdSource& purchase_operation_ids,
                runtime::SessionRegistry& session_registry,
                runtime::ClanTicketBroker& clan_tickets,
                const application::SystemClock& clock,
                protocol::GatewayEndpoint gateway_endpoint,
                std::shared_ptr<LauncherAuthenticationRateLimiter>
                    launcher_rate_limiter,
                application::DailyLoginRewardGranter* daily_rewards,
                application::KeyMapStore* key_maps,
                application::GiftStore* gifts,
                application::LevelIconStore* level_icons,
                runtime::CoupleFlow* couples,
                runtime::SocialSpacesFlow* social_spaces,
                application::ChatBlockStore* chat_blocks,
                runtime::LobbyMinigameService* lobby_minigames,
                application::NativeTitleStore* titles,
                application::EventLogger* logger,
                runtime::LifeAttendanceTracker* life_attendance)
        : handle(context.handle),
          peer_address(std::move(context.peer_address)),
          online_players(std::move(context.online_players)),
          flow(ticket_consumer, character_creation, menu_snapshots,
               shop_catalog, purchases, equipment, purchase_operation_ids,
               clock, *online_players, gateway_endpoint, daily_rewards),
          account_registration(account_registration),
          password_login(password_login),
          menu_snapshots(menu_snapshots),
          shop_catalog(shop_catalog),
          service_rules(service_rules),
          treasure(treasure),
          mystery_shop(mystery_shop),
          workshop(workshop),
          clans(clans),
          item_use(item_use),
          world_access(world_access),
          world_executor(world_executor),
          clock(clock),
          social_snapshots(social_snapshots),
          social_relationships(social_relationships),
          private_messages(private_messages),
          purchase_operation_ids(purchase_operation_ids),
          session_registry(session_registry),
          clan_tickets(clan_tickets),
          launcher_rate_limiter(std::move(launcher_rate_limiter)),
          key_maps(key_maps),
          gifts(gifts),
          level_icons(level_icons),
          couples(couples),
          social_spaces(social_spaces),
          chat_blocks(chat_blocks),
          lobby_minigames(lobby_minigames),
          titles(titles),
          logger(logger),
          life_attendance(life_attendance) {}

    std::mutex mutex;
    std::mutex attendance_mutex;  ///< Short memory-only bind/close ordering;
                                  ///< never held during SQL.
    runtime::LifeAttendanceTracker* life_attendance{nullptr};
    bool attendance_started{false};
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> authenticated_player{0U};
    application::EventLogger* logger{nullptr};  ///< 供异步分支记录处置结果。
    std::mutex rejection_mutex;
    std::chrono::steady_clock::time_point last_rejection_log{};
    bool logged_rejection{false};
    std::chrono::steady_clock::time_point next_moderation_check{};
    std::chrono::steady_clock::time_point last_movement_moderation_log{};
    bool device_activated{false};
    runtime::SessionHandle handle;
    std::string peer_address;
    // 声明顺序保证 flow 先析构、再销毁其借用的 Session 专属 registry。
    std::unique_ptr<runtime::OnlinePlayerRegistry> online_players;
    runtime::GatewayLoginFlow flow;
    application::RegisterAccountService& account_registration;
    application::LoginWithPasswordService& password_login;
    application::PlayerMenuSnapshotLoader& menu_snapshots;
    application::ShopCatalogLoader& shop_catalog;
    application::ServiceRulesLoader& service_rules;
    application::TreasureExecutor& treasure;
    application::MysteryShopExecutor& mystery_shop;
    application::WorkshopExecutor& workshop;
    application::ClanExecutor& clans;
    application::ItemUseExecutor& item_use;
    application::WorldAccessLoader& world_access;
    runtime::WorldExecutor& world_executor;
    const application::SystemClock& clock;
    runtime::WorldSessionState world_state;
    application::SocialSnapshotLoader& social_snapshots;
    application::FriendRelationshipChanger& social_relationships;
    application::PrivateMessageSender& private_messages;
    runtime::PurchaseOperationIdSource& purchase_operation_ids;
    runtime::SessionRegistry& session_registry;
    runtime::ClanTicketBroker& clan_tickets;
    std::shared_ptr<LauncherAuthenticationRateLimiter> launcher_rate_limiter;
    application::KeyMapStore* key_maps;
    application::GiftStore* gifts;
    application::LevelIconStore* level_icons;
    runtime::CoupleFlow* couples;
    runtime::SocialSpacesFlow* social_spaces;
    application::ChatBlockStore* chat_blocks;
    runtime::LobbyMinigameService* lobby_minigames;
    application::NativeTitleStore* titles;
    runtime::LobbyFishingSession
        lobby_fishing;  ///< 571/10 与 571/11 之间已确认的本连接计费条件。
    std::unique_ptr<runtime::SocialCommandFlow> social_flow;
    std::set<std::uint16_t> logged_unsupported_opcodes;
  };

  static void PublishCouple(const std::shared_ptr<SharedState>& state,
                            application::EventLogger* logger,
                            runtime::CoupleFlowResult& value) {
    std::map<domain::PlayerId, std::vector<runtime::OutboundMessage>> batches;
    for (auto& delivery : value.messages.deliveries)
      batches[delivery.recipient].push_back(std::move(delivery.message));
    for (auto& [player, messages] : batches) {
      const auto expected = value.expected_sessions.find(player);
      if (expected == value.expected_sessions.end()) continue;
      const auto dispatched = state->session_registry.DispatchIfCurrent(
          player, expected->second, std::move(messages));
      if (const auto* error =
              std::get_if<application::ServiceError>(&dispatched))
        logger->Log(application::LogLevel::kWarning,
                    "couple delivery rejected: " + error->safe_message);
    }
    if (!value.changed.empty())
      if (const auto error = state->world_executor.PostCoupleProfiles(
              std::move(value.changed)))
        logger->Log(application::LogLevel::kWarning,
                    "couple profile refresh rejected: " + error->safe_message);
  }

  static bool TutorialPersistenceFrame(std::uint16_t opcode,
                                       runtime::WorldSessionPhase phase) {
    return opcode == 107U ||
           ((phase == runtime::WorldSessionPhase::kTutorialLoading ||
             phase == runtime::WorldSessionPhase::kTutorialPlaying ||
             phase == runtime::WorldSessionPhase::kSinglePlayWaiting) &&
            (opcode == 905U || opcode == 725U || opcode == 105U ||
             opcode == 404U));
  }

  static void ValidateTutorialPersistenceFrame(const protocol::Packet& packet) {
    if (packet.opcode == 107U)
      static_cast<void>(protocol::DecodeTutorialStartReport(packet.body));
    else if (packet.opcode == 725U)
      static_cast<void>(protocol::DecodeSpecialModeFinishClaim(packet.body));
    else
      protocol::PacketReader{packet.body}
          .RequireEnd();  // 105/404/905 are original empty requests.
  }

  static runtime::WorldCommandCompletion TutorialLateCompletion(
      const std::shared_ptr<SharedState>& state, domain::PlayerId player) {
    const auto origin = state->handle;
    return [state, player, origin](
               application::ServiceResult<std::vector<runtime::OutboundMessage>>
                   result) {
      // Empty frame acceptance only releases the socket read. All positive
      // scene/phase candidates are independently SQL-confirmed and handle-bound
      // by WorldExecutor.
      if (const auto* error = std::get_if<application::ServiceError>(&result))
        static_cast<void>(state->session_registry.DispatchIfCurrent(
            player, origin,
            std::vector<runtime::OutboundMessage>{
                Notice(error->safe_message)}));
    };
  }

 public:
  GatewaySessionHandler(
      SessionHandlerContext context,
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
      std::shared_ptr<LauncherAuthenticationRateLimiter> launcher_rate_limiter,
      application::DailyLoginRewardGranter* daily_rewards,
      application::KeyMapStore* key_maps, application::GiftStore* gifts,
      application::LevelIconStore* level_icons, runtime::CoupleFlow* couples,
      runtime::SocialSpacesFlow* social_spaces,
      application::ChatBlockStore* chat_blocks,
      runtime::LobbyMinigameService* lobby_minigames,
      application::NativeTitleStore* titles,
      runtime::LifeAttendanceTracker* life_attendance)
      : database_tasks_(database_tasks),
        logger_(logger),
        schedule_after_(std::move(context.schedule_after)),
        state_(std::make_shared<SharedState>(
            std::move(context), ticket_consumer, account_registration,
            password_login, character_creation, menu_snapshots, shop_catalog,
            purchases, equipment, social_snapshots, social_relationships,
            private_messages, service_rules, treasure, mystery_shop, workshop,
            clans, item_use, world_access, world_executor,
            purchase_operation_ids, session_registry, clan_tickets, clock,
            gateway_endpoint, std::move(launcher_rate_limiter), daily_rewards,
            key_maps, gifts, level_icons, couples, social_spaces, chat_blocks,
            lobby_minigames, titles, &logger_, life_attendance)) {}

  void AsyncHandle(protocol::Packet packet,
                   PacketCompletion completion) override {
    // Older clients may still send retired scan/EAC traffic.
    if (packet.opcode == 65040U || packet.opcode == 65050U) {
      completion(std::vector<runtime::OutboundMessage>{});
      return;
    }
    auto state = state_;
    auto* logger = &logger_;
    const auto opcode = packet.opcode;
    const auto body_bytes = packet.body.size();
    auto completion_state = std::make_shared<std::optional<PacketCompletion>>(
        std::move(completion));
    auto complete_once =
        [completion_state, state, logger, opcode, body_bytes](
            application::ServiceResult<std::vector<runtime::OutboundMessage>>
                result) mutable {
          if (!completion_state->has_value()) {
            return;
          }
          auto callback = std::move(**completion_state);
          completion_state->reset();
          const auto player =
              state->authenticated_player.load(std::memory_order_acquire);
          if (const auto* error =
                  std::get_if<application::ServiceError>(&result);
              error && player != 0U &&
              !state->closing.load(std::memory_order_acquire) &&
              (error->code == application::ErrorCode::kInvalidState ||
               error->code == application::ErrorCode::kConflict ||
               error->code == application::ErrorCode::kPermissionDenied ||
               error->code == application::ErrorCode::kInsufficientBalance ||
               error->code == application::ErrorCode::kOverloaded)) {
            // 世界、装备异步回执和网关直接处理共用这一出口；规则拒绝不能落到
            // TCP finalize。
            bool log_rejection = false;
            {
              std::scoped_lock lock{state->rejection_mutex};
              const auto now = std::chrono::steady_clock::now();
              if (!state->logged_rejection ||
                  now - state->last_rejection_log >= std::chrono::seconds{5}) {
                state->last_rejection_log = now;
                state->logged_rejection = true;
                log_rejection = true;
              }
            }
            if (log_rejection) {
              logger->Log(
                  application::LogLevel::kWarning,
                  "gateway rule rejected, keeping connection: opcode=" +
                      std::to_string(opcode) +
                      " body_bytes=" + std::to_string(body_bytes) +
                      " player_id=" + std::to_string(player) + " code=" +
                      std::string{application::ErrorCodeName(error->code)} +
                      " detail=" + error->safe_message);
            }
            callback(std::vector<runtime::OutboundMessage>{});
            return;
          }
          callback(std::move(result));
        };
    if (ClassifyGatewayOpcode(packet.opcode) ==
        GatewayOpcodeRoute::kWorldDirect) {
      if (PostWorldDirect(state, packet, complete_once, *logger)) {
        return;
      }
      if (schedule_after_) {
        auto check = std::make_shared<PendingAccessCheck>();
        check->state = state;
        check->packet = std::move(packet);
        check->completion = complete_once;
        check->database_tasks = &database_tasks_;
        check->logger = logger;
        check->schedule_after = schedule_after_;
        AttemptAccessCheck(std::move(check));
        return;
      }
    }
    const auto post_error = database_tasks_
                                .Post(
                                    [state, packet = std::move(packet),
                                     complete_once, logger]() mutable {
                                      if (state->closing.load(
                                              std::memory_order_acquire)) {
                                        complete_once(
                                            MakeError(application::ErrorCode::
                                                          kInvalidState,
                                                      "Session 正在关闭"));
                                        return;
                                      }
                                      try {
                                        std::scoped_lock lock{state->mutex};
                                        if (state->closing.load(
                                                std::memory_order_acquire)) {
                                          complete_once(
                                              MakeError(application::ErrorCode::
                                                            kInvalidState,
                                                        "Session 正在关闭"));
                                          return;
                                        }
                                        if (state->flow.PlayerId() &&
                                            std::chrono::steady_clock::now() >=
                                                state->next_moderation_check) {
                                          const auto checked =
                                              state->world_access.Load(
                                                  *state->flow.PlayerId());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &checked)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          if (std::get<application::
                                                           WorldAccessSnapshot>(
                                                  checked)
                                                  .banned) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "账号或当前机器已被封禁"));
                                            return;
                                          }
                                          state->next_moderation_check =
                                              std::chrono::steady_clock::now() +
                                              std::chrono::seconds{1};
                                        }
                                        if (IsGiftRequest(packet.opcode) &&
                                            state->gifts) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "礼物服务需要完成菜单登录"));
                                            return;
                                          }
                                          const auto request =
                                              DecodeGiftRequest(packet);
                                          const auto player =
                                              *state->flow.PlayerId();
                                          auto work = [state, player, request](
                                                          bool allowed,
                                                          std::uint32_t) {
                                            return HandleGiftRequest(
                                                request, player, allowed,
                                                *state->gifts,
                                                state->menu_snapshots,
                                                state->purchase_operation_ids,
                                                state->session_registry);
                                          };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle))
                                            complete_once(*error);
                                          return;
                                        }
                                        // 65030 is the 752 bridge's explicit
                                        // refresh; 147/148 and responses
                                        // 170/171/172 keep the verified build
                                        // 1302 layouts (EE6BB0/EE6970,
                                        // 9D0030/9D0290/9D0450).
                                        if (packet.opcode ==
                                                protocol::
                                                    kLobbyMinigameRequestOpcode &&
                                            state->lobby_minigames) {
                                          // LobbyMinigameContext only exists in
                                          // the plaza stage (971B20 handles
                                          // S2C571).
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId() ||
                                              state->world_state.phase !=
                                                  runtime::WorldSessionPhase::
                                                      kPlaza) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "广场小游戏只能在自由广场使用"));
                                            return;
                                          }
                                          const auto request = protocol::
                                              DecodeLobbyMinigameRequest(
                                                  packet.body);
                                          auto reply =
                                              state->lobby_minigames->Handle(
                                                  *state->flow.PlayerId(),
                                                  request,
                                                  state->lobby_fishing);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &reply)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          std::vector<runtime::OutboundMessage>
                                              replies{
                                                  {.opcode = protocol::
                                                       kLobbyMinigameRequestOpcode,
                                                   .body = std::get<std::vector<
                                                       std::uint8_t>>(
                                                       std::move(reply))}};
                                          // 67E930 selects the next cast from
                                          // the client's actual inventory.
                                          // Refresh after consumption/rewards
                                          // and stale terms; a later read
                                          // failure cannot undo 571.
                                          try {
                                            auto snapshot =
                                                state->menu_snapshots.Load(
                                                    *state->flow.PlayerId());
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &snapshot)) {
                                              replies.push_back(
                                                  Notice(error->safe_message));
                                            } else {
                                              replies.push_back(
                                                  {.opcode = 802U,
                                                   .body = protocol::
                                                       EncodeInventorySnapshotResponse(
                                                           std::get<
                                                               application::
                                                                   PlayerMenuSnapshot>(
                                                               std::move(
                                                                   snapshot)))});
                                            }
                                            auto record =
                                                state->lobby_minigames->Record(
                                                    *state->flow.PlayerId());
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &record)) {
                                              replies.push_back(
                                                  Notice(error->safe_message));
                                            } else {
                                              const auto& value = std::get<
                                                  runtime::LobbyMinigameRecord>(
                                                  record);
                                              replies.push_back(
                                                  {.opcode = 134U,
                                                   .body = protocol::
                                                       EncodeLobbyMinigameRecord(
                                                           value.fishing_count,
                                                           value.fortune_paper
                                                               .value_or(0U))});
                                            }
                                          } catch (const std::exception&) {
                                            replies.push_back(Notice(
                                                "小游戏回执已保留，库存刷新暂不可用，请重新读取仓库。"));
                                          }
                                          complete_once(std::move(replies));
                                          return;
                                        }
                                        if ((packet.opcode == 147U ||
                                             packet.opcode == 148U ||
                                             packet.opcode == 65030U) &&
                                            state->chat_blocks) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "聊天名单需要完成菜单登录"));
                                            return;
                                          }
                                          std::vector<runtime::OutboundMessage>
                                              replies;
                                          if (packet.opcode == 65030U) {
                                            protocol::PacketReader{packet.body}
                                                .RequireEnd();
                                          } else {
                                            const auto target =
                                                protocol::DecodeChatBlockTarget(
                                                    packet.body);
                                            auto changed =
                                                state->chat_blocks->Change(
                                                    *state->flow.PlayerId(),
                                                    target,
                                                    packet.opcode == 147U);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &changed)) {
                                              replies.push_back(
                                                  Notice(error->safe_message));
                                            } else {
                                              const auto& entry = std::get<
                                                  application::ChatBlockEntry>(
                                                  changed);
                                              replies.push_back(
                                                  {.opcode = static_cast<
                                                       std::uint16_t>(
                                                       packet.opcode == 147U
                                                           ? 171U
                                                           : 172U),
                                                   .body =
                                                       packet.opcode == 147U
                                                           ? protocol::
                                                                 EncodeChatBlockAdded(
                                                                     entry)
                                                           : protocol::
                                                                 EncodeChatBlockRemoved(
                                                                     entry)});
                                            }
                                          }
                                          auto loaded =
                                              state->chat_blocks->Load(
                                                  *state->flow.PlayerId());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            replies.push_back(
                                                Notice(error->safe_message));
                                          } else {
                                            replies.push_back(
                                                {.opcode = 170U,
                                                 .body = protocol::
                                                     EncodeChatBlockList(
                                                         std::get<std::vector<
                                                             application::
                                                                 ChatBlockEntry>>(
                                                             loaded))});
                                          }
                                          complete_once(std::move(replies));
                                          return;
                                        }
                                        if (packet.opcode == 2008U &&
                                            state->level_icons) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "等级图标需要完成菜单登录"));
                                            return;
                                          }
                                          const auto selection =
                                              protocol::DecodeLevelIconRequest(
                                                  packet.body);
                                          const auto player =
                                              *state->flow.PlayerId();
                                          auto work = [state, player,
                                                       selection](bool allowed,
                                                                  std::uint32_t)
                                              -> application::ServiceResult<
                                                  runtime::
                                                      EquipmentWorldResult> {
                                            if (!allowed)
                                              return runtime::EquipmentWorldResult{
                                                  .replies = {Notice(
                                                      "当前阶段不能修改等级图标")}};
                                            auto saved =
                                                state->level_icons->Save(
                                                    player, selection);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &saved)) {
                                              if (error->code ==
                                                      application::ErrorCode::
                                                          kInvalidInput ||
                                                  error->code ==
                                                      application::ErrorCode::
                                                          kInvalidState ||
                                                  error->code ==
                                                      application::ErrorCode::
                                                          kPermissionDenied)
                                                return runtime::
                                                    EquipmentWorldResult{
                                                        .replies = {Notice(
                                                            error
                                                                ->safe_message)}};
                                              return *error;
                                            }
                                            auto loaded =
                                                state->menu_snapshots.Load(
                                                    player);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &loaded))
                                              return *error;
                                            return runtime::
                                                EquipmentWorldResult{
                                                    .changed = std::get<
                                                        application::
                                                            PlayerMenuSnapshot>(
                                                        std::move(loaded)),
                                                    .level_icon_only = true};
                                          };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle))
                                            complete_once(*error);
                                          return;
                                        }
                                        if (packet.opcode == 2016U &&
                                            state->key_maps) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "按键设置需要完成菜单登录"));
                                            return;
                                          }
                                          const auto settings =
                                              protocol::DecodeKeyMapRequest(
                                                  packet.body);
                                          auto saved = state->key_maps->Save(
                                              *state->flow.PlayerId(),
                                              settings);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &saved)) {
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    {.opcode = 981U,
                                                     .body = protocol::
                                                         EncodeKeyMapResponse(
                                                             false, {})},
                                                    Notice(
                                                        error->safe_message)});
                                          } else {
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    {.opcode = 981U,
                                                     .body = protocol::
                                                         EncodeKeyMapResponse(
                                                             true,
                                                             settings
                                                                 .bindings)}});
                                          }
                                          return;
                                        }
                                        if (packet.opcode ==
                                            kLauncherAuthenticationRequestOpcode) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kConnected) {
                                            complete_once(LauncherErrorResponse(
                                                "账号操作只能在新连接上执行。"));
                                            return;
                                          }
                                          complete_once(HandleLauncherRequest(
                                              packet, state->peer_address,
                                              *state->launcher_rate_limiter,
                                              state->account_registration,
                                              state->password_login));
                                          return;
                                        }
                                        if ((packet.opcode >= 936U &&
                                             packet.opcode <= 939U) ||
                                            packet.opcode == 956U) {
                                          if (!state->couples ||
                                              state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(
                                                std::vector<
                                                    runtime::
                                                        OutboundMessage>{Notice(
                                                    "情侣功能需要完成登录初始化")});
                                            return;
                                          }
                                          auto publish =
                                              [state, logger](
                                                  runtime::CoupleFlowResult&
                                                      value) {
                                                PublishCouple(state, logger,
                                                              value);
                                              };
                                          auto result = state->couples->Handle(
                                              *state->flow.PlayerId(),
                                              state->handle, packet,
                                              std::chrono::steady_clock::now(),
                                              state->clock.Now(), publish);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &result)) {
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    Notice(
                                                        error->safe_message)});
                                          } else
                                            complete_once(
                                                std::vector<
                                                    runtime::
                                                        OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 65036U) {
                                          if (!state->social_spaces ||
                                              !state->flow.PlayerId() ||
                                              state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady) {
                                            complete_once(
                                                std::vector<
                                                    runtime::
                                                        OutboundMessage>{Notice(
                                                    "通讯功能需要完成登录初始化")});
                                            return;
                                          }
                                          const auto error =
                                              state->social_spaces->Handle(
                                                  *state->flow.PlayerId(),
                                                  state->handle, packet.body,
                                                  std::chrono::steady_clock::
                                                      now());
                                          if (error)
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    Notice(
                                                        error->safe_message)});
                                          else
                                            complete_once(
                                                std::vector<
                                                    runtime::
                                                        OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 65010U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "社交请求只能在菜单初始化后发送"));
                                            return;
                                          }
                                          if (!state->social_flow) {
                                            const auto player_id =
                                                state->flow.PlayerId();
                                            const auto account_name =
                                                state->flow.AccountName();
                                            if (!player_id.has_value() ||
                                                !account_name.has_value()) {
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kInternal,
                                                  "已认证 Session 缺少社交身份"));
                                              return;
                                            }
                                            auto parsed_account =
                                                domain::AccountName::Parse(
                                                    *account_name);
                                            const auto* account = std::get_if<
                                                domain::AccountName>(
                                                &parsed_account);
                                            if (account == nullptr) {
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kInternal,
                                                  "已认证 Session 账号名不满足社交契约"));
                                              return;
                                            }
                                            state->social_flow =
                                                std::make_unique<
                                                    runtime::SocialCommandFlow>(
                                                    runtime::
                                                        SocialActorIdentity{
                                                            .player_id =
                                                                *player_id,
                                                            .account_name =
                                                                *account},
                                                    state->social_snapshots,
                                                    state->social_relationships,
                                                    state->private_messages,
                                                    state->session_registry);
                                          }
                                          const bool was_native =
                                              state->social_flow
                                                  ->NativeSocialEnabled();
                                          auto social_result =
                                              state->social_flow->Handle(
                                                  packet,
                                                  std::chrono::steady_clock::
                                                      now());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &social_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          if (const auto error =
                                                  state->session_registry
                                                      .SetSocialCapabilities(
                                                          state->handle, true,
                                                          state->social_flow
                                                              ->NativeSocialEnabled())) {
                                            complete_once(*error);
                                            return;
                                          }
                                          if (!was_native &&
                                              state->social_flow
                                                  ->NativeSocialEnabled()) {
                                            auto update =
                                                runtime::SocialPresenceChanged(
                                                    *state->flow.PlayerId(),
                                                    state->social_snapshots,
                                                    state->session_registry);
                                            if (auto* value = std::get_if<
                                                    runtime::
                                                        SocialCommandResult>(
                                                    &update))
                                              static_cast<void>(
                                                  state->session_registry
                                                      .Dispatch(
                                                          std::move(*value)));
                                          }
                                          auto dispatch_result =
                                              state->session_registry.Dispatch(
                                                  std::get<
                                                      runtime::
                                                          SocialCommandResult>(
                                                      std::move(
                                                          social_result)));
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &dispatch_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto& report = std::get<
                                              runtime::SessionDispatchReport>(
                                              dispatch_result);
                                          const auto current_player =
                                              state->flow.PlayerId();
                                          const bool sender_rejected =
                                              !current_player.has_value() ||
                                              std::ranges::find(
                                                  report.offline_players,
                                                  *current_player) !=
                                                  report.offline_players
                                                      .end() ||
                                              std::ranges::any_of(
                                                  report.rejected_batches,
                                                  [&](const auto& failure) {
                                                    return failure.player_id ==
                                                           *current_player;
                                                  });
                                          if (sender_rejected) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kOverloaded,
                                                "社交响应无法投递到当前 Session",
                                                true));
                                            return;
                                          }
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 25U &&
                                            packet.body.empty()) {
                                          // 0.2.80.1 客户端会在票据登录前探测
                                          // C2S25，并在 S2C3 后、C2S102
                                          // 前请求正式
                                          // 战队凭证。前者与旧服务端一致地静默忽略，后者必须允许签发，否则
                                          // TCP 层会把
                                          // 状态错误视为致命错误并令客户端退出。
                                          switch (state->flow.Phase()) {
                                            case runtime::GatewaySessionPhase::
                                                kPatchReady:
                                            case runtime::GatewaySessionPhase::
                                                kCreatingCharacter:
                                              complete_once(
                                                  std::vector<
                                                      runtime::
                                                          OutboundMessage>{});
                                              return;
                                            case runtime::GatewaySessionPhase::
                                                kAuthenticated:
                                            case runtime::GatewaySessionPhase::
                                                kMenuReady:
                                              break;
                                            case runtime::GatewaySessionPhase::
                                                kConnected:
                                            case runtime::GatewaySessionPhase::
                                                kClosing:
                                            case runtime::GatewaySessionPhase::
                                                kClosed:
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kInvalidState,
                                                  "战队连接请求不允许在当前网关阶段执行"));
                                              return;
                                          }
                                          const auto player_id =
                                              state->flow.PlayerId();
                                          const auto nickname =
                                              state->flow.Nickname();
                                          if (!player_id.has_value() ||
                                              !nickname.has_value()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInternal,
                                                "已认证 Session 缺少战队身份"));
                                            return;
                                          }
                                          auto ticket_result =
                                              state->clan_tickets.Issue(
                                                  *player_id, *nickname,
                                                  state->handle);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &ticket_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto& ticket = std::get<
                                              runtime::IssuedClanTicket>(
                                              ticket_result);
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{
                                                  {.opcode = 23U,
                                                   .body = protocol::
                                                       EncodeGatewayClanTicket(
                                                           ticket.player_id,
                                                           ticket.nickname,
                                                           ticket.secret)}});
                                          return;
                                        }
                                        if (packet.opcode == 2033U) {
                                          if (!packet.body.empty()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidInput,
                                                "房间目录打开通知包含尾随字段"));
                                            return;
                                          }
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              (state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kMenu &&
                                               state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kPlaza)) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "房间目录打开通知不允许在当前阶段发送"));
                                            return;
                                          }

                                          // 0.2.80.1
                                          // 客户端在打开“等待房间”目录时，先发送空
                                          // C2S2033，再发送真正的 C2S401
                                          // 分页查询。反编译样本确认前者没有字段；Python
                                          // 基线也只记录后静默
                                          // 忽略。这里仅接受已验证的空通知，房间列表仍由
                                          // C2S401 生成，不能伪造响应。
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 2004U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              (state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kMenu &&
                                               state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kPlaza)) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "世界上下文标志不允许在当前阶段发送"));
                                            return;
                                          }

                                          // 当前 EXE
                                          // 的唯一发送函数固定写入一个字节；五个调用路径只传
                                          // 0/1，且发送后
                                          // 不等待响应。本轮原客户端在大厅进入竞技场前观察到该消息；字段语义尚无证据，
                                          // 因此只做严格线格式兼容，不把它提升为房间、广场或玩家状态命令。
                                          static_cast<void>(
                                              protocol::
                                                  DecodeWorldContextFlagRequest(
                                                      packet.body));
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 2009U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              (state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kMenu &&
                                               state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kPlaza)) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "世界上下文信号不允许在当前阶段发送"));
                                            return;
                                          }

                                          // 当前 EXE 的发送函数固定写入一个
                                          // DWORD，两个调用路径分别传 0/1；8
                                          // 份历史 原客户端消息只出现在
                                          // menu_ready/plaza，旧 Python
                                          // 服务不回包并保持连接。
                                          // 字段的业务语义没有证据，因此仅校验已知取值，不把它提升为权威状态命令。
                                          static_cast<void>(
                                              protocol::
                                                  DecodeWorldContextSignalRequest(
                                                      packet.body));
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 2006U ||
                                            packet.opcode == 711U ||
                                            packet.opcode == 927U ||
                                            packet.opcode == 2040U ||
                                            packet.opcode == 2045U ||
                                            packet.opcode == 2007U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "原生旁路通知不允许在当前网关阶段发送"));
                                            return;
                                          }
                                          // 原生发送器格式已由 IDA
                                          // 确认。旧服务端无响应；允许迟到通知跨场景到达，
                                          // 不把未知业务含义的报告当成奖励、伤害或物品生成授权。
                                          protocol::
                                              ValidateNativeAuxiliaryReport(
                                                  packet.opcode, packet.body);
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 2019U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "房间上下文信号不允许在当前阶段发送"));
                                            return;
                                          }

                                          // 当前 EXE 的唯一发送函数固定写入一个
                                          // DWORD；三份原客户端日志分别观察到值
                                          // 1/4， 旧 Python
                                          // 服务记录为未恢复消息后不回包且保持连接。字段语义尚无证据，因此只做
                                          // 严格线格式兼容，绝不据此修改准备、开局、席位或战斗状态。
                                          static_cast<void>(
                                              protocol::
                                                  DecodeRoomContextSignalRequest(
                                                      packet.body));
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 935U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "战士排名请求只能在菜单初始化后发送"));
                                            return;
                                          }
                                          static_cast<void>(
                                              protocol::
                                                  DecodeFighterRankingRequest(
                                                      packet.body));
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 939U,
                                               .body = protocol::
                                                   EncodeFighterRankingResponse(
                                                       {.result = 1U})},
                                              Notice(
                                                  "战士分项排行的原版统计规则尚未恢复")});
                                          return;
                                        }
                                        if (packet.opcode == 811U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "兑换请求只能在菜单初始化后发送"));
                                            return;
                                          }
                                          protocol::PacketReader reader{
                                              packet.body};
                                          const auto value = reader.ReadU32();
                                          reader.RequireEnd();
                                          // 盛大现金是外部平台钱包，不存在已认证的钱包来源时不能扣款或发放点券。
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 816U,
                                               .body = protocol::
                                                   EncodeBubbleCashExchangeResponse(
                                                       false, value)}});
                                          return;
                                        }
                                        if (packet.opcode == 931U ||
                                            packet.opcode == 932U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "称号请求需要完成本人角色登录"));
                                            return;
                                          }
                                          const bool claim =
                                              packet.opcode == 931U;
                                          const auto player =
                                              *state->flow.PlayerId();
                                          const auto grant =
                                              claim
                                                  ? protocol::
                                                        DecodeNativeTitleClaimRequest(
                                                            packet.body)
                                                  : protocol::
                                                        NativeTitleClaimRequest{};
                                          const auto setting =
                                              claim
                                                  ? protocol::
                                                        NativeTitleSetRequest{}
                                                  : protocol::
                                                        DecodeNativeTitleSetRequest(
                                                            packet.body);
                                          const auto self =
                                              claim ? grant.self : setting.self;
                                          const auto title =
                                              claim ? grant.title_id
                                                    : setting.title_id;
                                          const auto response = [claim, title](
                                                                    std::int32_t
                                                                        code) {
                                            return runtime::OutboundMessage{
                                                .opcode =
                                                    static_cast<std::uint16_t>(
                                                        claim ? 935U : 936U),
                                                .body =
                                                    claim
                                                        ? protocol::
                                                              EncodeTitleClaimResponse(
                                                                  title, code)
                                                        : protocol::
                                                              EncodeNativeTitleSetResponse(
                                                                  code)};
                                          };
                                          if (self != player ||
                                              !state->titles) {
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    response(-1)});
                                            return;
                                          }
                                          auto operation =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto id =
                                              std::get<domain::OperationId>(
                                                  std::move(operation));
                                          auto
                                              work =
                                                  [state, player, title, claim,
                                                   setting, id, response,
                                                   logger](
                                                      bool allowed, std::
                                                                        uint32_t) -> application::
                                                                                      ServiceResult<
                                                                                          runtime::
                                                                                              EquipmentWorldResult> {
                                                                                        if (!allowed)
                                                                                          return runtime::EquipmentWorldResult{
                                                                                              .replies = {response(
                                                                                                  -1)}};
                                                                                        auto changed =
                                                                                            claim
                                                                                                ? state
                                                                                                      ->titles
                                                                                                      ->Claim(
                                                                                                          player,
                                                                                                          title,
                                                                                                          id)
                                                                                                : state
                                                                                                      ->titles
                                                                                                      ->Set(
                                                                                                          player,
                                                                                                          title,
                                                                                                          setting
                                                                                                              .action_flag);
                                                                                        if (const auto* error =
                                                                                                std::get_if<
                                                                                                    application::
                                                                                                        ServiceError>(
                                                                                                    &changed))
                                                                                          return *error;
                                                                                        const auto result =
                                                                                            std::get<
                                                                                                application::
                                                                                                    NativeTitleResult>(
                                                                                                changed);
                                                                                        std::vector<
                                                                                            runtime::
                                                                                                OutboundMessage>
                                                                                            replies{response(
                                                                                                result
                                                                                                    .result)};
                                                                                        if (result
                                                                                                .result <
                                                                                            0)
                                                                                          return runtime::EquipmentWorldResult{
                                                                                              .replies =
                                                                                                  std::move(
                                                                                                      replies)};
                                                                                        application::ServiceResult<
                                                                                            application::
                                                                                                PlayerMenuSnapshot>
                                                                                            loaded;
                                                                                        try {
                                                                                          loaded =
                                                                                              state
                                                                                                  ->menu_snapshots
                                                                                                  .Load(
                                                                                                      player);
                                                                                        } catch (
                                                                                            const std::
                                                                                                exception&) {
                                                                                          loaded = MakeError(
                                                                                              application::
                                                                                                  ErrorCode::
                                                                                                      kStorageUnavailable,
                                                                                              "称号提交后的资料刷新失败");
                                                                                        }
                                                                                        if (const auto* error =
                                                                                                std::get_if<
                                                                                                    application::
                                                                                                        ServiceError>(
                                                                                                    &loaded)) {
                                                                                          logger
                                                                                              ->Log(
                                                                                                  application::
                                                                                                      LogLevel::
                                                                                                          kWarning,
                                                                                                  "native title committed; refresh unavailable: " +
                                                                                                      error
                                                                                                          ->safe_message);
                                                                                          replies
                                                                                              .push_back(Notice(
                                                                                                  "称号操作已提交，资料刷新暂不可用，请重新读取仓库和勋章"));
                                                                                          return runtime::EquipmentWorldResult{
                                                                                              .replies =
                                                                                                  std::move(
                                                                                                      replies)};
                                                                                        }
                                                                                        auto snapshot = std::get<
                                                                                            application::
                                                                                                PlayerMenuSnapshot>(
                                                                                            std::move(
                                                                                                loaded));
                                                                                        std::vector<
                                                                                            runtime::
                                                                                                OutboundMessage>
                                                                                            refresh;
                                                                                        try {
                                                                                          refresh
                                                                                              .push_back(
                                                                                                  {.opcode =
                                                                                                       802U,
                                                                                                   .body = protocol::
                                                                                                       EncodeInventorySnapshotResponse(
                                                                                                           snapshot)});
                                                                                          refresh
                                                                                              .push_back(
                                                                                                  {.opcode =
                                                                                                       806U,
                                                                                                   .body = protocol::
                                                                                                       EncodeEquippedItemsResponse(
                                                                                                           snapshot)});
                                                                                        } catch (
                                                                                            const std::
                                                                                                exception&) {
                                                                                          logger
                                                                                              ->Log(
                                                                                                  application::
                                                                                                      LogLevel::
                                                                                                          kWarning,
                                                                                                  "native title committed; refresh encoding failed");
                                                                                          replies
                                                                                              .push_back(Notice(
                                                                                                  "称号操作已提交，资料刷新暂不可用，请重新读取仓库和勋章"));
                                                                                          return runtime::EquipmentWorldResult{
                                                                                              .replies =
                                                                                                  std::move(
                                                                                                      replies)};
                                                                                        }
                                                                                        try {
                                                                                          refresh
                                                                                              .push_back(
                                                                                                  {.opcode =
                                                                                                       126U,
                                                                                                   .body = protocol::
                                                                                                       EncodeNativeMedalSnapshot(
                                                                                                           snapshot)});
                                                                                        } catch (
                                                                                            const protocol::
                                                                                                ProtocolError&) {
                                                                                          // 64位真实余额不能截断成原客户端有符号DWORD；也不能抹掉已经提交的称号回执。
                                                                                          refresh
                                                                                              .push_back(Notice(
                                                                                                  "称号操作已提交，当前勋章余额超过客户端可表示范围"));
                                                                                        }
                                                                                        for (
                                                                                            auto&
                                                                                                refresh_message :
                                                                                            refresh)
                                                                                          replies
                                                                                              .push_back(
                                                                                                  std::move(
                                                                                                      refresh_message));
                                                                                        return runtime::EquipmentWorldResult{
                                                                                            .replies =
                                                                                                std::move(
                                                                                                    replies),
                                                                                            .changed =
                                                                                                result.changed &&
                                                                                                        !claim
                                                                                                    ? std::optional{std::move(
                                                                                                          snapshot)}
                                                                                                    : std::
                                                                                                          nullopt};
                                                                                      };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle))
                                            complete_once(*error);
                                          return;
                                        }
                                        if (packet.opcode == 125U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "勋章信息需要完成角色登录"));
                                            return;
                                          }
                                          // 该上行不携带目标，查询身份只能来自认证会话。
                                          protocol::
                                              ValidateEventZoneConfigRequest(
                                                  packet.body);
                                          auto loaded =
                                              state->menu_snapshots.Load(
                                                  *state->flow.PlayerId());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 126U,
                                               .body = protocol::
                                                   EncodeNativeMedalSnapshot(
                                                       std::get<
                                                           application::
                                                               PlayerMenuSnapshot>(
                                                           loaded))}});
                                          return;
                                        }
                                        if (packet.opcode == 115U ||
                                            packet.opcode == 116U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "活动区请求只能在菜单初始化后发送"));
                                            return;
                                          }
                                          bool cancel = false;
                                          if (packet.opcode == 115U) {
                                            static_cast<void>(
                                                protocol::
                                                    DecodeEventZoneRequest(
                                                        packet.body));
                                          } else {
                                            cancel = protocol::
                                                DecodeEventZoneCancelRequest(
                                                    packet.body);
                                          }
                                          // B836D0: 22 显示原维护提示、清
                                          // pending 并可能发 116/action1；1
                                          // 完成取消。
                                          // 完整终态回执使原等待框可以关闭，但不创建队列、房间或任何比赛/资产事实。
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 976U,
                                               .body = protocol::
                                                   EncodeEventZoneUnavailableResponse(
                                                       cancel)}});
                                          return;
                                        }
                                        if (packet.opcode == 140U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "活动赛配置只能在菜单初始化后请求"));
                                            return;
                                          }
                                          protocol::
                                              ValidateEventZoneConfigRequest(
                                                  packet.body);
                                          // 原版活动赛的分组与开放档期只在原服务端；客户端匹配流程（115/116/141
                                          // -> 976， 房主随后以 503
                                          // 自动设置）尚未在本服实现，因此如实下发“未开放”配置。
                                          // 客户端据此显示自带的维护提示，并在次日重新请求。
                                          const auto config =
                                              ClosedEventZoneConfiguration();
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{
                                                  {.opcode = 152U,
                                                   .body = protocol::
                                                       EncodeEventZoneConfig(
                                                           config)}});
                                          return;
                                        }
                                        if (packet.opcode == 952U) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "场景资源报告不允许在当前阶段发送"));
                                            return;
                                          }

                                          // 当前 EXE 与多份原客户端抓包均确认
                                          // C2S952 只携带长度前缀 UTF-16
                                          // 场景名； 旧 Python
                                          // 服务静默忽略且保持连接。该包可能与极短对局的
                                          // Results 转换交错，
                                          // 因而在网关房间上下文内完成严格校验，不把它误当成权威加载或终局命令。
                                          static_cast<void>(
                                              protocol::
                                                  DecodeSceneResourceReportRequest(
                                                      packet.body));
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (packet.opcode == 139U ||
                                            packet.opcode == 1500U ||
                                            IsWorkshopActionOpcode(
                                                packet.opcode)) {
                                          // 原客户端在首次角色页面也会读取科学工坊的静态状态与动态配置。它们不访问玩家
                                          // 资产，允许在 creating_character
                                          // 阶段返回只读响应；真正会改变资产的工坊动作仍
                                          // 严格要求 menu_ready
                                          // 且处于大厅/广场。
                                          if (state->flow.Phase() ==
                                                  runtime::GatewaySessionPhase::
                                                      kCreatingCharacter &&
                                              (packet.opcode == 139U ||
                                               packet.opcode == 1500U)) {
                                            if (packet.opcode == 139U) {
                                              if (!packet.body.empty()) {
                                                complete_once(MakeError(
                                                    application::ErrorCode::
                                                        kInvalidInput,
                                                    "创建角色阶段科学工坊状态请求包含尾随字段"));
                                                return;
                                              }
                                              auto science =
                                                  WorkshopScienceSection(
                                                      state->workshop);
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &science)) {
                                                if (error->code ==
                                                    application::ErrorCode::
                                                        kInvalidState)
                                                  complete_once(
                                                      std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                          Notice(
                                                              error
                                                                  ->safe_message)});
                                                else
                                                  complete_once(*error);
                                                return;
                                              }
                                              complete_once(
                                                  std::vector<
                                                      runtime::OutboundMessage>{
                                                      {.opcode = 151U,
                                                       .body =
                                                           std::get<std::vector<
                                                               std::uint8_t>>(
                                                               std::move(
                                                                   science))}});
                                              return;
                                            }
                                            const auto request = protocol::
                                                DecodeDynamicConfigurationRequest(
                                                    packet.body);
                                            const auto event_zone_section =
                                                EventZoneDynamicSection(
                                                    request.mask);
                                            std::vector<std::uint8_t>
                                                science_section;
                                            if ((request.mask & (1U << 7U)) !=
                                                0U) {
                                              auto science =
                                                  WorkshopScienceSection(
                                                      state->workshop);
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &science)) {
                                                if (error->code !=
                                                    application::ErrorCode::
                                                        kInvalidState) {
                                                  complete_once(*error);
                                                  return;
                                                }
                                              } else {
                                                science_section = std::get<
                                                    std::vector<std::uint8_t>>(
                                                    std::move(science));
                                              }
                                            }
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 1500U,
                                                 .body = protocol::
                                                     EncodeDynamicConfigurationResponse(
                                                         request.mask, {},
                                                         event_zone_section,
                                                         science_section)}});
                                            return;
                                          }
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "科学工坊请求不允许在当前网关阶段执行"));
                                            return;
                                          }
                                          // 139/1500
                                          // 是客户端在进入房间等场景切换后也会自动重读的全局只读配置，不能按
                                          // 会修改资产的工坊动作限制在大厅/广场，否则原客户端刚创建房间就会被断开。
                                          if (packet.opcode == 139U) {
                                            if (!packet.body.empty()) {
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kInvalidInput,
                                                  "科学工坊状态请求包含尾随字段"));
                                              return;
                                            }
                                            auto science =
                                                WorkshopScienceSection(
                                                    state->workshop);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &science)) {
                                              if (error->code ==
                                                  application::ErrorCode::
                                                      kInvalidState)
                                                complete_once(
                                                    std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                                        Notice(
                                                            error
                                                                ->safe_message)});
                                              else
                                                complete_once(*error);
                                              return;
                                            }
                                            complete_once(
                                                std::vector<
                                                    runtime::OutboundMessage>{
                                                    {.opcode = 151U,
                                                     .body =
                                                         std::get<std::vector<
                                                             std::uint8_t>>(
                                                             std::move(
                                                                 science))}});
                                            return;
                                          }
                                          if (packet.opcode == 1500U) {
                                            const auto request = protocol::
                                                DecodeDynamicConfigurationRequest(
                                                    packet.body);
                                            std::vector<std::uint8_t>
                                                minigame_section;
                                            std::vector<std::uint8_t>
                                                science_section;
                                            const auto event_zone_section =
                                                EventZoneDynamicSection(
                                                    request.mask);
                                            if ((request.mask & (1U << 7U)) !=
                                                0U) {
                                              auto science =
                                                  WorkshopScienceSection(
                                                      state->workshop);
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &science)) {
                                                if (error->code !=
                                                    application::ErrorCode::
                                                        kInvalidState) {
                                                  complete_once(*error);
                                                  return;
                                                }
                                              } else {
                                                science_section = std::get<
                                                    std::vector<std::uint8_t>>(
                                                    std::move(science));
                                              }
                                            }
                                            const bool minigames =
                                                state->lobby_minigames !=
                                                    nullptr &&
                                                (request.mask &
                                                 (1U << protocol::
                                                      kLobbyMinigameConfigSection)) !=
                                                    0U;
                                            if (minigames)
                                              minigame_section = protocol::
                                                  EncodeLobbyMinigameConfig(
                                                      state->lobby_minigames
                                                          ->Rules()
                                                          .config);
                                            std::vector<
                                                runtime::OutboundMessage>
                                                replies{
                                                    {.opcode = 1500U,
                                                     .body = protocol::
                                                         EncodeDynamicConfigurationResponse(
                                                             request.mask,
                                                             minigame_section,
                                                             event_zone_section,
                                                             science_section)}};
                                            if (minigames &&
                                                state->flow.PlayerId()) {
                                              // S2C134 gspRepLmgUserRecord
                                              // (B854F0) restores today's count
                                              // and paper.
                                              auto record =
                                                  state->lobby_minigames
                                                      ->Record(
                                                          *state->flow
                                                               .PlayerId());
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &record)) {
                                                complete_once(*error);
                                                return;
                                              }
                                              const auto& value = std::get<
                                                  runtime::LobbyMinigameRecord>(
                                                  record);
                                              replies.push_back(
                                                  {.opcode = protocol::
                                                       kLobbyMinigameRecordOpcode,
                                                   .body = protocol::
                                                       EncodeLobbyMinigameRecord(
                                                           value.fishing_count,
                                                           value.fortune_paper
                                                               .value_or(0U))});
                                            }
                                            complete_once(std::move(replies));
                                            return;
                                          }
                                          if (state->world_state.phase !=
                                                  runtime::WorldSessionPhase::
                                                      kMenu &&
                                              state->world_state.phase !=
                                                  runtime::WorldSessionPhase::
                                                      kPlaza) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "科学工坊只能在大厅或广场使用"));
                                            return;
                                          }
                                          const auto response_opcode =
                                              static_cast<std::uint16_t>(
                                                  packet.opcode + 11U);
                                          if (packet.opcode == 136U) {
                                            const auto request = protocol::
                                                DecodeAdvancedClothingRequest(
                                                    packet.body);
                                            const auto* set =
                                                domain::FindNativeClothingSet(
                                                    request.set_id);
                                            const auto message =
                                                set == nullptr
                                                    ? "无法识别所选仓库服装套装"
                                                : !set->catalog_advancedable
                                                    ? "所选服装套装不支持高级化"
                                                    : "服装高级化发放规则尚未恢复，材料和物品均未扣除";
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = response_opcode,
                                                 .body = protocol::
                                                     EncodeWorkshopFailure()},
                                                Notice(message)});
                                            return;
                                          }
                                          const auto request =
                                              protocol::DecodeWorkshopRequest(
                                                  packet.body);
                                          if (packet.opcode == 134U ||
                                              packet.opcode == 135U) {
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = response_opcode,
                                                 .body = protocol::
                                                     EncodeWorkshopFailure()},
                                                Notice(
                                                    "该科学工坊属性操作尚未恢复，材料和物品均未扣除")});
                                            return;
                                          }
                                          const auto player_id =
                                              state->flow.PlayerId();
                                          if (!player_id.has_value()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInternal,
                                                "已认证 Session 缺少科学工坊玩家标识"));
                                            return;
                                          }
                                          auto operation_result =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          auto result = state->workshop.Execute(
                                              std::get<domain::OperationId>(
                                                  std::move(operation_result)),
                                              *player_id,
                                              static_cast<
                                                  application::WorkshopAction>(
                                                  packet.opcode),
                                              request.input_item_ids,
                                              request.option);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &result)) {
                                            if (!IsClientVisibleTreasureError(
                                                    error->code)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = response_opcode,
                                                 .body = protocol::
                                                     EncodeWorkshopFailure()},
                                                Notice(error->safe_message)});
                                            return;
                                          }
                                          std::vector<runtime::OutboundMessage>
                                              replies{
                                                  {.opcode = response_opcode,
                                                   .body = protocol::
                                                       EncodeWorkshopResult(
                                                           std::get<
                                                               application::
                                                                   WorkshopResult>(
                                                               std::move(
                                                                   result)))}};
                                          // 材料、产物及工坊收据已经在同一事务提交；独立刷新不能把成功改为失败。
                                          // 原132/133/137没有客户端操作UUID，丢掉回执会诱发重复消耗材料。
                                          try {
                                            auto snapshot =
                                                state->menu_snapshots.Load(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &snapshot)) {
                                              logger->Log(
                                                  application::LogLevel::
                                                      kWarning,
                                                  "workshop committed; inventory refresh unavailable: " +
                                                      error->safe_message);
                                              replies.push_back(Notice(
                                                  "工坊操作已提交，仓库刷新暂不可用；请重新读取仓库，不要重复操作。"));
                                            } else {
                                              replies.push_back(
                                                  {.opcode = 802U,
                                                   .body = protocol::
                                                       EncodeInventorySnapshotResponse(
                                                           std::get<
                                                               application::
                                                                   PlayerMenuSnapshot>(
                                                               std::move(
                                                                   snapshot)))});
                                            }
                                          } catch (const std::exception&) {
                                            logger->Log(
                                                application::LogLevel::kWarning,
                                                "workshop committed; inventory refresh failed");
                                            replies.push_back(Notice(
                                                "工坊操作已提交，仓库刷新暂不可用；请重新读取仓库，不要重复操作。"));
                                          }
                                          complete_once(std::move(replies));
                                          return;
                                        }
                                        if (packet.opcode == 129U) {
                                          const auto request = protocol::
                                              DecodeMysteryShopRequest(
                                                  packet.body);
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "神秘商店只能在大厅或广场使用"));
                                            return;
                                          }
                                          // 原客户端进入房间/对局后仍有后台查询；它不代表打开商店或购买。
                                          if (request.action !=
                                                  protocol::MysteryAction::
                                                      kQuery &&
                                              state->world_state.phase !=
                                                  runtime::WorldSessionPhase::
                                                      kMenu &&
                                              state->world_state.phase !=
                                                  runtime::WorldSessionPhase::
                                                      kPlaza) {
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{Notice(
                                                "请先返回大厅或广场操作时空宝盒")});
                                            return;
                                          }
                                          const auto player_id =
                                              state->flow.PlayerId();
                                          if (!player_id.has_value()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInternal,
                                                "已认证 Session 缺少神秘商店玩家标识"));
                                            return;
                                          }
                                          auto rules_result =
                                              state->service_rules.Load();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &rules_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto& mystery_rules =
                                              std::get<
                                                  application::
                                                      PublishedServiceRules>(
                                                  rules_result)
                                                  .mystery_shop;
                                          if (request.action ==
                                                  protocol::MysteryAction::
                                                      kOpen ||
                                              request.action ==
                                                  protocol::MysteryAction::
                                                      kQuery) {
                                            auto selection_result =
                                                state->mystery_shop.State(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &selection_result)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 140U,
                                                 .body =
                                                     request.action ==
                                                             protocol::
                                                                 MysteryAction::
                                                                     kQuery
                                                         ? protocol::EncodeMysteryShopQueryResponse(
                                                               request,
                                                               std::get<
                                                                   application::
                                                                       MysterySelectionSnapshot>(
                                                                   selection_result),
                                                               mystery_rules,
                                                               state->clock
                                                                   .Now())
                                                         : protocol::EncodeMysteryShopResponse(
                                                               request.action,
                                                               std::get<
                                                                   application::
                                                                       MysterySelectionSnapshot>(
                                                                   selection_result),
                                                               mystery_rules,
                                                               state->clock
                                                                   .Now())}});
                                            return;
                                          }
                                          if (request.action ==
                                                  protocol::MysteryAction::
                                                      kRefresh &&
                                              request.value == 1U) {
                                            auto current_result =
                                                state->mystery_shop.State(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &current_result)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            const auto& current = std::get<
                                                application::
                                                    MysterySelectionSnapshot>(
                                                current_result);
                                            if (request.generation <
                                                current.generation) {
                                              // 原版 696640 对 action=3
                                              // 弹成功提示，对 action=2
                                              // 静默更新。
                                              // 过期或已完成的代次只同步，不能再次收费或重复播报刷新结果。
                                              complete_once(std::vector<
                                                            runtime::
                                                                OutboundMessage>{
                                                  {.opcode = 140U,
                                                   .body = protocol::
                                                       EncodeMysteryShopResponse(
                                                           protocol::
                                                               MysteryAction::
                                                                   kQuery,
                                                           current,
                                                           mystery_rules,
                                                           state->clock
                                                               .Now())}});
                                              return;
                                            }
                                          }
                                          auto operation_result =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto operation_id =
                                              std::get<domain::OperationId>(
                                                  std::move(operation_result));
                                          application::ServiceResult<
                                              application::
                                                  MysterySelectionSnapshot>
                                              selection_result = MakeError(
                                                  application::ErrorCode::
                                                      kInvalidInput,
                                                  "神秘商店请求参数无效");
                                          std::optional<domain::StockId>
                                              reward_stock;
                                          bool inventory_changed = false;
                                          if (request.action ==
                                                  protocol::MysteryAction::
                                                      kRefresh &&
                                              request.value == 1U) {
                                            selection_result =
                                                state->mystery_shop.Refresh(
                                                    operation_id, *player_id,
                                                    request.generation);
                                            inventory_changed =
                                                !std::holds_alternative<
                                                    application::ServiceError>(
                                                    selection_result);
                                          } else if (request.action ==
                                                         protocol::
                                                             MysteryAction::
                                                                 kPurchase &&
                                                     request.value == 0U &&
                                                     request.stock_id != 0U) {
                                            auto purchase_result =
                                                state->mystery_shop.Purchase(
                                                    operation_id, *player_id,
                                                    request.generation,
                                                    domain::StockId{
                                                        request.stock_id});
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &purchase_result)) {
                                              selection_result = *error;
                                            } else {
                                              auto purchased = std::get<
                                                  application::
                                                      MysteryPurchaseResult>(
                                                  std::move(purchase_result));
                                              inventory_changed =
                                                  purchased.inventory_changed;
                                              // 696640 -> CF41B0 resolves
                                              // Stock/StockSet (7AFB10 kinds
                                              // 3/4). An item template ID
                                              // cannot populate the native
                                              // reward dialog.
                                              reward_stock = domain::StockId{
                                                  request.stock_id};
                                              selection_result = std::move(
                                                  purchased.selection);
                                            }
                                          }
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &selection_result)) {
                                            logger->Log(
                                                application::LogLevel::kWarning,
                                                "mystery action failed; returning native acknowledgement: " +
                                                    error->safe_message);
                                            auto current =
                                                state->mystery_shop.State(
                                                    *player_id);
                                            if (const auto* state_error =
                                                    std::get_if<
                                                        application::
                                                            ServiceError>(
                                                        &current)) {
                                              complete_once(*state_error);
                                              return;
                                            }
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 140U,
                                                 .body = protocol::
                                                     EncodeMysteryShopFailure(
                                                         request.action,
                                                         std::get<
                                                             application::
                                                                 MysterySelectionSnapshot>(
                                                             current),
                                                         mystery_rules,
                                                         state->clock.Now())},
                                                Notice(error->safe_message)});
                                            return;
                                          }
                                          auto selection = std::get<
                                              application::
                                                  MysterySelectionSnapshot>(
                                              std::move(selection_result));
                                          std::vector<runtime::OutboundMessage>
                                              replies{
                                                  {.opcode = 140U,
                                                   .body = protocol::
                                                       EncodeMysteryShopResponse(
                                                           request.action,
                                                           selection,
                                                           mystery_rules,
                                                           state->clock.Now(),
                                                           reward_stock
                                                                   .has_value()
                                                               ? static_cast<
                                                                     std::
                                                                         uint32_t>(
                                                                     reward_stock
                                                                         ->Value())
                                                               : 0U)}};
                                          if (inventory_changed ||
                                              reward_stock.has_value()) {
                                            // The native140 above is backed by
                                            // the completed purchase/refresh or
                                            // its durable generation/stock
                                            // replay. Only the independent menu
                                            // load/encoding is guarded here.
                                            try {
                                              auto snapshot =
                                                  state->menu_snapshots.Load(
                                                      *player_id);
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &snapshot)) {
                                                logger->Log(
                                                    application::LogLevel::
                                                        kWarning,
                                                    "mystery committed; inventory snapshot unavailable: " +
                                                        error->safe_message);
                                                replies.push_back(Notice(
                                                    "时空宝盒操作已提交，仓库刷新暂不可用；请稍后刷新，不要重"
                                                    "复购买或使用刷新券。"));
                                              } else {
                                                const auto& menu = std::get<
                                                    application::
                                                        PlayerMenuSnapshot>(
                                                    snapshot);
                                                // Encode the complete refresh
                                                // before publishing either105
                                                // or802.
                                                std::vector<
                                                    runtime::OutboundMessage>
                                                    refresh_messages;
                                                if (reward_stock.has_value()) {
                                                  refresh_messages.push_back(
                                                      {.opcode = 105U,
                                                       .body = protocol::
                                                           EncodePlayerProfileResponse(
                                                               menu)});
                                                }
                                                refresh_messages.push_back(
                                                    {.opcode = 802U,
                                                     .body = protocol::
                                                         EncodeInventorySnapshotResponse(
                                                             menu)});
                                                for (auto& refresh_message :
                                                     refresh_messages) {
                                                  replies.push_back(std::move(
                                                      refresh_message));
                                                }
                                              }
                                            } catch (const std::exception&) {
                                              logger->Log(
                                                  application::LogLevel::
                                                      kWarning,
                                                  "mystery committed; inventory snapshot load or encoding threw");
                                              replies.push_back(Notice(
                                                  "时空宝盒操作已提交，仓库刷新暂不可用；请稍后刷新，不要重复购买或使用刷新券。"));
                                            }
                                          }
                                          complete_once(std::move(replies));
                                          return;
                                        }
                                        if (IsTreasureOpcode(packet.opcode)) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              (state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kMenu &&
                                               state->world_state.phase !=
                                                   runtime::WorldSessionPhase::
                                                       kPlaza)) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "泡泡抽取只能在大厅或广场使用"));
                                            return;
                                          }
                                          const auto player_id =
                                              state->flow.PlayerId();
                                          if (!player_id.has_value()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInternal,
                                                "已认证 Session 缺少泡泡玩家标识"));
                                            return;
                                          }
                                          if (packet.opcode == 917U) {
                                            static_cast<void>(
                                                protocol::
                                                    DecodeTreasureStateRequest(
                                                        packet.body));
                                            complete_once(TreasureStateMessages(
                                                *player_id, state->treasure,
                                                state->service_rules,
                                                state->shop_catalog));
                                            return;
                                          }
                                          if (packet.opcode == 920U) {
                                            const auto request = protocol::
                                                DecodeTreasurePreviewRequest(
                                                    packet.body);
                                            auto loaded =
                                                state->service_rules.Load();
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &loaded)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 921U,
                                                 .body = protocol::
                                                     EncodeTreasurePreviewResponse(
                                                         request.needle_item_id,
                                                         std::get<
                                                             application::
                                                                 PublishedServiceRules>(
                                                             loaded)
                                                             .bubbles)}});
                                            return;
                                          }
                                          auto operation_result =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation_result)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const auto operation_id =
                                              std::get<domain::OperationId>(
                                                  std::move(operation_result));
                                          if (packet.opcode == 918U) {
                                            const auto request = protocol::
                                                DecodeTreasureOpenRequest(
                                                    packet.body);
                                            auto result = state->treasure.Open(
                                                operation_id, *player_id,
                                                domain::ItemId{
                                                    request.needle_item_id},
                                                request.primary_position,
                                                request.positions);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &result)) {
                                              complete_once(std::vector<
                                                            runtime::
                                                                OutboundMessage>{
                                                  {.opcode = 919U,
                                                   .body = protocol::
                                                       EncodeTreasureOpenFailure()},
                                                  Notice(error->safe_message)});
                                              return;
                                            }
                                            auto opened = std::get<std::pair<
                                                application::
                                                    TreasureDrawSnapshot,
                                                bool>>(std::move(result));
                                            std::vector<
                                                runtime::OutboundMessage>
                                                replies{
                                                    {.opcode = 919U,
                                                     .body = protocol::
                                                         EncodeTreasureRevealResponse(
                                                             opened.first)}};
                                            if (opened.second) {
                                              // The needle debit and the
                                              // immutable pending draw already
                                              // committed. A separate refresh
                                              // cannot discard its native
                                              // reveal or reroll it.
                                              try {
                                                auto snapshot =
                                                    state->menu_snapshots.Load(
                                                        *player_id);
                                                if (const auto* error =
                                                        std::get_if<
                                                            application::
                                                                ServiceError>(
                                                            &snapshot)) {
                                                  logger->Log(
                                                      application::LogLevel::
                                                          kWarning,
                                                      "bubble draw committed; inventory snapshot unavailable: " +
                                                          error->safe_message);
                                                  replies.push_back(Notice(
                                                      "抽取结果已保存，仓库刷新暂不可用；请重新读取仓库，不要再次扣针抽取。"));
                                                } else {
                                                  replies.push_back(
                                                      {.opcode = 802U,
                                                       .body = protocol::
                                                           EncodeInventorySnapshotResponse(
                                                               std::get<
                                                                   application::
                                                                       PlayerMenuSnapshot>(
                                                                   std::move(
                                                                       snapshot)))});
                                                }
                                              } catch (const std::exception&) {
                                                logger->Log(
                                                    application::LogLevel::
                                                        kWarning,
                                                    "bubble draw committed; inventory snapshot refresh failed");
                                                replies.push_back(Notice(
                                                    "抽取结果已保存，仓库刷新暂不可用；请重新读取仓库，不要再次扣针抽取。"));
                                              }
                                            }
                                            complete_once(std::move(replies));
                                            return;
                                          }
                                          const auto request = protocol::
                                              DecodeTreasureClaimRequest(
                                                  packet.body);
                                          auto result = state->treasure.Claim(
                                              operation_id, *player_id,
                                              request.choice, request.position);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &result)) {
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 920U,
                                                 .body = protocol::
                                                     EncodeTreasureClaimFailure()},
                                                Notice(error->safe_message)});
                                            return;
                                          }
                                          std::vector<runtime::OutboundMessage> replies{
                                              {.opcode = 920U,
                                               .body = protocol::
                                                   EncodeTreasureClaimResponse(
                                                       std::get<
                                                           application::
                                                               TreasureClaimResult>(
                                                           std::move(
                                                               result)))}};
                                          // Preserve the committed grant
                                          // receipt, including an exact
                                          // persisted replay. Never convert a
                                          // failed read/encoder into another
                                          // grant or a native failure.
                                          try {
                                            auto snapshot =
                                                state->menu_snapshots.Load(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &snapshot)) {
                                              logger->Log(
                                                  application::LogLevel::
                                                      kWarning,
                                                  "bubble claim committed; inventory snapshot unavailable: " +
                                                      error->safe_message);
                                              replies.push_back(Notice(
                                                  "奖励领取已提交，仓库刷新暂不可用；请重新读取仓库，不要重复领取。"));
                                            } else {
                                              replies.push_back(
                                                  {.opcode = 802U,
                                                   .body = protocol::
                                                       EncodeInventorySnapshotResponse(
                                                           std::get<
                                                               application::
                                                                   PlayerMenuSnapshot>(
                                                               std::move(
                                                                   snapshot)))});
                                            }
                                          } catch (const std::exception&) {
                                            logger->Log(
                                                application::LogLevel::kWarning,
                                                "bubble claim committed; inventory snapshot refresh failed");
                                            replies.push_back(Notice(
                                                "奖励领取已提交，仓库刷新暂不可用；请重新读取仓库，不要重复领取。"));
                                          }
                                          complete_once(std::move(replies));
                                          return;
                                        }
                                        if (packet.opcode == 119U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "查看战士资料需要完成角色登录"));
                                            return;
                                          }
                                          const auto request = protocol::
                                              DecodeFighterInfoRequest(
                                                  packet.body);
                                          if (request.requester !=
                                              *state->flow.PlayerId()) {
                                            complete_once(std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                {.opcode = 125U,
                                                 .body = protocol::
                                                     EncodeFighterInfoResponse(
                                                         nullptr)}});
                                            return;
                                          }
                                          auto loaded =
                                              state->menu_snapshots.Load(
                                                  request.target);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            if (error->code ==
                                                application::ErrorCode::
                                                    kInvalidState) {
                                              complete_once(std::vector<
                                                            runtime::
                                                                OutboundMessage>{
                                                  {.opcode = 125U,
                                                   .body = protocol::
                                                       EncodeFighterInfoResponse(
                                                           nullptr)}});
                                            } else {
                                              complete_once(*error);
                                            }
                                            return;
                                          }
                                          const auto& snapshot = std::get<
                                              application::PlayerMenuSnapshot>(
                                              loaded);
                                          std::optional<
                                              application::PlayerMenuSnapshot>
                                              partner;
                                          if (snapshot.couple) {
                                            auto value =
                                                state->menu_snapshots.Load(
                                                    snapshot.couple
                                                        ->partner_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &value)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            partner = std::get<
                                                application::
                                                    PlayerMenuSnapshot>(
                                                std::move(value));
                                          }
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 806U,
                                               .body = protocol::
                                                   EncodeEquippedItemsResponse(
                                                       snapshot)},
                                              {.opcode = 125U,
                                               .body = protocol::
                                                   EncodeFighterInfoResponse(
                                                       &snapshot,
                                                       partner ? &*partner
                                                               : nullptr)}});
                                          return;
                                        }
                                        if (packet.opcode == 120U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "查看战士称号需要完成角色登录"));
                                            return;
                                          }
                                          const auto request = protocol::
                                              DecodeFighterTitleRequest(
                                                  packet.body);
                                          auto loaded =
                                              state->menu_snapshots.Load(
                                                  request.target);
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            if (error->code ==
                                                application::ErrorCode::
                                                    kInvalidState) {
                                              complete_once(std::vector<
                                                            runtime::
                                                                OutboundMessage>{
                                                  {.opcode = 126U,
                                                   .body = protocol::
                                                       EncodeFighterTitleFailure(
                                                           request.target,
                                                           protocol::FighterTitleFailure::
                                                               kTargetMissing)}});
                                            } else {
                                              complete_once(*error);
                                            }
                                            return;
                                          }
                                          // 仅使用已读取的原生累计事件；未知数据源不能冒充零事件成功。
                                          complete_once(std::vector<
                                                        runtime::
                                                            OutboundMessage>{
                                              {.opcode = 126U,
                                               .body = protocol::
                                                   EncodeNativeMedalSnapshot(
                                                       std::get<
                                                           application::
                                                               PlayerMenuSnapshot>(
                                                           loaded))}});
                                          return;
                                        }
                                        if (packet.opcode == 520U) {
                                          if (state->flow.Phase() !=
                                                  runtime::GatewaySessionPhase::
                                                      kMenuReady ||
                                              !state->flow.PlayerId()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kUnauthenticated,
                                                "喇叭需要完成角色登录"));
                                            return;
                                          }
                                          const auto request =
                                              protocol::DecodeMegaphoneRequest(
                                                  packet.body);
                                          auto operation =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          application::MegaphoneCommand command{
                                              .operation_id =
                                                  std::get<domain::OperationId>(
                                                      std::move(operation)),
                                              .player_id =
                                                  *state->flow.PlayerId(),
                                              .type = request.type,
                                              .selection = request.selection,
                                              .text = request.text};
                                          auto work =
                                              [state](
                                                  const application::
                                                      MegaphoneCommand& frozen)
                                              -> application::ServiceResult<
                                                  runtime::
                                                      MegaphoneWorldResult> {
                                            auto access =
                                                state->world_access.Load(
                                                    frozen.player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &access))
                                              return *error;
                                            runtime::MegaphoneWorldResult
                                                result;
                                            const auto& moderation = std::get<
                                                application::
                                                    WorldAccessSnapshot>(
                                                access);
                                            if (moderation.muted ||
                                                moderation.banned) {
                                              result.replies.push_back(Notice(
                                                  "账号处于禁言或封禁期间，暂时无法使用喇叭"));
                                              return result;
                                            }
                                            result.blocked =
                                                std::get<
                                                    application::
                                                        WorldAccessSnapshot>(
                                                    std::move(access))
                                                    .chat_blocked_peers;
                                            auto used =
                                                state->item_use.Megaphone(
                                                    frozen);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &used)) {
                                              if (!IsClientVisibleTreasureError(
                                                      error->code))
                                                return *error;
                                              result.replies.push_back(
                                                  Notice(error->safe_message));
                                              return result;
                                            }
                                            result.receipt = std::get<
                                                application::MegaphoneReceipt>(
                                                std::move(used));
                                            auto loaded =
                                                state->menu_snapshots.Load(
                                                    frozen.player_id);
                                            if (const auto* snapshot =
                                                    std::get_if<
                                                        application::
                                                            PlayerMenuSnapshot>(
                                                        &loaded)) {
                                              result.replies.push_back(
                                                  {.opcode = 802U,
                                                   .body = protocol::
                                                       EncodeInventorySnapshotResponse(
                                                           *snapshot)});
                                            }
                                            return result;
                                          };
                                          auto finished =
                                              [complete_once](
                                                  application::ServiceResult<
                                                      std::vector<
                                                          runtime::
                                                              OutboundMessage>>
                                                      result) mutable {
                                                if (const auto* error =
                                                        std::get_if<
                                                            application::
                                                                ServiceError>(
                                                            &result);
                                                    error &&
                                                    IsClientVisibleTreasureError(
                                                        error->code)) {
                                                  complete_once(
                                                      std::vector<
                                                          runtime::
                                                              OutboundMessage>{
                                                          Notice(
                                                              error
                                                                  ->safe_message)});
                                                } else
                                                  complete_once(
                                                      std::move(result));
                                              };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostMegaphone(
                                                          std::move(command),
                                                          std::move(work),
                                                          finished))
                                            finished(*error);
                                          return;
                                        }
                                        if (packet.opcode == 714U &&
                                            state->flow.Phase() ==
                                                runtime::GatewaySessionPhase::
                                                    kMenuReady &&
                                            state->flow.PlayerId()
                                                .has_value()) {
                                          protocol::PacketReader reader{
                                              packet.body};
                                          const auto object = reader.ReadU32();
                                          reader.RequireEnd();
                                          auto work =
                                              [state](
                                                  const application::
                                                      UseItemCommand& command)
                                              -> application::ServiceResult<
                                                  runtime::
                                                      InstantRespawnWorldResult> {
                                            auto used =
                                                state->item_use.Use(command);
                                            runtime::InstantRespawnWorldResult
                                                result;
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &used)) {
                                              if (!IsClientVisibleTreasureError(
                                                      error->code)) {
                                                return *error;
                                              }
                                              result.replies.push_back(
                                                  Notice(error->safe_message));
                                            } else {
                                              result.authorized = true;
                                            }
                                            auto loaded =
                                                state->menu_snapshots.Load(
                                                    command.player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &loaded)) {
                                              // 扣费已经提交时仍必须安装授权；刷新失败不能把已付费的生命锁死。
                                              if (!result.authorized) {
                                                return *error;
                                              }
                                            } else {
                                              result.replies.push_back(
                                                  {.opcode = 802U,
                                                   .body = protocol::
                                                       EncodeInventorySnapshotResponse(
                                                           std::get<
                                                               application::
                                                                   PlayerMenuSnapshot>(
                                                               std::move(
                                                                   loaded)))});
                                            }
                                            return result;
                                          };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostInstantRespawn(
                                                          *state->flow
                                                               .PlayerId(),
                                                          object,
                                                          std::move(work),
                                                          complete_once)) {
                                            complete_once(*error);
                                          }
                                          return;
                                        }
                                        if (IsWorldOpcode(packet.opcode)) {
                                          if (state->flow.Phase() !=
                                              runtime::GatewaySessionPhase::
                                                  kMenuReady) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInvalidState,
                                                "世界命令只能在菜单初始化后发送"));
                                            return;
                                          }
                                          const auto player_id =
                                              state->flow.PlayerId();
                                          if (!player_id.has_value()) {
                                            complete_once(MakeError(
                                                application::ErrorCode::
                                                    kInternal,
                                                "已认证 Session 缺少世界玩家标识"));
                                            return;
                                          }
                                          runtime::WorldActorContext actor{
                                              .player_id = *player_id,
                                              .state = state->world_state,
                                              .orig_session =
                                                  std::pair{
                                                      state->handle.id,
                                                      state->handle.generation},
                                              .peer_address =
                                                  state->peer_address};
                                          if (WorldOpcodeRequiresMenuSnapshot(
                                                  packet.opcode)) {
                                            auto snapshot =
                                                state->menu_snapshots.Load(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &snapshot)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            actor.player_snapshot = std::get<
                                                application::
                                                    PlayerMenuSnapshot>(
                                                std::move(snapshot));
                                          }
                                          if (WorldOpcodeRequiresAccessSnapshot(
                                                  packet.opcode)) {
                                            auto access =
                                                state->world_access.Load(
                                                    *player_id);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &access)) {
                                              complete_once(*error);
                                              return;
                                            }
                                            auto snapshot = std::get<
                                                application::
                                                    WorldAccessSnapshot>(
                                                std::move(access));
                                            if (snapshot.banned) {
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kUnauthenticated,
                                                  "账号或当前机器已被封禁"));
                                              return;
                                            }
                                            actor.muted = snapshot.muted;
                                            actor.has_clan_membership =
                                                snapshot.has_clan_membership;
                                            actor
                                                .chat_blocked_peers = std::move(
                                                snapshot.chat_blocked_peers);
                                            actor.has_chat_penalty_history =
                                                snapshot
                                                    .has_chat_penalty_history;
                                          }
                                          const bool release_frame =
                                              TutorialPersistenceFrame(
                                                  packet.opcode,
                                                  state->world_state.phase);
                                          if (release_frame)
                                            ValidateTutorialPersistenceFrame(
                                                packet);
                                          auto world_completion =
                                              release_frame
                                                  ? TutorialLateCompletion(
                                                        state, *player_id)
                                                  : runtime::
                                                        WorldCommandCompletion{
                                                            complete_once};
                                          if (const auto error =
                                                  state->world_executor.Post(
                                                      std::move(actor),
                                                      std::move(packet),
                                                      std::move(
                                                          world_completion))) {
                                            complete_once(*error);
                                          } else if (release_frame) {
                                            complete_once(
                                                std::vector<
                                                    runtime::
                                                        OutboundMessage>{});  // Keep reading to
                                                                              // detect H1 close
                                                                              // during SQL.
                                          }
                                          return;
                                        }
                                        if (packet.opcode == 808U &&
                                            state->flow.Phase() ==
                                                runtime::GatewaySessionPhase::
                                                    kMenuReady &&
                                            state->flow.PlayerId()) {
                                          const auto request = protocol::
                                              DecodeExpiredItemDiscardRequest(
                                                  packet.body);
                                          const auto player =
                                              *state->flow.PlayerId();
                                          auto operation =
                                              state->purchase_operation_ids
                                                  .Next();
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &operation)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          const application::
                                              ExpiredItemDiscardCommand command{
                                                  .operation_id = std::get<
                                                      domain::OperationId>(
                                                      std::move(operation)),
                                                  .player_id = player,
                                                  .native_item_key =
                                                      request.native_item_key};
                                          auto work =
                                              [state, command, logger](
                                                  bool allowed,
                                                  std::uint32_t) -> application::
                                                                     ServiceResult<
                                                                         runtime::
                                                                             EquipmentWorldResult> {
                                                                       bool success =
                                                                           false;
                                                                       std::string
                                                                           reason =
                                                                               "当前阶段不允许删弃过期物品";
                                                                       if (allowed) {
                                                                         auto result =
                                                                             state
                                                                                 ->item_use
                                                                                 .DiscardExpired(
                                                                                     command);
                                                                         if (const auto* error =
                                                                                 std::get_if<
                                                                                     application::
                                                                                         ServiceError>(
                                                                                     &result)) {
                                                                           if (!IsClientVisibleTreasureError(
                                                                                   error
                                                                                       ->code))
                                                                             return *error;
                                                                           reason =
                                                                               error
                                                                                   ->safe_message;
                                                                         } else
                                                                           success =
                                                                               true;
                                                                       }
                                                                       std::vector<
                                                                           runtime::
                                                                               OutboundMessage>
                                                                           replies{
                                                                               {.opcode =
                                                                                    813U,
                                                                                .body = protocol::EncodeExpiredItemDiscardResponse(
                                                                                    command
                                                                                        .native_item_key,
                                                                                    success)}};
                                                                       if (!success)
                                                                         replies.push_back(
                                                                             Notice(
                                                                                 reason));
                                                                       application::ServiceResult<
                                                                           application::
                                                                               PlayerMenuSnapshot>
                                                                           loaded;
                                                                       try {
                                                                         loaded =
                                                                             state
                                                                                 ->menu_snapshots
                                                                                 .Load(
                                                                                     command
                                                                                         .player_id);
                                                                       } catch (
                                                                           const std::
                                                                               exception&) {
                                                                         loaded = MakeError(
                                                                             application::
                                                                                 ErrorCode::
                                                                                     kStorageUnavailable,
                                                                             "删弃后的仓库刷新失败");
                                                                       }
                                                                       if (const auto* error =
                                                                               std::get_if<
                                                                                   application::
                                                                                       ServiceError>(
                                                                                   &loaded)) {
                                                                         if (!success)
                                                                           return *error;
                                                                         logger->Log(
                                                                             application::
                                                                                 LogLevel::
                                                                                     kWarning,
                                                                             "expired discard committed; inventory refresh unavailable: " +
                                                                                 error
                                                                                     ->safe_message);
                                                                         replies.push_back(
                                                                             Notice(
                                                                                 "过期删弃已提交，仓库刷新暂不可用，请稍后发送803刷新"));
                                                                         return runtime::EquipmentWorldResult{
                                                                             .replies =
                                                                                 std::move(
                                                                                     replies)};
                                                                       }
                                                                       auto snapshot = std::get<
                                                                           application::
                                                                               PlayerMenuSnapshot>(
                                                                           std::move(
                                                                               loaded));
                                                                       std::vector<
                                                                           runtime::
                                                                               OutboundMessage>
                                                                           refresh;
                                                                       try {
                                                                         refresh.push_back(
                                                                             {.opcode =
                                                                                  802U,
                                                                              .body = protocol::
                                                                                  EncodeInventorySnapshotResponse(
                                                                                      snapshot)});
                                                                         refresh.push_back(
                                                                             {.opcode =
                                                                                  835U,
                                                                              .body = protocol::
                                                                                  EncodeWeaponPresetsResponse(
                                                                                      snapshot)});
                                                                         refresh.push_back(
                                                                             {.opcode =
                                                                                  806U,
                                                                              .body = protocol::
                                                                                  EncodeEquippedItemsResponse(
                                                                                      snapshot)});
                                                                       } catch (
                                                                           const std::
                                                                               exception&) {
                                                                         if (!success)
                                                                           return MakeError(
                                                                               application::
                                                                                   ErrorCode::
                                                                                       kInternal,
                                                                               "删弃后的仓库编码失败");
                                                                         logger->Log(
                                                                             application::
                                                                                 LogLevel::
                                                                                     kWarning,
                                                                             "expired discard committed; inventory refresh encoding failed");
                                                                         replies.push_back(
                                                                             Notice(
                                                                                 "过期删弃已提交，仓库刷新暂不可用，请稍后发送803刷新"));
                                                                         return runtime::EquipmentWorldResult{
                                                                             .replies =
                                                                                 std::move(
                                                                                     replies)};
                                                                       }
                                                                       for (
                                                                           auto&
                                                                               message :
                                                                           refresh)
                                                                         replies.push_back(
                                                                             std::move(
                                                                                 message));
                                                                       // No
                                                                       // changed
                                                                       // snapshot:
                                                                       // deleted
                                                                       // expired
                                                                       // refs
                                                                       // were
                                                                       // already
                                                                       // absent
                                                                       // from
                                                                       // active
                                                                       // loadouts.
                                                                       // Keep
                                                                       // room
                                                                       // ready/model
                                                                       // state
                                                                       // and
                                                                       // deliver
                                                                       // native813
                                                                       // only
                                                                       // to the
                                                                       // originating
                                                                       // socket.
                                                                       return runtime::EquipmentWorldResult{
                                                                           .replies =
                                                                               std::move(
                                                                                   replies)};
                                                                     };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle))
                                            complete_once(*error);
                                          return;
                                        }
                                        if ((packet.opcode == 806U ||
                                             packet.opcode == 143U) &&
                                            state->flow.Phase() ==
                                                runtime::GatewaySessionPhase::
                                                    kMenuReady &&
                                            state->flow.PlayerId()
                                                .has_value()) {
                                          protocol::PacketReader reader{
                                              packet.body};
                                          application::UseItemCommand command;
                                          command.player_id =
                                              *state->flow.PlayerId();
                                          command.item_id =
                                              domain::ItemId{reader.ReadU32()};
                                          if (packet.opcode == 143U) {
                                            command.operation = application::
                                                ItemUseOperation::kPetLevelUp;
                                          } else {
                                            command.quantity = reader.ReadU32();
                                            command.target =
                                                protocol::Utf16ToUtf8(
                                                    reader.ReadUtf16(64U));
                                          }
                                          reader.RequireEnd();
                                          auto
                                              work = [state, command,
                                                      logger](bool allowed,
                                                              std::uint32_t) -> application::
                                                                                 ServiceResult<
                                                                                     runtime::
                                                                                         EquipmentWorldResult> {
                                                                                   bool success =
                                                                                       false;
                                                                                   bool equipment_restored =
                                                                                       false;
                                                                                   auto use_status =
                                                                                       std::numeric_limits<
                                                                                           std::
                                                                                               uint32_t>::
                                                                                           max();
                                                                                   std::string
                                                                                       message =
                                                                                           "当前阶段不能使用道具";
                                                                                   if (allowed) {
                                                                                     auto result =
                                                                                         state
                                                                                             ->item_use
                                                                                             .Use(
                                                                                                 command);
                                                                                     if (const auto* error =
                                                                                             std::get_if<
                                                                                                 application::
                                                                                                     ServiceError>(
                                                                                                 &result)) {
                                                                                       const bool no_life_target =
                                                                                           command.item_id
                                                                                                   .Value() ==
                                                                                               190144U &&
                                                                                           error->code ==
                                                                                               application::
                                                                                                   ErrorCode::
                                                                                                       kInvalidState;
                                                                                       if (!IsClientVisibleTreasureError(
                                                                                               error
                                                                                                   ->code) &&
                                                                                           !no_life_target) {
                                                                                         return *error;
                                                                                       }
                                                                                       message =
                                                                                           error
                                                                                               ->safe_message;
                                                                                       if (no_life_target) {
                                                                                         use_status =
                                                                                             14U;  // B70890 function18: no applicable Life
                                                                                                   // items, no needle spent.
                                                                                       }
                                                                                       if (command
                                                                                               .item_id
                                                                                               .Value() ==
                                                                                           100104U) {
                                                                                         if (error
                                                                                                 ->code ==
                                                                                             application::
                                                                                                 ErrorCode::
                                                                                                     kConflict) {
                                                                                           use_status =
                                                                                               9U;  // Native name-already-used dialog.
                                                                                         } else if (
                                                                                             error->code ==
                                                                                                 application::
                                                                                                     ErrorCode::
                                                                                                         kInvalidInput &&
                                                                                             command.quantity ==
                                                                                                 1U) {
                                                                                           const auto parsed =
                                                                                               domain::Nickname::
                                                                                                   Parse(
                                                                                                       command
                                                                                                           .target);
                                                                                           const auto* invalid =
                                                                                               std::get_if<
                                                                                                   domain::
                                                                                                       NicknameValidationError>(
                                                                                                   &parsed);
                                                                                           use_status =
                                                                                               !invalid
                                                                                                   ? 10U
                                                                                                   : (*invalid ==
                                                                                                              domain::
                                                                                                                  NicknameValidationError::
                                                                                                                      kInvalidLength
                                                                                                          ? 11U
                                                                                                          : 12U);
                                                                                         }
                                                                                       }
                                                                                     } else {
                                                                                       success =
                                                                                           true;
                                                                                       equipment_restored =
                                                                                           std::get<
                                                                                               application::
                                                                                                   UseItemResult>(
                                                                                               result)
                                                                                               .equipment_restored;
                                                                                       message
                                                                                           .clear();
                                                                                     }
                                                                                   }
                                                                                   protocol::PacketWriter
                                                                                       response;
                                                                                   const bool level_up =
                                                                                       command
                                                                                           .operation ==
                                                                                       application::
                                                                                           ItemUseOperation::
                                                                                               kPetLevelUp;
                                                                                   std::vector<
                                                                                       runtime::
                                                                                           OutboundMessage>
                                                                                       replies;
                                                                                   if (level_up) {
                                                                                     response
                                                                                         .WriteU32(static_cast<
                                                                                                   std::
                                                                                                       uint32_t>(
                                                                                             command
                                                                                                 .item_id
                                                                                                 .Value()));
                                                                                     response
                                                                                         .WriteU32(
                                                                                             success
                                                                                                 ? 0U
                                                                                                 : use_status);
                                                                                   } else {
                                                                                     response
                                                                                         .WriteU32(
                                                                                             success
                                                                                                 ? 0U
                                                                                                 : use_status);
                                                                                     response
                                                                                         .WriteU32(static_cast<
                                                                                                   std::
                                                                                                       uint32_t>(
                                                                                             command
                                                                                                 .item_id
                                                                                                 .Value()));
                                                                                     response
                                                                                         .WriteUtf16(
                                                                                             protocol::Utf8ToUtf16(
                                                                                                 message));
                                                                                     replies
                                                                                         .push_back(
                                                                                             {.opcode =
                                                                                                  811U,
                                                                                              .body =
                                                                                                  response
                                                                                                      .Release()});
                                                                                   }
                                                                                   application::ServiceResult<
                                                                                       application::
                                                                                           PlayerMenuSnapshot>
                                                                                       loaded;
                                                                                   try {
                                                                                     loaded =
                                                                                         state
                                                                                             ->menu_snapshots
                                                                                             .Load(
                                                                                                 command
                                                                                                     .player_id);
                                                                                   } catch (
                                                                                       const std::
                                                                                           exception&) {
                                                                                     loaded = MakeError(
                                                                                         application::
                                                                                             ErrorCode::
                                                                                                 kStorageUnavailable,
                                                                                         "已提交道具操作的快照读取失败");
                                                                                   }
                                                                                   if (const auto* error =
                                                                                           std::get_if<
                                                                                               application::
                                                                                                   ServiceError>(
                                                                                               &loaded)) {
                                                                                     if (!success)
                                                                                       return *error;
                                                                                     // Tx
                                                                                     // success
                                                                                     // is
                                                                                     // not
                                                                                     // a
                                                                                     // failed
                                                                                     // use
                                                                                     // just
                                                                                     // because
                                                                                     // this
                                                                                     // independent
                                                                                     // read
                                                                                     // failed.
                                                                                     // Preserve
                                                                                     // the
                                                                                     // native
                                                                                     // 811/156
                                                                                     // acknowledgement;
                                                                                     // 806
                                                                                     // has
                                                                                     // no
                                                                                     // client
                                                                                     // operation
                                                                                     // ID.
                                                                                     logger
                                                                                         ->Log(
                                                                                             application::
                                                                                                 LogLevel::
                                                                                                     kWarning,
                                                                                             "item use committed; snapshot unavailable: " +
                                                                                                 error
                                                                                                     ->safe_message);
                                                                                     if (level_up)
                                                                                       replies
                                                                                           .push_back(
                                                                                               {.opcode =
                                                                                                    156U,
                                                                                                .body =
                                                                                                    response
                                                                                                        .Release()});
                                                                                     replies
                                                                                         .push_back(Notice(
                                                                                             "道具操作已提交，仓库刷新暂不可用；请稍后刷新，不要重复使用道具。"));
                                                                                     return runtime::EquipmentWorldResult{
                                                                                         .replies =
                                                                                             std::move(
                                                                                                 replies)};
                                                                                   }
                                                                                   auto snapshot = std::get<
                                                                                       application::
                                                                                           PlayerMenuSnapshot>(
                                                                                       std::move(
                                                                                           loaded));
                                                                                   const bool clan_expansion =
                                                                                       command.operation ==
                                                                                           application::
                                                                                               ItemUseOperation::
                                                                                                   kUse &&
                                                                                       command.item_id
                                                                                               .Value() ==
                                                                                           101062U;
                                                                                   const bool inventory_only =
                                                                                       clan_expansion ||
                                                                                       (command.item_id
                                                                                                .Value() ==
                                                                                            190144U &&
                                                                                        !equipment_restored);
                                                                                   std::vector<
                                                                                       runtime::
                                                                                           OutboundMessage>
                                                                                       refresh;
                                                                                   try {
                                                                                     if (success &&
                                                                                         !inventory_only) {
                                                                                       refresh
                                                                                           .push_back(
                                                                                               {.opcode =
                                                                                                    105U,
                                                                                                .body = protocol::
                                                                                                    EncodePlayerProfileResponse(
                                                                                                        snapshot)});
                                                                                       refresh
                                                                                           .push_back(
                                                                                               {.opcode =
                                                                                                    806U,
                                                                                                .body = protocol::
                                                                                                    EncodeEquippedItemsResponse(
                                                                                                        snapshot)});
                                                                                     } else if (
                                                                                         !success) {
                                                                                       refresh
                                                                                           .push_back(Notice(
                                                                                               message));
                                                                                     }
                                                                                     refresh
                                                                                         .push_back(
                                                                                             {.opcode =
                                                                                                  802U,
                                                                                              .body = protocol::
                                                                                                  EncodeInventorySnapshotResponse(
                                                                                                      snapshot)});
                                                                                   } catch (
                                                                                       const std::
                                                                                           exception&) {
                                                                                     if (!success)
                                                                                       return MakeError(
                                                                                           application::
                                                                                               ErrorCode::
                                                                                                   kInternal,
                                                                                           "道具操作后的快照编码失败");
                                                                                     logger
                                                                                         ->Log(
                                                                                             application::
                                                                                                 LogLevel::
                                                                                                     kWarning,
                                                                                             "item use committed; snapshot encoding failed");
                                                                                     if (level_up)
                                                                                       replies
                                                                                           .push_back(
                                                                                               {.opcode =
                                                                                                    156U,
                                                                                                .body =
                                                                                                    response
                                                                                                        .Release()});
                                                                                     replies
                                                                                         .push_back(Notice(
                                                                                             "道具操作已提交，仓库刷新暂不可用；请稍后刷新，不要重复使用道具。"));
                                                                                     return runtime::EquipmentWorldResult{
                                                                                         .replies =
                                                                                             std::move(
                                                                                                 replies)};
                                                                                   }
                                                                                   if (success) {
                                                                                     if (command
                                                                                             .item_id
                                                                                             .Value() ==
                                                                                         100104U) {
                                                                                       try {
                                                                                         {
                                                                                           std::scoped_lock
                                                                                               identity_lock{
                                                                                                   state
                                                                                                       ->mutex};
                                                                                           state
                                                                                               ->flow
                                                                                               .UpdateNickname(
                                                                                                   snapshot
                                                                                                       .nickname);
                                                                                         }
                                                                                         auto presence = runtime::SocialPresenceChanged(
                                                                                             command
                                                                                                 .player_id,
                                                                                             state
                                                                                                 ->social_snapshots,
                                                                                             state
                                                                                                 ->session_registry);
                                                                                         if (auto* value =
                                                                                                 std::get_if<
                                                                                                     runtime::
                                                                                                         SocialCommandResult>(
                                                                                                     &presence))
                                                                                           static_cast<
                                                                                               void>(
                                                                                               state
                                                                                                   ->session_registry
                                                                                                   .Dispatch(std::move(
                                                                                                       *value)));
                                                                                         if (state
                                                                                                 ->couples) {
                                                                                           auto changed =
                                                                                               state
                                                                                                   ->couples
                                                                                                   ->IdentityChanged(
                                                                                                       command
                                                                                                           .player_id,
                                                                                                       state
                                                                                                           ->handle,
                                                                                                       [state,
                                                                                                        logger](
                                                                                                           runtime::CoupleFlowResult&
                                                                                                               value) {
                                                                                                         PublishCouple(
                                                                                                             state,
                                                                                                             logger,
                                                                                                             value);
                                                                                                       });
                                                                                           if (const auto* error =
                                                                                                   std::get_if<
                                                                                                       application::
                                                                                                           ServiceError>(
                                                                                                       &changed))
                                                                                             logger
                                                                                                 ->Log(
                                                                                                     application::
                                                                                                         LogLevel::
                                                                                                             kWarning,
                                                                                                     "rename couple refresh failed: " +
                                                                                                         error
                                                                                                             ->safe_message);
                                                                                         }
                                                                                       } catch (
                                                                                           const std::
                                                                                               exception&) {
                                                                                         logger
                                                                                             ->Log(
                                                                                                 application::
                                                                                                     LogLevel::
                                                                                                         kWarning,
                                                                                                 "rename committed; identity notification refresh failed");
                                                                                       }
                                                                                     }
                                                                                   }
                                                                                   for (
                                                                                       auto&
                                                                                           refresh_message :
                                                                                       refresh)
                                                                                     replies
                                                                                         .push_back(
                                                                                             std::move(
                                                                                                 refresh_message));
                                                                                   if (level_up) {
                                                                                     // B812D0/60A060
                                                                                     // 在反馈中直接读取本地宠物；先同步已提交的新属性。
                                                                                     replies
                                                                                         .push_back(
                                                                                             {.opcode =
                                                                                                  156U,
                                                                                              .body =
                                                                                                  response
                                                                                                      .Release()});
                                                                                   }
                                                                                   return runtime::EquipmentWorldResult{
                                                                                       .replies =
                                                                                           std::move(
                                                                                               replies),
                                                                                       // Clan
                                                                                       // capacity
                                                                                       // and
                                                                                       // Life
                                                                                       // counts
                                                                                       // retain
                                                                                       // ready/loadout
                                                                                       // state.
                                                                                       // Native
                                                                                       // function24
                                                                                       // refreshes
                                                                                       // clan
                                                                                       // state
                                                                                       // via
                                                                                       // Nx1098;
                                                                                       // syringe
                                                                                       // function18
                                                                                       // consumes
                                                                                       // the
                                                                                       // new802.
                                                                                       .changed =
                                                                                           success &&
                                                                                                   !inventory_only
                                                                                               ? std::optional{std::move(
                                                                                                     snapshot)}
                                                                                               : std::
                                                                                                     nullopt,
                                                                                       .refresh_menu_slot =
                                                                                           success &&
                                                                                           command.item_id
                                                                                                   .Value() ==
                                                                                               100104U};
                                                                                 };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          command.player_id,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle)) {
                                            complete_once(*error);
                                          }
                                          return;
                                        }
                                        if (packet.opcode == 957U &&
                                            state->flow.PlayerId()
                                                .has_value()) {
                                          const auto player =
                                              *state->flow.PlayerId();
                                          protocol::PacketReader reader{
                                              packet.body};
                                          const auto version = reader.ReadU32();
                                          const auto card = reader.ReadU32();
                                          const auto mark = reader.ReadU32();
                                          const auto quantity =
                                              reader.ReadU32();
                                          reader.RequireEnd();
                                          auto work = [state, player, version,
                                                       card, mark,
                                                       quantity](bool allowed,
                                                                 std::uint32_t)
                                              -> application::ServiceResult<
                                                  runtime::
                                                      EquipmentWorldResult> {
                                            std::int32_t status = -1;
                                            std::uint32_t consumed = 0U;
                                            std::vector<domain::PlayerId>
                                                members;
                                            std::vector<
                                                runtime::OutboundMessage>
                                                replies;
                                            if (allowed) {
                                              auto result =
                                                  state->clans.ChangeMark(
                                                      player, version, card,
                                                      mark, quantity);
                                              if (const auto* error =
                                                      std::get_if<
                                                          application::
                                                              ServiceError>(
                                                          &result)) {
                                                if (!IsClientVisibleTreasureError(
                                                        error->code)) {
                                                  return *error;
                                                }
                                                replies.push_back(Notice(
                                                    error->safe_message));
                                              } else {
                                                status = 0;
                                                consumed =
                                                    std::get<
                                                        application::
                                                            ClanMarkResult>(
                                                        result)
                                                        .consumed_cards;
                                                auto roster =
                                                    state->clans.Roster(player);
                                                if (const auto* roster_error =
                                                        std::get_if<
                                                            application::
                                                                ServiceError>(
                                                            &roster)) {
                                                  return *roster_error;
                                                }
                                                for (const auto& member :
                                                     std::get<std::vector<
                                                         application::
                                                             ClanMemberView>>(
                                                         roster)) {
                                                  members.push_back(
                                                      member.player_id);
                                                }
                                              }
                                            }
                                            protocol::PacketWriter response;
                                            response.WriteU32(mark);
                                            response.WriteU32(
                                                static_cast<std::uint32_t>(
                                                    status));
                                            response.WriteU32(consumed);
                                            replies.insert(
                                                replies.begin(),
                                                {.opcode = 990U,
                                                 .body = response.Release()});
                                            auto loaded =
                                                state->menu_snapshots.Load(
                                                    player);
                                            if (const auto* error = std::get_if<
                                                    application::ServiceError>(
                                                    &loaded)) {
                                              return *error;
                                            }
                                            auto snapshot = std::get<
                                                application::
                                                    PlayerMenuSnapshot>(
                                                std::move(loaded));
                                            replies.push_back(
                                                {.opcode = 802U,
                                                 .body = protocol::
                                                     EncodeInventorySnapshotResponse(
                                                         snapshot)});
                                            return runtime::
                                                EquipmentWorldResult{
                                                    .replies =
                                                        std::move(replies),
                                                    .changed = std::nullopt,
                                                    .clan_members =
                                                        std::move(members),
                                                    .clan_mark = mark};
                                          };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once, false,
                                                          state->handle)) {
                                            complete_once(*error);
                                          }
                                          return;
                                        }
                                        if ((packet.opcode == 804U ||
                                             packet.opcode == 805U ||
                                             packet.opcode == 832U) &&
                                            state->flow.PlayerId()
                                                .has_value()) {
                                          const auto player =
                                              *state->flow.PlayerId();
                                          if (packet.opcode == 832U) {
                                            static_cast<void>(
                                                protocol::
                                                    DecodeWeaponPresetRequest(
                                                        packet.body));
                                          } else {
                                            static_cast<void>(
                                                protocol::
                                                    DecodeEquipmentChangeRequest(
                                                        packet.body));
                                          }
                                          auto work = [state, player,
                                                       packet](bool allowed,
                                                               std::uint32_t
                                                                   combat_mode) -> application::
                                                                                    ServiceResult<
                                                                                        runtime::
                                                                                            EquipmentWorldResult> {
                                                                                      std::vector<
                                                                                          runtime::
                                                                                              OutboundMessage>
                                                                                          replies;
                                                                                      bool changed =
                                                                                          false;
                                                                                      if (allowed) {
                                                                                        auto handled =
                                                                                            state
                                                                                                ->flow
                                                                                                .Handle(
                                                                                                    packet,
                                                                                                    combat_mode);
                                                                                        if (const auto* error =
                                                                                                std::get_if<
                                                                                                    application::
                                                                                                        ServiceError>(
                                                                                                    &handled)) {
                                                                                          return *error;
                                                                                        }
                                                                                        replies = std::get<
                                                                                            std::vector<
                                                                                                runtime::
                                                                                                    OutboundMessage>>(
                                                                                            std::move(
                                                                                                handled));
                                                                                        if (!replies
                                                                                                 .empty() &&
                                                                                            !replies
                                                                                                 .front()
                                                                                                 .body
                                                                                                 .empty()) {
                                                                                          changed =
                                                                                              packet.opcode ==
                                                                                                      832U
                                                                                                  ? replies.front()
                                                                                                            .body
                                                                                                            .at(1U) ==
                                                                                                        0U
                                                                                                  : replies.front()
                                                                                                            .body
                                                                                                            .front() ==
                                                                                                        0U;
                                                                                        }
                                                                                      }
                                                                                      auto loaded =
                                                                                          state
                                                                                              ->menu_snapshots
                                                                                              .Load(
                                                                                                  player);
                                                                                      if (const auto* error =
                                                                                              std::get_if<
                                                                                                  application::
                                                                                                      ServiceError>(
                                                                                                  &loaded)) {
                                                                                        return *error;
                                                                                      }
                                                                                      auto snapshot = std::get<
                                                                                          application::
                                                                                              PlayerMenuSnapshot>(
                                                                                          std::move(
                                                                                              loaded));
                                                                                      if (!allowed) {
                                                                                        if (packet
                                                                                                .opcode ==
                                                                                            832U) {
                                                                                          const auto request =
                                                                                              protocol::DecodeWeaponPresetRequest(
                                                                                                  packet
                                                                                                      .body);
                                                                                          replies
                                                                                              .push_back(
                                                                                                  {.opcode =
                                                                                                       834U,
                                                                                                   .body = protocol::EncodeWeaponPresetResult(
                                                                                                       request
                                                                                                           .preset,
                                                                                                       false)});
                                                                                        } else {
                                                                                          const auto request =
                                                                                              protocol::DecodeEquipmentChangeRequest(
                                                                                                  packet
                                                                                                      .body);
                                                                                          replies
                                                                                              .push_back(
                                                                                                  packet.opcode ==
                                                                                                          804U
                                                                                                      ? runtime::
                                                                                                            OutboundMessage{.opcode =
                                                                                                                                809U,
                                                                                                                            .body = protocol::EncodeEquipmentAppliedResponse(
                                                                                                                                {.result =
                                                                                                                                     1U,
                                                                                                                                 .item_id =
                                                                                                                                     request
                                                                                                                                         .item_id})}
                                                                                                      : runtime::OutboundMessage{
                                                                                                            .opcode =
                                                                                                                810U,
                                                                                                            .body = protocol::EncodeEquipmentRemovedResponse(
                                                                                                                {.result =
                                                                                                                     1U})});
                                                                                        }
                                                                                        replies
                                                                                            .push_back(
                                                                                                {.opcode =
                                                                                                     806U,
                                                                                                 .body = protocol::
                                                                                                     EncodeEquippedItemsResponse(
                                                                                                         snapshot)});
                                                                                        replies
                                                                                            .push_back(
                                                                                                {.opcode =
                                                                                                     835U,
                                                                                                 .body = protocol::
                                                                                                     EncodeWeaponPresetsResponse(
                                                                                                         snapshot)});
                                                                                        replies
                                                                                            .push_back(
                                                                                                {.opcode =
                                                                                                     802U,
                                                                                                 .body = protocol::
                                                                                                     EncodeInventorySnapshotResponse(
                                                                                                         snapshot)});
                                                                                        // 被拒绝的操作也恢复原版左侧槽位，避免沿用客户端的旧库存标志。
                                                                                        replies
                                                                                            .push_back(
                                                                                                {.opcode =
                                                                                                     806U,
                                                                                                 .body = protocol::
                                                                                                     EncodeEquippedItemsResponse(
                                                                                                         snapshot)});
                                                                                      }
                                                                                      return runtime::EquipmentWorldResult{
                                                                                          .replies =
                                                                                              std::move(
                                                                                                  replies),
                                                                                          .changed =
                                                                                              changed
                                                                                                  ? std::optional{std::move(
                                                                                                        snapshot)}
                                                                                                  : std::
                                                                                                        nullopt,
                                                                                          .refresh_menu_slot =
                                                                                              true};
                                                                                    };
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostEquipmentChange(
                                                          player,
                                                          std::move(work),
                                                          complete_once,
                                                          packet.opcode == 832U,
                                                          state->handle)) {
                                            complete_once(*error);
                                          }
                                          return;
                                        }
                                        std::optional<std::vector<
                                            application::KeyBinding>>
                                            initial_keys;
                                        if (packet.opcode == 102U &&
                                            state->key_maps &&
                                            state->flow.PlayerId()) {
                                          auto loaded = state->key_maps->Load(
                                              *state->flow.PlayerId());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          initial_keys = std::get<std::vector<
                                              application::KeyBinding>>(
                                              std::move(loaded));
                                        }
                                        auto gateway_result =
                                            state->flow.Handle(packet);
                                        if (initial_keys) {
                                          if (auto* replies =
                                                  std::get_if<std::vector<
                                                      runtime::
                                                          OutboundMessage>>(
                                                      &gateway_result)) {
                                            replies->push_back(
                                                {.opcode = 981U,
                                                 .body = protocol::
                                                     EncodeKeyMapResponse(
                                                         true, *initial_keys)});
                                          }
                                        }
                                        if (const auto* error = std::get_if<
                                                application::ServiceError>(
                                                &gateway_result);
                                            error != nullptr &&
                                            error->code ==
                                                application::ErrorCode::
                                                    kUnsupportedOpcode) {
                                          // 仅对未实现路由容错；已知包的解析、身份和阶段错误不走此分支。
                                          // 每个连接最多记录 32
                                          // 种编号，防止高频未知包刷屏或无界积累。
                                          if (state->logged_unsupported_opcodes
                                                      .size() < 32U &&
                                              state->logged_unsupported_opcodes
                                                  .insert(packet.opcode)
                                                  .second) {
                                            logger->Log(
                                                application::LogLevel::kWarning,
                                                "未实现 opcode，已忽略并保持连接: opcode=" +
                                                    std::to_string(
                                                        packet.opcode) +
                                                    " body_bytes=" +
                                                    std::to_string(
                                                        packet.body.size()) +
                                                    " player_id=" +
                                                    std::to_string(
                                                        state->flow.PlayerId()
                                                                .has_value()
                                                            ? state->flow
                                                                  .PlayerId()
                                                                  ->Value()
                                                            : 0U));
                                          }
                                          complete_once(
                                              std::vector<
                                                  runtime::OutboundMessage>{});
                                          return;
                                        }
                                        if (!std::holds_alternative<
                                                application::ServiceError>(
                                                gateway_result) &&
                                            state->flow.PlayerId()
                                                .has_value()) {
                                          // Clear a previous session's device
                                          // for legacy launchers too, after
                                          // claiming the online identity,
                                          // before loading moderation state.
                                          if (!state->device_activated) {
                                            if (const auto error =
                                                    state->world_access
                                                        .ActivateDevice(
                                                            *state->flow
                                                                 .PlayerId(),
                                                            state->flow
                                                                .MachineCode())) {
                                              complete_once(*error);
                                              return;
                                            }
                                            state->device_activated = true;
                                          }
                                          if (state->life_attendance &&
                                              !state->attendance_started) {
                                            bool already_closed = false;
                                            std::optional<
                                                application::ServiceError>
                                                observation_error;
                                            std::chrono::system_clock::
                                                time_point login_observed_at;
                                            {
                                              std::lock_guard attendance_lock{
                                                  state->attendance_mutex};
                                              already_closed =
                                                  state->closing.load(
                                                      std::
                                                          memory_order_acquire);
                                              if (!already_closed) {
                                                login_observed_at =
                                                    state->clock.Now();
                                                observation_error =
                                                    state->life_attendance
                                                        ->Connect(
                                                            state->handle,
                                                            *state->flow
                                                                 .PlayerId(),
                                                            login_observed_at);
                                                state->attendance_started =
                                                    !observation_error;
                                              }
                                            }
                                            if (already_closed) {
                                              complete_once(MakeError(
                                                  application::ErrorCode::
                                                      kStorageUnavailable,
                                                  "登录会话已关闭"));
                                              return;
                                            }
                                            if (observation_error) {
                                              complete_once(*observation_error);
                                              return;
                                            }
                                            if (const auto attendance_error =
                                                    state->life_attendance
                                                        ->SettleLogin(
                                                            *state->flow
                                                                 .PlayerId(),
                                                            login_observed_at)) {
                                              // No successful login or
                                              // inventory is published while
                                              // absence settlement is unknown.
                                              complete_once(*attendance_error);
                                              return;
                                            }
                                          }
                                          state->authenticated_player.store(
                                              state->flow.PlayerId()->Value(),
                                              std::memory_order_release);
                                          if (const auto error =
                                                  state->session_registry
                                                      .SetSocialCapabilities(
                                                          state->handle, true,
                                                          state->social_flow !=
                                                                  nullptr &&
                                                              state->social_flow
                                                                  ->NativeSocialEnabled())) {
                                            complete_once(*error);
                                            return;
                                          }
                                        }
                                        if (packet.opcode == 102U &&
                                            state->flow.Phase() ==
                                                runtime::GatewaySessionPhase::
                                                    kMenuReady &&
                                            !std::holds_alternative<
                                                application::ServiceError>(
                                                gateway_result)) {
                                          auto loaded =
                                              state->menu_snapshots.Load(
                                                  *state->flow.PlayerId());
                                          if (const auto* error = std::get_if<
                                                  application::ServiceError>(
                                                  &loaded)) {
                                            complete_once(*error);
                                            return;
                                          }
                                          state->session_registry.SetWorldState(
                                              state->handle,
                                              state->world_state);
                                          if (const auto error =
                                                  state->world_executor
                                                      .PostPlayerOnline(
                                                          std::get<
                                                              application::
                                                                  PlayerMenuSnapshot>(
                                                              std::move(
                                                                  loaded)),
                                                          state->handle)) {
                                            complete_once(*error);
                                            return;
                                          }
                                        }
                                        complete_once(
                                            std::move(gateway_result));
                                      } catch (const protocol::ProtocolError&
                                                   error) {
                                        // Opcode, size and error class identify
                                        // the request; never log the payload.
                                        logger->Log(
                                            application::LogLevel::kWarning,
                                            "Gateway handler rejected malformed packet: opcode=" +
                                                std::to_string(packet.opcode) +
                                                " body_bytes=" +
                                                std::to_string(
                                                    packet.body.size()) +
                                                " reason=" +
                                                std::string{
                                                    protocol::
                                                        ProtocolErrorCodeName(
                                                            error.Code())} +
                                                " phase=" +
                                                std::to_string(static_cast<int>(
                                                    state->flow.Phase())) +
                                                " player_id=" +
                                                std::to_string(
                                                    state->flow.PlayerId()
                                                        ? state->flow
                                                              .PlayerId()
                                                              ->Value()
                                                        : 0U));
                                        complete_once(
                                            MakeError(application::ErrorCode::
                                                          kInvalidInput,
                                                      "协议消息字段无效"));
                                      } catch (const std::exception& error) {
                                        // Log only request metadata and the
                                        // exception type, never what(), packet
                                        // contents, one-time upload tokens or
                                        // database credentials.
                                        logger->Log(
                                            application::LogLevel::kError,
                                            "Gateway handler business task failed by exception: opcode=" +
                                                std::to_string(packet.opcode) +
                                                " body_bytes=" +
                                                std::to_string(
                                                    packet.body.size()) +
                                                " phase=" +
                                                std::to_string(static_cast<int>(
                                                    state->flow.Phase())) +
                                                " player_id=" +
                                                std::to_string(
                                                    state->flow.PlayerId()
                                                        ? state->flow
                                                              .PlayerId()
                                                              ->Value()
                                                        : 0U) +
                                                " exception_type=" +
                                                typeid(error).name());
                                        complete_once(MakeError(
                                            application::ErrorCode::kInternal,
                                            "处理网关请求时发生未分类错误"));
                                      } catch (...) {
                                        logger->Log(
                                            application::LogLevel::kCritical,
                                            "Gateway handler business task failed by unknown exception");
                                        complete_once(MakeError(
                                            application::ErrorCode::kInternal,
                                            "处理网关请求时发生未知错误"));
                                      }
                                    });
    if (post_error.has_value()) {
      complete_once(*post_error);
    }
  }

  std::optional<application::ServiceError> ApplyDispatch(
      const runtime::SessionDispatch& dispatch) override {
    if (state_->closing.load(std::memory_order_acquire)) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "Session 正在关闭");
    }
    if (dispatch.room_stage.has_value() || dispatch.plaza_stage.has_value()) {
      return MakeError(application::ErrorCode::kInvalidState,
                       "组合世界 handler 不接受旧房间或广场阶段字段");
    }
    std::scoped_lock lock{state_->mutex};
    // An explicit world-stage installation includes same-plaza/channel reentry
    // (fresh 112). Its old fishing context cannot authorize a later charge in
    // the replacement scene. Ordinary roster/config/inventory updates carry no
    // stage and keep the current cast.
    if (dispatch.world_phase.has_value() ||
        (dispatch.channel &&
         *dispatch.channel != state_->world_state.channel)) {
      state_->lobby_fishing.active_terms.reset();
    }
    if (dispatch.world_phase.has_value()) {
      state_->world_state.phase = *dispatch.world_phase;
    }
    if (dispatch.channel.has_value()) {
      state_->world_state.channel = *dispatch.channel;
    }
    return std::nullopt;
  }

  void BeginClose() noexcept override {
    // Capture before waiting for connect's local fsync, which may cross
    // midnight.
    const auto closed_at = state_->clock.Now();
    if (state_->closing.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (state_->life_attendance) {
      std::lock_guard attendance_lock{state_->attendance_mutex};
      try {
        if (state_->life_attendance->Disconnect(state_->handle, closed_at))
          logger_.Log(application::LogLevel::kCritical,
                      "Life close observation not durable");
      } catch (...) {
        logger_.Log(application::LogLevel::kError,
                    "Life close observation failed");
      }
    }
    auto state = state_;
    const auto error = database_tasks_.Post([state] {
      std::scoped_lock lock{state->mutex};
      const auto player_id = state->flow.PlayerId();
      if (player_id.has_value()) {
        const auto disconnect_error = state->world_executor.PostDisconnect(
            *player_id, state->world_state.phase, state->handle);
        static_cast<void>(disconnect_error);
      }
      if (player_id && state->couples) {
        auto cancelled = state->couples->Disconnect(*player_id, state->handle);
        for (auto& delivery : cancelled.messages.deliveries) {
          const auto target =
              cancelled.expected_sessions.find(delivery.recipient);
          if (target != cancelled.expected_sessions.end())
            static_cast<void>(state->session_registry.DispatchIfCurrent(
                delivery.recipient, target->second,
                std::vector<runtime::OutboundMessage>{
                    std::move(delivery.message)}));
        }
      }
      state->flow.BeginClose();
      state->flow.CompleteClose();
      if (player_id) {
        auto update = runtime::SocialPresenceChanged(
            *player_id, state->social_snapshots, state->session_registry);
        if (auto* value = std::get_if<runtime::SocialCommandResult>(&update))
          static_cast<void>(
              state->session_registry.Dispatch(std::move(*value)));
      }
    });
    if (error.has_value()) {
      // 任务池已停或过载时 SharedState 的最终析构仍会释放在线租约；TCP
      // 必须先于任务池停止。
      logger_.Log(application::LogLevel::kWarning,
                  "Gateway close cleanup could not enter database task pool");
    }
  }

 private:
  /** @brief Retain exactly one unexecuted world request during its due status
   * check. */
  struct PendingAccessCheck final {
    std::shared_ptr<SharedState> state;
    protocol::Packet packet;
    PacketCompletion completion;
    application::BackgroundTaskExecutor* database_tasks{nullptr};
    application::EventLogger* logger{nullptr};
    SessionWorkScheduler schedule_after;
    std::chrono::steady_clock::time_point created_at{
        std::chrono::steady_clock::now()};
    std::uint32_t retries{0U};
  };

  /** @brief Keep the unexecuted world request ordered while a due read-only
   * check is busy. */
  static void RetryAccessCheck(
      const std::shared_ptr<PendingAccessCheck>& check) {
    if (check->state->closing.load(std::memory_order_acquire)) {
      check->completion(
          MakeError(application::ErrorCode::kInvalidState, "Session 正在关闭"));
      return;
    }
    if (std::chrono::steady_clock::now() - check->created_at >=
        std::chrono::seconds{2}) {
      check->completion(MakeError(application::ErrorCode::kStorageUnavailable,
                                  "账号状态检查等待超时", true));
      return;
    }
    const auto delay = std::chrono::milliseconds{
        std::min<std::uint32_t>(50U, 5U << std::min(check->retries, 4U))};
    ++check->retries;
    if (const auto error = check->schedule_after(
            delay, [check] { AttemptAccessCheck(check); })) {
      check->completion(MakeError(application::ErrorCode::kStorageUnavailable,
                                  "无法等待账号状态检查", true));
    }
  }

  static void AttemptAccessCheck(std::shared_ptr<PendingAccessCheck> check) {
    if (check->state->closing.load(std::memory_order_acquire)) {
      check->completion(
          MakeError(application::ErrorCode::kInvalidState, "Session 正在关闭"));
      return;
    }
    if (std::chrono::steady_clock::now() - check->created_at >=
        std::chrono::seconds{2}) {
      check->completion(MakeError(application::ErrorCode::kStorageUnavailable,
                                  "账号状态检查等待超时", true));
      return;
    }
    const auto queued_at = std::chrono::steady_clock::now();
    const auto admission = check->database_tasks->Post([check, queued_at] {
      const auto started_at = std::chrono::steady_clock::now();
      auto state = check->state;
      try {
        std::optional<domain::PlayerId> player;
        {
          std::scoped_lock lock{state->mutex};
          if (state->closing.load(std::memory_order_acquire)) {
            check->completion(MakeError(application::ErrorCode::kInvalidState,
                                        "Session 正在关闭"));
            return;
          }
          player = state->flow.PlayerId();
          if (!player) {
            check->completion(MakeError(application::ErrorCode::kInternal,
                                        "已认证 Session 缺少世界玩家标识"));
            return;
          }
        }
        // Never hold the Session mutex while SQL or the connection pool can
        // wait.
        const auto check_started_at = std::chrono::steady_clock::now();
        auto checked = state->world_access.Load(*player);
        const auto completed_at = std::chrono::steady_clock::now();
        {
          std::scoped_lock lock{state->mutex};
          if ((check->packet.opcode == 32769U ||
               check->packet.opcode == 32885U) &&
              completed_at - check->created_at >=
                  std::chrono::milliseconds{25} &&
              completed_at - state->last_movement_moderation_log >=
                  std::chrono::seconds{5}) {
            state->last_movement_moderation_log = completed_at;
            const auto milliseconds = [](auto duration) {
              return std::chrono::duration_cast<std::chrono::milliseconds>(
                         duration)
                  .count();
            };
            check->logger->Log(
                application::LogLevel::kWarning,
                "native movement moderation latency: opcode=" +
                    std::to_string(check->packet.opcode) + " player_id=" +
                    std::to_string(player->Value()) + " admission_wait_ms=" +
                    std::to_string(
                        milliseconds(queued_at - check->created_at)) +
                    " database_queue_ms=" +
                    std::to_string(milliseconds(started_at - queued_at)) +
                    " session_lock_ms=" +
                    std::to_string(
                        milliseconds(check_started_at - started_at)) +
                    " check_ms=" +
                    std::to_string(
                        milliseconds(completed_at - check_started_at)) +
                    " admission_retries=" + std::to_string(check->retries));
          }
        }
        if (completed_at - check->created_at >= std::chrono::seconds{2}) {
          check->completion(
              MakeError(application::ErrorCode::kStorageUnavailable,
                        "账号状态检查等待超时", true));
          return;
        }
        if (const auto* error =
                std::get_if<application::ServiceError>(&checked)) {
          if (error->is_retryable &&
              (error->code == application::ErrorCode::kOverloaded ||
               error->code == application::ErrorCode::kStorageUnavailable)) {
            RetryAccessCheck(check);
          } else {
            check->completion(*error);
          }
          return;
        }
        if (std::get<application::WorldAccessSnapshot>(checked).banned) {
          check->logger->Log(
              application::LogLevel::kInfo,
              "world access check denied: opcode=" +
                  std::to_string(check->packet.opcode) +
                  " player_id=" + std::to_string(player->Value()));
          check->completion(MakeError(application::ErrorCode::kUnauthenticated,
                                      "账号或当前机器已被封禁"));
          return;
        }
        {
          std::scoped_lock lock{state->mutex};
          if (state->closing.load(std::memory_order_acquire) ||
              state->flow.PlayerId() != player) {
            check->completion(MakeError(
                application::ErrorCode::kUnauthenticated, "账号状态无效"));
            return;
          }
          state->next_moderation_check = completed_at + std::chrono::seconds{1};
        }
        if (!PostWorldDirect(state, check->packet, check->completion,
                             *check->logger)) {
          RetryAccessCheck(check);
        }
      } catch (const std::exception&) {
        check->completion(MakeError(application::ErrorCode::kInternal,
                                    "账号状态检查执行失败"));
      } catch (...) {
        check->completion(MakeError(application::ErrorCode::kInternal,
                                    "账号状态检查执行失败"));
      }
    });
    if (admission) {
      if (admission->code == application::ErrorCode::kOverloaded &&
          admission->is_retryable) {
        RetryAccessCheck(check);
      } else {
        check->completion(*admission);
      }
    }
  }

  /** @brief Dispatch only with a fresh status check; false requests an ordered
   * database check. */
  template <typename Completion>
  static bool PostWorldDirect(const std::shared_ptr<SharedState>& state,
                              protocol::Packet& packet,
                              Completion& complete_once,
                              application::EventLogger& logger) {
    try {
      if (state->closing.load(std::memory_order_acquire)) {
        complete_once(MakeError(application::ErrorCode::kInvalidState,
                                "Session 正在关闭"));
        return true;
      }
      std::optional<runtime::WorldActorContext> actor;
      {
        std::scoped_lock lock{state->mutex};
        if (state->closing.load(std::memory_order_acquire)) {
          complete_once(MakeError(application::ErrorCode::kInvalidState,
                                  "Session 正在关闭"));
          return true;
        }
        const auto player_id = state->flow.PlayerId();
        if (player_id &&
            std::chrono::steady_clock::now() >= state->next_moderation_check) {
          return false;
        }
        if (state->flow.Phase() != runtime::GatewaySessionPhase::kMenuReady) {
          complete_once(MakeError(application::ErrorCode::kInvalidState,
                                  "世界命令只能在菜单初始化后发送"));
          return true;
        }
        if (!player_id.has_value()) {
          complete_once(MakeError(application::ErrorCode::kInternal,
                                  "已认证 Session 缺少世界玩家标识"));
          return true;
        }
        actor = runtime::WorldActorContext{
            .player_id = *player_id,
            .state = state->world_state,
            .orig_session =
                std::pair{state->handle.id, state->handle.generation},
            .peer_address = state->peer_address};
      }
      const bool release_frame =
          TutorialPersistenceFrame(packet.opcode, actor->state.phase);
      if (release_frame) ValidateTutorialPersistenceFrame(packet);
      auto world_completion =
          release_frame ? TutorialLateCompletion(state, actor->player_id)
                        : runtime::WorldCommandCompletion{complete_once};
      if (const auto error =
              state->world_executor.Post(std::move(*actor), std::move(packet),
                                         std::move(world_completion))) {
        complete_once(*error);
      } else if (release_frame) {
        complete_once(std::vector<runtime::OutboundMessage>{});
      }
    } catch (const std::exception&) {
      logger.Log(application::LogLevel::kError,
                 "Gateway world command dispatch failed by exception");
      complete_once(MakeError(application::ErrorCode::kInternal,
                              "处理网关请求时发生未分类错误"));
    } catch (...) {
      logger.Log(application::LogLevel::kCritical,
                 "Gateway world command dispatch failed by unknown exception");
      complete_once(MakeError(application::ErrorCode::kInternal,
                              "处理网关请求时发生未知错误"));
    }
    return true;
  }

  application::BackgroundTaskExecutor& database_tasks_;
  application::EventLogger& logger_;
  SessionWorkScheduler schedule_after_;
  std::shared_ptr<SharedState> state_;
};

}  // namespace

GatewaySessionHandlerFactory::GatewaySessionHandlerFactory(
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
    application::DailyLoginRewardGranter* daily_rewards,
    bool allow_launcher_authentication, application::KeyMapStore* key_maps,
    application::GiftStore* gifts, application::LevelIconStore* level_icons,
    runtime::CoupleFlow* couples, runtime::SocialSpacesFlow* social_spaces,
    application::ChatBlockStore* chat_blocks,
    runtime::LobbyMinigameService* lobby_minigames,
    application::NativeTitleStore* titles,
    runtime::LifeAttendanceTracker* life_attendance) noexcept
    : database_tasks_(database_tasks),
      ticket_consumer_(ticket_consumer),
      daily_rewards_(daily_rewards),
      account_registration_(account_registration),
      password_login_(password_login),
      character_creation_(character_creation),
      menu_snapshots_(menu_snapshots),
      shop_catalog_(shop_catalog),
      purchases_(purchases),
      equipment_(equipment),
      social_snapshots_(social_snapshots),
      social_relationships_(social_relationships),
      private_messages_(private_messages),
      service_rules_(service_rules),
      treasure_(treasure),
      mystery_shop_(mystery_shop),
      workshop_(workshop),
      clans_(clans),
      item_use_(item_use),
      world_access_(world_access),
      world_executor_(world_executor),
      purchase_operation_ids_(purchase_operation_ids),
      session_registry_(session_registry),
      clan_tickets_(clan_tickets),
      clock_(clock),
      logger_(logger),
      gateway_endpoint_(gateway_endpoint),
      launcher_rate_limiter_(
          std::make_shared<LauncherAuthenticationRateLimiter>(
              allow_launcher_authentication)),
      gifts_(gifts),
      level_icons_(level_icons),
      couples_(couples),
      social_spaces_(social_spaces),
      chat_blocks_(chat_blocks),
      lobby_minigames_(lobby_minigames),
      titles_(titles),
      key_maps_(key_maps),
      life_attendance_(life_attendance) {}

application::ServiceResult<std::unique_ptr<SessionPacketHandler>>
GatewaySessionHandlerFactory::Create(SessionHandlerContext context) {
  if (!context.handle.IsValid() || !context.online_players ||
      context.peer_address.empty() || gateway_endpoint_.port == 0U ||
      std::ranges::all_of(gateway_endpoint_.ipv4_address,
                          [](std::uint8_t octet) { return octet == 0U; })) {
    return MakeError(application::ErrorCode::kInternal,
                     "创建 Gateway Session handler 的上下文无效");
  }
  try {
    return std::unique_ptr<SessionPacketHandler>{
        std::make_unique<GatewaySessionHandler>(
            std::move(context), database_tasks_, ticket_consumer_,
            account_registration_, password_login_, character_creation_,
            menu_snapshots_, shop_catalog_, purchases_, equipment_,
            social_snapshots_, social_relationships_, private_messages_,
            service_rules_, treasure_, mystery_shop_, workshop_, clans_,
            item_use_, world_access_, world_executor_, purchase_operation_ids_,
            session_registry_, clan_tickets_, clock_, logger_,
            gateway_endpoint_, launcher_rate_limiter_, daily_rewards_,
            key_maps_, gifts_, level_icons_, couples_, social_spaces_,
            chat_blocks_, lobby_minigames_, titles_, life_attendance_)};
  } catch (const std::exception&) {
    return MakeError(application::ErrorCode::kOverloaded,
                     "创建 Gateway Session handler 失败", true);
  }
}

}  // namespace bf::infrastructure::network
