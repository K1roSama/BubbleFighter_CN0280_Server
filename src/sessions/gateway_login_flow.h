#ifndef BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_LOGIN_FLOW_H_
#define BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_LOGIN_FLOW_H_

/**
 * @file gateway_login_flow.h
 * @brief 定义网关握手、票据登录与首次角色创建的同步会话路由核心。
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "application/error.h"
#include "application/system_clock.h"
#include "features/authentication/authentication.h"
#include "features/characters/character_creation.h"
#include "features/inventory/equipment_service.h"
#include "features/lobby/player_menu_snapshot.h"
#include "features/shop/purchase_service.h"
#include "features/shop/shop_catalog.h"
#include "protocol/codec/frame_codec.h"
#include "protocol/login/gateway_messages.h"
#include "sessions/gateway_session_state.h"
#include "sessions/outbound_message.h"

namespace bf::runtime {

/**
 * @brief 在线玩家唯一租约。
 * @note 具体 registry 必须原子保证一个 player_id
 * 仅有一份活动租约；析构释放，不能由裸 Session 指针或弱引用代替。
 */
class OnlinePlayerLease {
 public:
  virtual ~OnlinePlayerLease() = default;
};

/** @brief 跨 Session 原子占用在线玩家的运行时端口。 */
class OnlinePlayerRegistry {
 public:
  virtual ~OnlinePlayerRegistry() = default;

  /** @brief 成功返回独占租约；重复在线返回冲突，过载返回可重试错误。 */
  [[nodiscard]] virtual application::ServiceResult<
      std::unique_ptr<OnlinePlayerLease>>
  Claim(domain::PlayerId player_id) = 0;
};

/** @brief 为无客户端幂等键的购买请求分配服务端内部操作标识。 */
class PurchaseOperationIdSource {
 public:
  virtual ~PurchaseOperationIdSource() = default;

  /** @brief 每个新 C2S809 返回一个非零且进程内不重复的随机 UUID。 */
  [[nodiscard]] virtual application::ServiceResult<domain::OperationId>
  Next() = 0;
};

/**
 * @brief 单个网关 Session 的登录、菜单初始化和基础资产路由核心。
 * @note 本对象不加锁，必须由未来 Session strand
 * 独占；方法内同步调用应用用例，正式网络层
 *       应把阻塞数据库/密码工作投递到有界工作池后再将结果回到同一 strand。
 * @note 当前处理
 * C2S1/3/18/21/22/31/32/33/102/107/521/522/801/802/803/804/805/809/827/922/959/2022；房间业务由后续路由器负责。
 */
class GatewayLoginFlow final {
 public:
  /** @brief 注入用例、时钟、在线 registry
   * 和当前监听端点；引用生命周期必须覆盖本对象。 */
  GatewayLoginFlow(
      application::LoginTicketConsumer& ticket_consumer,
      application::CharacterCreationUseCases& character_creation,
      application::PlayerMenuSnapshotLoader& menu_snapshots,
      application::ShopCatalogLoader& shop_catalog,
      application::PurchaseExecutor& purchases,
      application::EquipmentChanger& equipment,
      PurchaseOperationIdSource& purchase_operation_ids,
      const application::SystemClock& clock,
      OnlinePlayerRegistry& online_players,
      protocol::GatewayEndpoint gateway_endpoint,
      application::DailyLoginRewardGranter* daily_rewards = nullptr) noexcept;

  /**
   * @brief 解析并处理一条已解密 frame；未知编号返回
   * kUnsupportedOpcode，阶段错误独立处理。
   * @note 网络适配层对已认证玩家的未知编号记录日志并忽略，不结束连接。
   * @note ProtocolError
   * 表示不可信字节畸形，调用方应按协议违规关闭连接；业务错误不抛异常。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>> Handle(
      const protocol::Packet& packet, std::uint32_t combat_mode = 0U);

  /** @brief 开始幂等关闭并释放在线租约，使断线玩家可以重新登录。 */
  void BeginClose() noexcept;

  /** @brief socket/回调全部失效后完成关闭。 */
  void CompleteClose() noexcept;

  /** @brief 返回当前会话阶段；只能在所属 executor 上读取。 */
  [[nodiscard]] GatewaySessionPhase Phase() const noexcept;

  /** @brief 登录成功后返回玩家 ID；未认证或已关闭时为空。 */
  [[nodiscard]] std::optional<domain::PlayerId> PlayerId() const noexcept;
  [[nodiscard]] std::optional<domain::AccountId> AccountId() const noexcept;

  /** @brief 登录成功后复制数据库权威账号展示名；未认证或关闭时为空。 */
  [[nodiscard]] std::optional<std::string> AccountName() const;

