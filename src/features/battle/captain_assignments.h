#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CAPTAIN_ASSIGNMENTS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CAPTAIN_ASSIGNMENTS_H_

/** @file captain_assignments.h @brief
 * 队长选任、临时第4栏和恢复收据；不决定比赛分数。 */
#include <random>

#include "features/battle/combat_relay.h"

namespace bf::runtime {
/**
 * @brief
 * 模式20原生选任与Fever生命周期；现有房间胜负为明确标注的本服team-pang策略。
 * @note
 * 调用方在认证生命周期变化后tick。提前到达的2021只保存收据，不能申请或撤销队长。
 */
class CaptainAssignments final {
 public:
  using Time = std::chrono::steady_clock::time_point;

  /** @brief
   * 已关联攻击者710/27与受害者确认死亡的单次事实；不含未恢复的计分倍率。 */
  struct DefeatNotice final {
    domain::PlayerId victim;
    domain::PlayerId attacker;
    std::uint64_t assignment;
    std::uint64_t life;
    std::uint32_t death_count;
    Time confirmed_at;
    std::uint8_t weapon_kind{
        0U};  ///< 报告时攻击者当前武器类别，不能作为致死武器或伤害证据。
  };

  /** @brief 全场选任广播与排除本机预测者的属性清理转发。 */
  struct Update final {
    std::vector<OutboundMessage> broadcasts;
    std::vector<CombatDelivery> deliveries;
    std::vector<DefeatNotice>
        defeats;  ///< 每个已确认队长死亡最多消费一次，不再次累计普通击杀。
  };

  /** @brief 已认证计量事件的接入口；不是网络请求，不从客户端传入任意增量。 */
  struct FeverIncrement final {
    std::uint64_t
        assignment;        ///< 本次选任代次，旧角色的异步事件不能给新角色充能。
    std::uint64_t serial;  ///< 调用方已去重且递增的事件编号，0无效。
    std::uint32_t
        amount;  ///< 91DB30舍入后的单次增量；0也检查就绪，伤害来源仍待可信接入。
    std::uint64_t life{
        0U};  ///< 事件产生时的生命代次，泡封前迟到事件不能给获救角色充能。
  };

  /** @brief 供计量适配层捕获选任代次及检查状态，不代表已有实机计量来源。 */
  struct FeverProgress final {
    std::uint64_t assignment;
    std::uint64_t charge;  ///< 保留第二次加量；宽整数避免原生32位溢出。
    bool ready;
    bool pending;
    bool active;
    std::uint64_t life;
  };

  /** @brief 固定种子供可重复回放；正式接入应由对局服务注入随机种子。 */
  explicit CaptainAssignments(std::uint32_t seed);
  /** @brief 两队各随机选一名活人；确认死亡/离场后接任，不将泡封当作死亡。 */
  [[nodiscard]] Update Tick(CombatRelay& combat, Time now);
  /** @brief 消费变身、恢复、清理与710/27收据；710须等待同次选任的确认死亡。 */
  [[nodiscard]] CombatRelayResult Handle(domain::PlayerId player,
                                         const protocol::Packet& packet,
                                         CombatRelay& combat, Time now);
  /** @brief 过滤转换前、恢复后的迟到切枪；身份和结构仍交给战斗账本验证。 */
  [[nodiscard]] bool PrepareAction(domain::PlayerId player,
                                   const protocol::Packet& packet,
                                   const CombatRelay& combat) const;
  /** @brief 终局恢复临时栏位且撤销未消费授权；后续tick不会重开。 */
  void Stop(CombatRelay& combat);
  /** @brief 返回当前队伍已指定者，等待两秒时也占用唯一队长名额。 */
  [[nodiscard]] std::optional<domain::PlayerId> Selected(
      std::uint8_t team) const noexcept;
  /** @brief
   * 仅接收服务端认证整数计量；RoomCommandFlow已接时间来源，不接自报伤害/HP。 */
  [[nodiscard]] bool CreditFever(domain::PlayerId player, FeverIncrement event,
                                 const CombatRelay& combat, Time now);
  [[nodiscard]] std::optional<FeverProgress> GetFeverProgress(
      domain::PlayerId player) const;

