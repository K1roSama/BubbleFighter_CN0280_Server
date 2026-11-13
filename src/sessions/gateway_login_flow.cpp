#include "sessions/gateway_login_flow.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "protocol/clans/clan_messages.h"
#include "protocol/codec/text_codec.h"
#include "protocol/inventory/inventory_messages.h"
#include "protocol/lobby/menu_messages.h"
#include "protocol/shop/shop_catalog_messages.h"
#include "protocol/social/couple_messages.h"

namespace bf::runtime {
namespace {

constexpr std::u16string_view kTicketPrefix = u"launcher-v2:";

application::ServiceError InvalidState(std::string_view message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidState,
      .safe_message = std::string{message}};
}

application::ServiceError InvalidInput(std::string_view message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = std::string{message}};
}

application::ServiceError ContractError(std::string_view message) {
  return application::ServiceError{.code = application::ErrorCode::kInternal,
                                   .safe_message = std::string{message}};
}

std::uint32_t LegacyPlayerId(domain::PlayerId player_id) {
  if (!player_id.IsValid() ||
      player_id.Value() > std::numeric_limits<std::uint32_t>::max()) {
    throw protocol::ProtocolError{
        protocol::ProtocolErrorCode::kLengthLimit,
        "player ID exceeds verified legacy protocol range"};
  }
  return static_cast<std::uint32_t>(player_id.Value());
}

std::vector<OutboundMessage> LoginSuccessMessages(
    const application::AuthenticatedPlayerSnapshot& player,
    std::chrono::system_clock::time_point now) {
  if (!player.is_character_created || !player.initial_fighter.has_value()) {
    return {
        {.opcode = 14U, .body = protocol::EncodeCharacterCreationRequired()}};
  }
  const auto unix_seconds =
      std::chrono::duration<double>{now.time_since_epoch()}.count();
  protocol::LoginSuccessResponse response{
      .account_name = protocol::Utf8ToUtf16(player.account_name),
      .player_id = LegacyPlayerId(player.player_id),
      .nickname = protocol::Utf8ToUtf16(player.nickname),
      .initial_fighter_id = static_cast<std::uint32_t>(*player.initial_fighter),
      .server_date_ole = protocol::UnixSecondsToOleDate(unix_seconds)};
  return {
      {.opcode = 3U, .body = protocol::EncodeLoginSuccessResponse(response)}};
}

bool IsClientVisibleCharacterError(application::ErrorCode code) noexcept {
  return code == application::ErrorCode::kInvalidInput ||
         code == application::ErrorCode::kConflict;
}

bool IsClientVisibleAssetError(application::ErrorCode code) noexcept {
  return code == application::ErrorCode::kInvalidInput ||
         code == application::ErrorCode::kPermissionDenied ||
         code == application::ErrorCode::kInvalidState ||
         code == application::ErrorCode::kInsufficientBalance ||
         code == application::ErrorCode::kConflict;
}

std::vector<OutboundMessage> CharacterFailureMessages(
    std::u16string nickname, const application::ServiceError& error) {
  auto message = error.safe_message.empty() ? std::string{"角色资料请求未完成"}
                                            : error.safe_message;
  return {
      {.opcode = 15U,
       .body = protocol::EncodeNicknameCheckResponse(
           {.result = 1U, .nickname = std::move(nickname)})},
      {.opcode = 109U,
       .body = protocol::EncodeNoticeResponse(protocol::Utf8ToUtf16(message))}};
}

}  // namespace