  [[nodiscard]] std::string MachineCode() const {
    return authenticated_player_ ? authenticated_player_->machine_code
                                 : std::string{};
  }

  /** @brief 登录成功后复制数据库权威玩家昵称；用于绑定战队票据显示身份。 */
  [[nodiscard]] std::optional<std::string> Nickname() const;
  /** @brief Refresh the leased identity after a committed rename. */
  void UpdateNickname(std::string nickname);

 private:
  /** @brief 原生武器页切换，提交后刷新五条预设和当前装备。 */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleWeaponPreset(std::span<const std::uint8_t> body,
                     std::uint32_t combat_mode);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandlePatch(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleEnvironment(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleLogin(std::span<const std::uint8_t> body);
  /**
   * @brief 处理创建角色页面的 C2S22/S2C16/C2S33 中心确认链。
   * @note 两个请求均为空载荷且只允许在 creating_character 阶段；C2S22
   * 返回成功结果，C2S33 仅完成客户端阶段确认，不改变角色事务状态。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleCharacterCreationCenter(std::uint16_t opcode,
                                std::span<const std::uint8_t> body);
  /**
   * @brief 校验并消费登录后的 C2S521/C2S522 客户端状态空包。
   * @note 当前 C++ 服务不运行旧保护组件，也不伪造其状态；兼容行为与已验证
   * Python 服务一致， 只在已有登录身份的阶段静默消费合法空包。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleClientStatus(std::uint16_t opcode, std::span<const std::uint8_t> body);
  /**
   * @brief 校验并消费 menu_ready 阶段的 C2S107 新手教程/单人任务开始上报。
   * @note
   * 当前只保留旧服已验证的无响应兼容行为；不把收到上报解释为教程完成或发奖依据。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleTutorialStart(std::span<const std::uint8_t> body);
  /**
   * @brief 校验并消费 menu_ready 阶段的 C2S922 空载荷兼容信号。
   * @note
   * 该包在原版抓包中紧随菜单初始化出现；当前不推断未恢复的子系统状态，也不产生响应或写库。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleMenuAuxiliaryReady(std::span<const std::uint8_t> body);
  /**
   * @brief 校验并消费菜单初始化后的 C2S959/C2S2022 旧客户端辅助上报。
   * @note
   * 历史样本覆盖大厅与广场，均无旧服响应；这里只验证线格式，不产生写库或状态迁移。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleLegacyMenuReport(std::uint16_t opcode,
                         std::span<const std::uint8_t> body);
  /** @brief 处理建角阶段 C2S31；建角提交后的合法迟到检查经结构校验后幂等忽略。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleNicknameCheck(std::span<const std::uint8_t> body);
  /** @brief 处理 C2S32 原子建角；提交后的合法迟到副本不重复写库。 */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleCharacterCreation(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleMenuBootstrap(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleInventoryRefresh(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleShopCatalog(std::uint16_t opcode, std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleEquipmentChange(std::uint16_t opcode,
                        std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandlePurchase(std::span<const std::uint8_t> body);
  /**
   * @brief 处理 C2S827 点券余额刷新及旧客户端的登录前探测。
   * @note patch_ready/creating_character
   * 阶段仅校验空载荷并静默忽略；authenticated/menu_ready
   *       阶段从仓储加载权威快照并返回 S2C832。畸形载荷始终抛出 ProtocolError。
   */
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleCashRefresh(std::span<const std::uint8_t> body);
  [[nodiscard]] application::ServiceResult<std::vector<OutboundMessage>>
  HandleExit(std::span<const std::uint8_t> body);

  application::LoginTicketConsumer& ticket_consumer_;
  application::DailyLoginRewardGranter* daily_rewards_;
  application::CharacterCreationUseCases& character_creation_;
  application::PlayerMenuSnapshotLoader& menu_snapshots_;
  application::ShopCatalogLoader& shop_catalog_;
  application::PurchaseExecutor& purchases_;
  application::EquipmentChanger& equipment_;
  PurchaseOperationIdSource& purchase_operation_ids_;
  const application::SystemClock& clock_;
  OnlinePlayerRegistry& online_players_;
  protocol::GatewayEndpoint gateway_endpoint_;
  GatewaySessionState state_;
  std::optional<application::AuthenticatedPlayerSnapshot> authenticated_player_;
  std::unique_ptr<OnlinePlayerLease> online_lease_;
  std::optional<application::DailyLoginRewardResult>
      pending_login_contribution_;  ///< 菜单就绪后发送一次。
};

}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_SESSIONS_GATEWAY_LOGIN_FLOW_H_