 private:
  /** @brief 就绪、707消费、32894激活及属性11结束的单次授权。 */
  struct Fever final {
    std::uint64_t charge{0U};
    std::uint64_t serial{0U};
    bool ready{false};
    std::optional<Time> requested;
    std::uint64_t request_life{0U};
    std::optional<Time> until;
    bool ended{false};
    bool cleanup_reported{false};
  };

  /** @brief 一次已下发选任及其临时装备、退任状态。 */
  struct Role final {
    std::uint8_t team;
    std::uint32_t nominated_deaths;  ///< 泡封/获救不撤销队长，确认死亡才撤销。
    Time ready;
    std::optional<Time> expires;
    std::optional<Time> applied;
    std::optional<std::uint32_t> previous_weapon;
    std::optional<std::uint32_t> temporary_weapon;
    bool retired{false};
    bool restore_reported{false};  ///< 跨连接死亡确认可能晚于本机恢复收据。
    bool attribute_reported{
        false};  ///< 32812可能早于或晚于2021，不能直接撤销活队长。
    bool attribute_forwarded{false};  ///< 只在认证退任后向同行执行一次清理。
    std::uint64_t assignment{0U};
    std::uint64_t fever_life{0U};
    Fever fever;
  };

  /** @brief 提前报告绑定预计死亡生命；泡封恢复后的报告不能借用下一次死亡。 */
  struct DefeatReceipt final {
    std::uint64_t life;
    Time received;
    std::uint8_t weapon_kind;
  };

  /** @brief
   * 按受害者保留有限关联窗口；恢复清理删除角色后仍可接收真实攻击者报告。 */
  struct DefeatClaim final {
    std::uint64_t assignment;
    std::map<domain::PlayerId, DefeatReceipt> receipts;
    std::optional<DefeatNotice> confirmed;
    std::optional<std::uint64_t> bubble_life;
    bool emitted{false};
  };

  void SyncDefeats(const CombatRelay& combat, Time now);
  [[nodiscard]] std::vector<DefeatNotice> CollectDefeats(
      const CombatRelay& combat, Time now);
  [[nodiscard]] CombatRelayResult HandleDefeat(domain::PlayerId player,
                                               const protocol::Packet& packet,
                                               const CombatRelay& combat,
                                               Time now);
  void Retire(domain::PlayerId player, Role& role, CombatRelay& combat);
  [[nodiscard]] std::vector<CombatDelivery> ForwardCleanup(
      domain::PlayerId player, Role& role, CombatRelay& combat);
  [[nodiscard]] CombatRelayResult HandleFever(domain::PlayerId player,
                                              const protocol::Packet& packet,
                                              CombatRelay& combat, Time now);
  [[nodiscard]] std::vector<CombatDelivery> UpdateFever(domain::PlayerId player,
                                                        Role& role,
                                                        CombatRelay& combat,
                                                        Time now);
  [[nodiscard]] static std::uint32_t BoostedWeapon(std::uint32_t base) noexcept;
  /** @brief 827F50事件51清零计量；保留属性11及单调事件号。 */
  static void SyncFeverLife(Role& role, std::uint64_t life) noexcept;
  std::mt19937 random_;
  std::map<domain::PlayerId, Role> roles_;
  std::map<domain::PlayerId, DefeatClaim> defeat_claims_;
  std::map<domain::PlayerId, std::set<std::uint32_t>> retired_weapons_;
  std::array<std::optional<domain::PlayerId>, 2U> selected_{};
  bool initialized_{false};
  bool stopped_{false};
  std::uint64_t next_assignment_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_CAPTAIN_ASSIGNMENTS_H_