GatewayLoginFlow::GatewayLoginFlow(
    application::LoginTicketConsumer& ticket_consumer,
    application::CharacterCreationUseCases& character_creation,
    application::PlayerMenuSnapshotLoader& menu_snapshots,
    application::ShopCatalogLoader& shop_catalog,
    application::PurchaseExecutor& purchases,
    application::EquipmentChanger& equipment,
    PurchaseOperationIdSource& purchase_operation_ids,
    const application::SystemClock& clock, OnlinePlayerRegistry& online_players,
    protocol::GatewayEndpoint gateway_endpoint,
    application::DailyLoginRewardGranter* daily_rewards) noexcept
    : ticket_consumer_(ticket_consumer),
      daily_rewards_(daily_rewards),
      character_creation_(character_creation),
      menu_snapshots_(menu_snapshots),
      shop_catalog_(shop_catalog),
      purchases_(purchases),
      equipment_(equipment),
      purchase_operation_ids_(purchase_operation_ids),
      clock_(clock),
      online_players_(online_players),
      gateway_endpoint_(gateway_endpoint) {}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::Handle(const protocol::Packet& packet,
                         std::uint32_t combat_mode) {
  switch (packet.opcode) {
    case 1U:
      return HandlePatch(packet.body);
    case 3U:
      return HandleLogin(packet.body);
    case 18U:
      return HandleExit(packet.body);
    case 21U:
      return HandleEnvironment(packet.body);
    case 22U:
      return HandleCharacterCreationCenter(packet.opcode, packet.body);
    case 31U:
      return HandleNicknameCheck(packet.body);
    case 32U:
      return HandleCharacterCreation(packet.body);
    case 33U:
      return HandleCharacterCreationCenter(packet.opcode, packet.body);
    case 102U:
      return HandleMenuBootstrap(packet.body);
    case 107U:
      return HandleTutorialStart(packet.body);
    case 521U:
    case 522U:
      return HandleClientStatus(packet.opcode, packet.body);
    case 801U:
    case 802U:
      return HandleShopCatalog(packet.opcode, packet.body);
    case 803U:
      return HandleInventoryRefresh(packet.body);
    case 804U:
    case 805U:
      return HandleEquipmentChange(packet.opcode, packet.body);
    case 809U:
      return HandlePurchase(packet.body);
    case 827U:
      return HandleCashRefresh(packet.body);
    case 832U:
      return HandleWeaponPreset(packet.body, combat_mode);
    case 922U:
      return HandleMenuAuxiliaryReady(packet.body);
    case 959U:
    case 2022U:
      return HandleLegacyMenuReport(packet.opcode, packet.body);
    default:
      return application::ServiceError{
          .code = application::ErrorCode::kUnsupportedOpcode,
          .safe_message = "尚未支持 opcode " + std::to_string(packet.opcode)};
  }
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandlePatch(std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodePatchHandshakeRequest(body);
  if (request.build != protocol::kSupportedClientBuild) {
    return InvalidInput("客户端构建号不是当前已验证版本");
  }
  if (gateway_endpoint_.port == 0U) {
    return ContractError("网关广告端点缺少有效端口");
  }

  std::vector<OutboundMessage> replies{
      {.opcode = 2U,
       .body = protocol::EncodePatchHandshakeResponse(
           {.needs_patch = false, .patch_url = {}})},
      {.opcode = 17U, .body = protocol::EncodeProcessProbeResponse({})},
      {.opcode = 19U,
       .body = protocol::EncodeGatewayListResponse({&gateway_endpoint_, 1U})}};
  if (auto error = state_.AcceptPatchHandshake()) {
    return *error;
  }
  return replies;
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleEnvironment(std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kPatchReady) {
    return InvalidState("环境报告只能在补丁握手后发送");
  }
  static_cast<void>(protocol::DecodeEnvironmentReport(body));
  return std::vector<OutboundMessage>{};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleLogin(std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kPatchReady) {
    return InvalidState("票据登录只能在 patch_ready 阶段执行");
  }
  const auto request = protocol::DecodeLoginRequest(body);
  const std::u16string* ticket = nullptr;
  for (const auto& text : request.texts) {
    if (text.starts_with(kTicketPrefix)) {
      if (ticket != nullptr) {
        return InvalidInput("登录请求包含多个启动器票据");
      }
      ticket = &text;
    }
  }
  if (ticket == nullptr) {
    return application::ServiceError{
        .code = application::ErrorCode::kUnauthenticated,
        .safe_message = "请通过启动器登录账号"};
  }

  const application::ConsumeLoginTicketCommand command{
      .ticket = protocol::Utf16ToUtf8(*ticket)};
  auto login_result = ticket_consumer_.Consume(command);
  if (const auto* error =
          std::get_if<application::ServiceError>(&login_result)) {
    return *error;
  }
  auto player = std::get<application::AuthenticatedPlayerSnapshot>(
      std::move(login_result));

  auto lease_result = online_players_.Claim(player.player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&lease_result)) {
    return *error;
  }
  auto lease =
      std::get<std::unique_ptr<OnlinePlayerLease>>(std::move(lease_result));
  if (!lease) {
    return ContractError("在线玩家 registry 返回空租约");
  }

  if (daily_rewards_ != nullptr) {
    // Failed awards roll back and are logged server-side; login remains
    // available.
    const auto awarded =
        daily_rewards_->GrantWithResult(player.player_id, clock_.Now());
    if (const auto* value =
            std::get_if<application::DailyLoginRewardResult>(&awarded);
        value && value->clan_id != 0U && value->clan_points > 0U) {
      pending_login_contribution_ = *value;
    }
  }
  auto replies = LoginSuccessMessages(player, clock_.Now());
  const auto disposition = player.is_character_created
                               ? LoginDisposition::kAuthenticated
                               : LoginDisposition::kCharacterCreationRequired;
  if (auto error = state_.CompleteLogin(disposition)) {
    return *error;
  }
  authenticated_player_ = std::move(player);
  online_lease_ = std::move(lease);
  return replies;
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleNicknameCheck(std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeNicknameCheckRequest(body);
  if (state_.Phase() == GatewaySessionPhase::kAuthenticated) {
    if (!authenticated_player_.has_value()) {
      return ContractError("已认证网关阶段缺少玩家快照");
    }
    // 原客户端在角色创建成功并收到 S2C3 后，可能仍投递已经排队的
    // C2S31。角色事实已经 提交，此时仅校验包结构并幂等忽略，避免迟到的 UI
    // 检查关闭刚进入大厅的连接。
    return std::vector<OutboundMessage>{};
  }
  if ((state_.Phase() != GatewaySessionPhase::kCreatingCharacter &&
       state_.Phase() != GatewaySessionPhase::kMenuReady) ||
      !authenticated_player_.has_value()) {
    return InvalidState("昵称检查需要进入角色创建或菜单阶段");
  }
  const auto nickname = protocol::Utf16ToUtf8(request.nickname);
  auto result = character_creation_.CheckNickname(
      {.player_id = authenticated_player_->player_id, .nickname = nickname});
  if (const auto* error = std::get_if<application::ServiceError>(&result)) {
    if (IsClientVisibleCharacterError(error->code)) {
      return CharacterFailureMessages(request.nickname, *error);
    }
    return *error;
  }
  return std::vector<OutboundMessage>{
      {.opcode = 15U,
       .body = protocol::EncodeNicknameCheckResponse(
           {.result = 0U, .nickname = request.nickname})}};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleCharacterCreationCenter(
    std::uint16_t opcode, std::span<const std::uint8_t> body) {
  if (opcode == 22U) {
    protocol::DecodeCharacterCreationCenterRequest(body);
  } else {
    protocol::DecodeCharacterCreationCenterAck(body);
  }
  if (state_.Phase() != GatewaySessionPhase::kCreatingCharacter ||
      !authenticated_player_.has_value()) {
    return InvalidState("创建角色中心确认只能在 creating_character 阶段执行");
  }
  if (opcode == 22U) {
    return std::vector<OutboundMessage>{
        {.opcode = 16U,
         .body =
             protocol::EncodeCharacterCreationCenterResponse({.result = 0U})}};
  }
  return std::vector<OutboundMessage>{};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleClientStatus(std::uint16_t opcode,
                                     std::span<const std::uint8_t> body) {
  if (opcode == 521U) {
    protocol::DecodeClientStatusReadyRequest(body);
  } else {
    protocol::DecodeClientStatusUpdate(body);
  }
  switch (state_.Phase()) {
    case GatewaySessionPhase::kCreatingCharacter:
    case GatewaySessionPhase::kAuthenticated:
    case GatewaySessionPhase::kMenuReady:
      if (!authenticated_player_.has_value()) {
        return ContractError("登录后网关阶段缺少玩家快照");
      }
      return std::vector<OutboundMessage>{};
    case GatewaySessionPhase::kConnected:
    case GatewaySessionPhase::kPatchReady:
    case GatewaySessionPhase::kClosing:
    case GatewaySessionPhase::kClosed:
      return InvalidState("客户端状态报告只能在登录后阶段执行");
  }
  return ContractError("未知网关 Session 阶段");
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleTutorialStart(std::span<const std::uint8_t> body) {
  static_cast<void>(protocol::DecodeTutorialStartReport(body));
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("新手教程开始上报只能在 menu_ready 阶段发送");
  }
  // Python
  // 兼容服和全部现有原版抓包只证明该包可被静默消费。教程关卡、结算和奖励尚无
  // 服务端权威语义，不能因为收到客户端上报就写入完成状态或发放资产。
  return std::vector<OutboundMessage>{};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleMenuAuxiliaryReady(std::span<const std::uint8_t> body) {
  protocol::DecodeMenuAuxiliaryReadyReport(body);
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("菜单辅助模块就绪信号只能在 menu_ready 阶段发送");
  }
  // 历史原版抓包与 Python 兼容服只证明：C2S922
  // 是菜单初始化后的空包，且无需回包。
  // 未恢复其具体子系统语义前，不据此修改玩家、社交或公告状态。
  return std::vector<OutboundMessage>{};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleLegacyMenuReport(std::uint16_t opcode,
                                         std::span<const std::uint8_t> body) {
  if (opcode == 959U) {
    static_cast<void>(protocol::DecodeLegacyMenuStateReport(body));
  } else {
    static_cast<void>(protocol::DecodeLegacyMenuTextReport(body));
  }
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("菜单辅助上报只能在 menu_ready 阶段发送");
  }
  // Python
  // 兼容服对两种消息均只做抓包记录并继续会话。现有原版样本只覆盖大厅和广场，
  // 因此这里不回包、不写库，也不将客户端提供的未知字段提升为服务端权威事实。
  return std::vector<OutboundMessage>{};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleCharacterCreation(std::span<const std::uint8_t> body) {
  const auto request = protocol::DecodeCreateCharacterRequest(body);
  if (state_.Phase() == GatewaySessionPhase::kAuthenticated ||
      state_.Phase() == GatewaySessionPhase::kMenuReady) {
    if (!authenticated_player_.has_value()) {
      return ContractError("已认证网关阶段缺少玩家快照");
    }
    // 与 C2S31 相同，建角成功后迟到的重复 C2S32
    // 不得再次写入数据库，也不能断开连接。
    return std::vector<OutboundMessage>{};
  }
  if (state_.Phase() != GatewaySessionPhase::kCreatingCharacter ||
      !authenticated_player_.has_value()) {
    return InvalidState("角色创建只能在 creating_character 阶段执行");
  }
  const auto nickname = protocol::Utf16ToUtf8(request.nickname);
  auto result = character_creation_.CreateCharacter(
      {.player_id = authenticated_player_->player_id,
       .nickname = nickname,
       .fighter_id = request.fighter_id});
  if (const auto* error = std::get_if<application::ServiceError>(&result)) {
    if (IsClientVisibleCharacterError(error->code)) {
      return CharacterFailureMessages(request.nickname, *error);
    }
    return *error;
  }
  const auto created =
      std::get<application::CharacterCreationResult>(std::move(result));
  if (created.player_id != authenticated_player_->player_id ||
      created.nickname != nickname) {
    return ContractError("角色创建结果与当前会话不一致");
  }

  auto updated = *authenticated_player_;
  updated.nickname = created.nickname;
  updated.is_character_created = true;
  updated.initial_fighter = created.fighter;
  auto replies = LoginSuccessMessages(updated, clock_.Now());
  if (auto error = state_.CompleteCharacterCreation()) {
    return *error;
  }
  authenticated_player_ = std::move(updated);
  return replies;
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleMenuBootstrap(std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kAuthenticated ||
      !authenticated_player_.has_value()) {
    return InvalidState("菜单初始化只能在 authenticated 阶段执行一次");
  }
  static_cast<void>(protocol::DecodePostLoginBootstrapRequest(body));
  auto snapshot_result = menu_snapshots_.Load(authenticated_player_->player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&snapshot_result)) {
    return *error;
  }
  const auto snapshot =
      std::get<application::PlayerMenuSnapshot>(std::move(snapshot_result));
  if (snapshot.player_id != authenticated_player_->player_id) {
    return ContractError("菜单快照与当前在线玩家不一致");
  }

  std::vector<OutboundMessage> replies{
      {.opcode = 105U, .body = protocol::EncodePlayerProfileResponse(snapshot)},
      {.opcode = 201U, .body = protocol::EncodeSoloPartyResponse(snapshot)},
      {.opcode = 112U, .body = protocol::EncodeMenuStateResponse(snapshot)},
      {.opcode = 806U, .body = protocol::EncodeEquippedItemsResponse(snapshot)},
      {.opcode = 835U, .body = protocol::EncodeWeaponPresetsResponse(snapshot)},
      {.opcode = 802U,
       .body = protocol::EncodeInventorySnapshotResponse(snapshot)}};
  replies.push_back(
      {.opcode = 944U, .body = protocol::EncodeCouplePartner(snapshot.couple)});
  if (!snapshot.single_play_clears.empty())
    replies.push_back({.opcode = 924U,
                       .body = protocol::EncodeSinglePlayClearsResponse(
                           snapshot.single_play_clears)});
  if (pending_login_contribution_) {
    replies.push_back(
        {.opcode = 25U,
         .body = protocol::EncodeClanPointsNotice(
             pending_login_contribution_->clan_id,
             pending_login_contribution_->clan_points, 1U, false)});
  }
  if (auto error = state_.AcceptPostLoginBootstrap()) {
    return *error;
  }
  pending_login_contribution_.reset();
  return replies;
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleInventoryRefresh(std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("仓库刷新只能在菜单初始化完成后执行");
  }
  static_cast<void>(protocol::DecodeInventorySnapshotRequest(body));
  auto snapshot_result = menu_snapshots_.Load(authenticated_player_->player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&snapshot_result)) {
    return *error;
  }
  const auto snapshot =
      std::get<application::PlayerMenuSnapshot>(std::move(snapshot_result));
  if (snapshot.player_id != authenticated_player_->player_id) {
    return ContractError("仓库快照与当前在线玩家不一致");
  }
  return std::vector<OutboundMessage>{
      {.opcode = 802U,
       .body = protocol::EncodeInventorySnapshotResponse(snapshot)},
      {.opcode = 835U, .body = protocol::EncodeWeaponPresetsResponse(snapshot)},
      {.opcode = 806U,
       .body = protocol::EncodeEquippedItemsResponse(snapshot)}};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleShopCatalog(std::uint16_t opcode,
                                    std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("商城目录只能在菜单初始化完成后读取");
  }
  std::uint32_t selection = 300U;
  if (opcode == 801U) {
    selection = protocol::DecodeShopCatalogRequest(body).selection;
  } else {
    static_cast<void>(protocol::DecodeCashShopCatalogRequest(body));
  }
  auto catalog_result = shop_catalog_.Load();
  if (const auto* error =
          std::get_if<application::ServiceError>(&catalog_result)) {
    return *error;
  }
  const auto catalog =
      std::get<application::PublishedShopCatalog>(std::move(catalog_result));
  if (catalog.revision == 0U) {
    return ContractError("商城目录加载器返回零发布版本");
  }
  if (opcode == 801U) {
    return std::vector<OutboundMessage>{
        {.opcode = 800U,
         .body = protocol::EncodeShopCatalogResponse(catalog, selection)}};
  }
  return std::vector<OutboundMessage>{
      {.opcode = 801U,
       .body = protocol::EncodeCashShopCatalogResponse(catalog)}};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleEquipmentChange(std::uint16_t opcode,
                                        std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("换装只能在菜单初始化完成后执行");
  }
  const auto request = protocol::DecodeEquipmentChangeRequest(body);
  if (request.item_id == 0U) {
    return InvalidState("换装物品编号不能为零");
  }
  const auto action = opcode == 804U ? application::EquipmentAction::kEquip
                                     : application::EquipmentAction::kUnequip;
  auto change_result =
      equipment_.Change({.player_id = authenticated_player_->player_id,
                         .item_id = domain::ItemId{request.item_id},
                         .action = action,
                         .loadout_id = 0U});
  bool succeeded = true;
  if (const auto* error =
          std::get_if<application::ServiceError>(&change_result)) {
    if (!IsClientVisibleAssetError(error->code)) {
      return *error;
    }
    succeeded = false;
  } else {
    const auto& changed =
        std::get<application::ChangeEquipmentResult>(change_result);
    if (changed.player_id != authenticated_player_->player_id ||
        changed.item_id != domain::ItemId{request.item_id} ||
        changed.action != action) {
      return ContractError("换装结果与当前请求不一致");
    }
  }

  auto snapshot_result = menu_snapshots_.Load(authenticated_player_->player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&snapshot_result)) {
    return *error;
  }
  const auto snapshot =
      std::get<application::PlayerMenuSnapshot>(std::move(snapshot_result));
  if (snapshot.player_id != authenticated_player_->player_id) {
    return ContractError("换装后的菜单快照与当前在线玩家不一致");
  }

  std::vector<OutboundMessage> replies;
  replies.reserve(5U);
  if (action == application::EquipmentAction::kEquip) {
    replies.push_back(
        {.opcode = 809U,
         .body = protocol::EncodeEquipmentAppliedResponse(
             {.result = succeeded ? 0U : 1U, .item_id = request.item_id})});
  } else {
    replies.push_back({.opcode = 810U,
                       .body = protocol::EncodeEquipmentRemovedResponse(
                           {.result = succeeded ? 0U : 1U})});
  }
  replies.push_back({.opcode = 806U,
                     .body = protocol::EncodeEquippedItemsResponse(snapshot)});
  replies.push_back({.opcode = 835U,
                     .body = protocol::EncodeWeaponPresetsResponse(snapshot)});
  // 原版 802 刷新头像时读取预览角色；先用 806
  // 更新模型，避免顶部保留上一个角色。
  replies.push_back(
      {.opcode = 802U,
       .body = protocol::EncodeInventorySnapshotResponse(snapshot)});
  // B6B8F0→CDC740→D11730 从库存 equipped 标志重建左侧槽位；必须在 802
  // 后再刷新。 前面的 806 仍用于先更新模型，供 802 刷新头像使用。
  replies.push_back({.opcode = 806U,
                     .body = protocol::EncodeEquippedItemsResponse(snapshot)});
  return replies;
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleWeaponPreset(std::span<const std::uint8_t> body,
                                     std::uint32_t combat_mode) {
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("武器预设只能在菜单初始化后切换");
  }
  const auto request = protocol::DecodeWeaponPresetRequest(body);
  const auto error = equipment_.SelectPreset(authenticated_player_->player_id,
                                             request.preset, combat_mode);
  if (error && !IsClientVisibleAssetError(error->code)) {
    return *error;
  }
  auto loaded = menu_snapshots_.Load(authenticated_player_->player_id);
  if (const auto* failure = std::get_if<application::ServiceError>(&loaded)) {
    return *failure;
  }
  const auto snapshot =
      std::get<application::PlayerMenuSnapshot>(std::move(loaded));
  if (snapshot.player_id != authenticated_player_->player_id) {
    return InvalidState("武器预设快照身份不匹配");
  }
  return std::vector<OutboundMessage>{
      {.opcode = 834U,
       .body = protocol::EncodeWeaponPresetResult(request.preset, !error)},
      {.opcode = 806U, .body = protocol::EncodeEquippedItemsResponse(snapshot)},
      {.opcode = 835U, .body = protocol::EncodeWeaponPresetsResponse(snapshot)},
      {.opcode = 802U,
       .body = protocol::EncodeInventorySnapshotResponse(snapshot)},
      // 当前页库存标志到达后，重建主副武器、近战和特殊武器图标。
      {.opcode = 806U,
       .body = protocol::EncodeEquippedItemsResponse(snapshot)}};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandlePurchase(std::span<const std::uint8_t> body) {
  if (state_.Phase() != GatewaySessionPhase::kMenuReady ||
      !authenticated_player_.has_value()) {
    return InvalidState("购买只能在菜单初始化完成后执行");
  }
  const auto request = protocol::DecodePurchaseOfferRequest(body);
  if (request.stock_id == 0U) {
    return InvalidState("销售规格编号不能为零");
  }
  if (request.gift_flag != 0U) {
    return std::vector<OutboundMessage>{
        {.opcode = 814U,
         .body = protocol::EncodePurchaseOfferResponse(
             {.stock_id = request.stock_id, .result = 0U})}};
  }

  auto operation_result = purchase_operation_ids_.Next();
  if (const auto* error =
          std::get_if<application::ServiceError>(&operation_result)) {
    return *error;
  }
  const auto operation_id =
      std::get<domain::OperationId>(std::move(operation_result));
  auto purchase_result =
      purchases_.Purchase({.operation_id = operation_id,
                           .player_id = authenticated_player_->player_id,
                           .stock_id = domain::StockId{request.stock_id}});
  if (const auto* error =
          std::get_if<application::ServiceError>(&purchase_result)) {
    if (!IsClientVisibleAssetError(error->code)) {
      return *error;
    }
    return std::vector<OutboundMessage>{
        {.opcode = 814U,
         .body = protocol::EncodePurchaseOfferResponse(
             {.stock_id = request.stock_id, .result = 0U})}};
  }
  const auto& purchased =
      std::get<application::PurchaseResult>(purchase_result);
  if (purchased.player_id != authenticated_player_->player_id ||
      purchased.stock_id != domain::StockId{request.stock_id}) {
    return ContractError("购买结果与当前请求不一致");
  }

  const OutboundMessage committed_reply{
      .opcode = 814U,
      .body = protocol::EncodePurchaseOfferResponse(
          {.stock_id = request.stock_id, .result = 1U})};
  const auto refresh_unavailable = [&committed_reply]() {
    return std::vector<OutboundMessage>{
        committed_reply,
        {.opcode = 109U,
         .body = protocol::EncodeNoticeResponse(
             u"购买已提交，仓库刷新暂不可用；请稍后刷新，不要重复付款。")}};
  };
  // Purchase execution and result identity checks remain outside this boundary.
  // A completed receipt must survive a later menu read/encoder failure; the
  // original809 has no client UUID.
  try {
    auto snapshot_result =
        menu_snapshots_.Load(authenticated_player_->player_id);
    if (std::holds_alternative<application::ServiceError>(snapshot_result)) {
      return refresh_unavailable();
    }
    const auto& snapshot =
        std::get<application::PlayerMenuSnapshot>(snapshot_result);
    if (snapshot.player_id != authenticated_player_->player_id) {
      return refresh_unavailable();
    }
    // Build every refresh before returning any packet, preserving the original
    // successful order.
    std::vector<OutboundMessage> replies{
        {.opcode = 105U,
         .body = protocol::EncodePlayerProfileResponse(snapshot)}};
    if (purchased.item_id == domain::ItemId{500001U}) {
      replies.push_back(
          {.opcode = 806U,
           .body = protocol::EncodeEquippedItemsResponse(snapshot)});
      replies.push_back(
          {.opcode = 835U,
           .body = protocol::EncodeWeaponPresetsResponse(snapshot)});
    }
    replies.push_back(
        {.opcode = 802U,
         .body = protocol::EncodeInventorySnapshotResponse(snapshot)});
    replies.push_back(committed_reply);
    return replies;
  } catch (const std::exception&) {
    return refresh_unavailable();
  }
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleCashRefresh(std::span<const std::uint8_t> body) {
  static_cast<void>(protocol::DecodeCashBalanceRequest(body));

  // 0.2.80.1 客户端会在票据登录前以及首次角色创建页主动探测
  // C2S827。旧服务端在这两个
  // 阶段校验空载荷后静默忽略；若把探测当作状态错误并关闭
  // socket，客户端会在登录页退出。 已认证阶段则可立即返回权威余额，不要求
  // C2S102 已完成。
  switch (state_.Phase()) {
    case GatewaySessionPhase::kPatchReady:
    case GatewaySessionPhase::kCreatingCharacter:
      return std::vector<OutboundMessage>{};
    case GatewaySessionPhase::kAuthenticated:
    case GatewaySessionPhase::kMenuReady:
      break;
    case GatewaySessionPhase::kConnected:
    case GatewaySessionPhase::kClosing:
    case GatewaySessionPhase::kClosed:
      return InvalidState("点券刷新不允许在当前网关阶段执行");
  }
  if (!authenticated_player_.has_value()) {
    return ContractError("已认证网关阶段缺少玩家快照");
  }

  auto snapshot_result = menu_snapshots_.Load(authenticated_player_->player_id);
  if (const auto* error =
          std::get_if<application::ServiceError>(&snapshot_result)) {
    return *error;
  }
  const auto snapshot =
      std::get<application::PlayerMenuSnapshot>(std::move(snapshot_result));
  if (snapshot.player_id != authenticated_player_->player_id) {
    return ContractError("钱包快照与当前在线玩家不一致");
  }
  return std::vector<OutboundMessage>{
      {.opcode = 832U, .body = protocol::EncodeCashBalanceResponse(snapshot)}};
}

application::ServiceResult<std::vector<OutboundMessage>>
GatewayLoginFlow::HandleExit(std::span<const std::uint8_t> body) {
  static_cast<void>(protocol::DecodeGatewayExitRequest(body));
  BeginClose();
  return std::vector<OutboundMessage>{};
}

void GatewayLoginFlow::BeginClose() noexcept {
  state_.BeginClose();
  online_lease_.reset();
  authenticated_player_.reset();
}

void GatewayLoginFlow::CompleteClose() noexcept {
  BeginClose();
  state_.CompleteClose();
}

GatewaySessionPhase GatewayLoginFlow::Phase() const noexcept {
  return state_.Phase();
}

std::optional<domain::PlayerId> GatewayLoginFlow::PlayerId() const noexcept {
  if (!authenticated_player_.has_value()) {
    return std::nullopt;
  }
  return authenticated_player_->player_id;
}

std::optional<domain::AccountId> GatewayLoginFlow::AccountId() const noexcept {
  return authenticated_player_
             ? std::optional{authenticated_player_->account_id}
             : std::nullopt;
}

std::optional<std::string> GatewayLoginFlow::AccountName() const {
  if (!authenticated_player_.has_value()) {
    return std::nullopt;
  }
  return authenticated_player_->account_name;
}

std::optional<std::string> GatewayLoginFlow::Nickname() const {
  if (!authenticated_player_.has_value() ||
      state_.Phase() == GatewaySessionPhase::kClosing ||
      state_.Phase() == GatewaySessionPhase::kClosed) {
    return std::nullopt;
  }
  return authenticated_player_->nickname;
}

void GatewayLoginFlow::UpdateNickname(std::string nickname) {
  if (authenticated_player_) {
    authenticated_player_->nickname = std::move(nickname);
  }
}

}  // namespace bf::runtime
