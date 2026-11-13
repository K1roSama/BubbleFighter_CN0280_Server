#ifndef BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_OBJECTIVES_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_OBJECTIVES_H_

/**
 * @file match_objectives.h
 * @brief 定义夺星、金币、生存和爆破的单局目标状态及定时决策。
 */

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "features/battle/bomb_maps.h"
#include "features/battle/combat_relay.h"
#include "features/battle/survival_loot.h"
#include "protocol/battle/match_transition_messages.h"

namespace bf::runtime {

/** @brief 开局冻结的模式和获胜目标，防止两个编号参数误换。 */
struct MatchObjectiveRules final {
  std::uint32_t mode{0U};    ///< 原生模式编号。
  std::uint32_t target{0U};  ///< 目标分或获胜回合数。
  std::string
      map_key;  ///< 已验证的地图资源键，生存掉落和爆破安装点使用对应原表。
  std::map<domain::PlayerId, std::u16string>
      nicknames;  ///< 开局可信昵称，死亡背包不信任请求文本。
};

/** @brief 目标状态机产生的拥有型效果与权威阶段决策。 */
struct MatchObjectiveUpdate final {
  std::vector<OutboundMessage> messages;  ///< 已验证的全房原生效果。
  bool score_changed{false};              ///< 是否需要重新编码实时成绩。
  bool finished{false};                   ///< 是否应提交整局可靠结算。
  std::optional<domain::PlayerId>
      exclude_recipient;      ///< 本机已执行的效果不再回显给发送者。
  bool restart_round{false};  ///< 是否应保留本局身份并重新加载下一回合。
  std::optional<std::string>
      rejection_detail;  ///< 仅写服务端日志的拾取拒绝原因。
  std::vector<CombatDelivery>
      deliveries;  ///< 扩展请求的私有确认，不广播背包数量。
};

/** @brief 单世界线程拥有的目标规则；仅使用认证战斗账本和开局冻结配置。 */
class MatchObjectives final {
 public:
  /** @brief 冻结目标模式、比分和成员，并读取房主已校验地图锚点。 */
  MatchObjectives(MatchObjectiveRules rules,
                  std::vector<CombatParticipant> participants,
                  const protocol::MapAssetsRequest& assets,
                  std::chrono::steady_clock::time_point now);
  /** @brief 房主迟到的 702 只补建缺失的僵尸补给；不重置已有物件和再生时钟。 */
  [[nodiscard]] bool RecoverInfectionSupplies(
      const protocol::MapAssetsRequest& assets);
  /** @brief 爆破新回合开始时重建固定补给，废弃上回合拾取和补弹凭据。 */
  void StartBombRound(std::chrono::steady_clock::time_point now);
  /** @brief 检查球编号属于房主的原生对象分配区间，排除角色本身。 */
  [[nodiscard]] static bool SoccerBallObject(std::uint32_t host_actor,
                                             std::uint32_t object) noexcept;
  /** @brief 登记足球房主单独报告的球对象；与玩家 Character 身份分离。 */
  [[nodiscard]] bool RegisterSoccerBall(domain::PlayerId host,
                                        std::uint32_t object,
                                        CombatRelay& combat);
  /** @brief 判断属于此目标模块的 C2S 编号。 */
  [[nodiscard]] static bool Accepts(std::uint16_t opcode) noexcept;
  /** @brief 无副作用检查目标请求格式，供阶段外迟到消息使用。 */
  static void Validate(const protocol::Packet& packet);
  /** @brief 处理本人拾取/掉落/爆破动作，非法状态无副作用忽略。 */
  [[nodiscard]] MatchObjectiveUpdate Handle(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, std::chrono::steady_clock::time_point now);
  /** @brief 推进目标计时、检查存活者及回合胜负，不信任客户端的预测胜方。 */
  [[nodiscard]] MatchObjectiveUpdate Tick(
      CombatRelay& combat, std::chrono::steady_clock::time_point now,
      std::chrono::steady_clock::time_point round_deadline);
  /** @brief 返回下一次目标计时截止点；无定时工作返回空。 */
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline()
      const;
  /** @brief 返回确认的个人目标分；不存在为零。 */
  [[nodiscard]] std::uint32_t Score(domain::PlayerId player) const noexcept;
  /** @brief 返回合法拾取并应随可靠终局事务入账的额外金币。 */
  [[nodiscard]] std::uint32_t BonusLucci(
      domain::PlayerId player) const noexcept;
  /** @brief 返回爆破队伍回合分，其他模式为零。 */
  [[nodiscard]] std::array<std::uint32_t, 2U> RoundScores() const noexcept;
  /** @brief 返回爆破终局胜方；尚未决定时为零。 */
  [[nodiscard]] std::uint8_t Winner() const noexcept;
  /** @brief 返回零基爆破回合索引。 */
  [[nodiscard]] std::uint32_t RoundIndex() const noexcept;
  /** @brief 判断爆破回合已结束且仍等待重载。 */
  [[nodiscard]] bool BetweenRounds() const noexcept;
  /** @brief 物品智能标记只能指向当前仍存在、未过期的生存掉落物。 */
  [[nodiscard]] bool SmartPingItem(
      std::uint32_t object,
      std::chrono::steady_clock::time_point now) const noexcept;
  /** @brief 判断爆破引信仍有效，允许其超过普通回合时限。 */
  [[nodiscard]] bool FuseActive() const noexcept;

 private:
  using Time = std::chrono::steady_clock::time_point;
  void LoadSupplies(const protocol::MapAssetsRequest& assets, Time now);
  Time supplies_started_at_{};

  /** @brief 海盗地图固定补给点；对象被拾取后按地图上下限重新生成。 */
  struct Supply final {
    std::array<float, 3U> position{};
    std::array<float, 3U> velocity{};
    std::uint32_t type{0U};
    std::uint32_t id{0U};
    std::uint8_t team{0U};
    std::uint32_t minimum_ms{15'000U};
    std::uint32_t maximum_ms{30'000U};
    Time due{};
    std::optional<Time>
        expires;  ///< Character-spawned boxes expire instead of regenerating.
  };

  struct ItemDropPermit final {
    std::uint64_t life{0U};
    Time expires{};
    std::uint32_t used{0U};
    bool spawned{false};
  };

  void ObserveItemDrops(const CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate DropItemBox(domain::PlayerId sender,
                                                 const protocol::Packet& packet,
                                                 const CombatRelay& combat,
                                                 Time now);
  std::map<domain::PlayerId, ItemDropPermit> item_drop_permits_;
  std::set<std::vector<std::uint8_t>> item_drop_requests_;

  /** @brief 已核准拾取的单次本机效果同步凭据；跨生命和超时自动作废。 */
  struct SupplyEffect final {
    domain::PlayerId player;
    std::uint32_t type{0U};
    std::uint64_t life{0U};
    Time expires{};
  };

  /** @brief 当前道具栏代次持有的道具及使用时间；泡封保留、死亡清空。 */
  struct HeldItem final {
    std::array<std::uint32_t, 2U> types{};
    std::uint64_t generation{0U};
    std::uint32_t selected{0U};
    Time usable_at{};
    bool clear_pending{
        false};  ///< 死亡代次失效后，在可接收道具的生命阶段同步两槽/UI。
  };

  void SynchronizeItemInventory(domain::PlayerId player,
                                const CombatRelay& combat,
                                MatchObjectiveUpdate& result);
  [[nodiscard]] OutboundMessage ItemSelection(domain::PlayerId player,
                                              const HeldItem& held,
                                              bool success) const;
  [[nodiscard]] MatchObjectiveUpdate ChangeItem(domain::PlayerId player,
                                                const CombatRelay& combat);
  [[nodiscard]] MatchObjectiveUpdate UseItem(domain::PlayerId player,
                                             CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate ApplySupplyEffect(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate CollectSupply(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);

  /** @brief 一处地图生成点或有界动态掉落及再生时间。 */
  struct Collectible final {
    std::array<float, 3U> position{};
    std::array<float, 3U> velocity{};
    std::uint32_t kind{1U};
    std::uint32_t variant{1U};
    std::uint32_t id{0U};
    std::uint32_t value{1U};
    std::uint16_t spawn_opcode{716U};
    std::uint16_t collect_opcode{732U};
    std::optional<std::chrono::milliseconds> delay;
    Time due{};
    std::optional<Time> expires;  ///< 动态恢复物/额外金币过期后不能拾取。
    Time active_at{};             ///< 原生掉落后的一秒不可拾取阶段。
    bool bonus_lucci{false};      ///< 额外资产金币与金币模式持有分数分开。
    bool pirate_coin{
        false};  ///< 原生海盗金币使用客户端碰撞报告；对象仍只能消费一次。
  };

  [[nodiscard]] MatchObjectiveUpdate PirateCoin(domain::PlayerId player,
                                                const protocol::Packet& packet,
                                                const CombatRelay& combat,
                                                Time now);
  std::set<std::pair<domain::PlayerId, std::uint64_t>> pirate_coin_claims_;
  [[nodiscard]] MatchObjectiveUpdate Loot(domain::PlayerId player,
                                          const protocol::Packet& packet,
                                          const CombatRelay& combat, Time now);

  /** @brief 原生 747/748 地面武器；当前只接受可溯源的道具火箭筒。 */
  struct DroppedWeapon final {
    std::uint32_t id{0U};
    std::uint32_t total_ammo{0U};
    std::uint32_t magazine_ammo{0U};
    std::array<float, 3U> position{};
    Time created{};
  };

  std::vector<DroppedWeapon> dropped_weapons_;

  /** @brief 金币报文保留认证控制者及重试期限，不能改变NPC的生命归属。 */
  struct PendingPirateCoin final {
    domain::PlayerId controller;
    protocol::Packet packet;
    Time received;
    Time retry;
  };

  std::vector<PendingPirateCoin> pending_pirate_coins_;
  void AdvancePirateCoins(const CombatRelay& combat, Time now,
                          MatchObjectiveUpdate& result);
  std::vector<SurvivalLoot> survival_loot_;

  struct PartialDropReply final {
    std::vector<std::uint8_t> request;
    OutboundMessage reply;
  };

  std::map<std::tuple<domain::PlayerId, std::uint64_t, std::uint32_t>,
           PartialDropReply>
      partial_drop_replies_;
  bool survival_loot_pending_{false};
  std::uint32_t survival_tide_phase_{
      0U};  ///< 756: 1 等待，2 警报，3 退水，4 涨水。
  std::optional<Time> survival_tide_due_;
  void AdvanceSurvivalTide(Time now, MatchObjectiveUpdate& result);

  /** @brief 每个已确认淘汰生命只允许一次背包提交，十秒为服务器传输期限。 */
  struct BackpackPermit final {
    std::uint64_t life{0U};
    Time expires{};
    bool submitted{false};
  };

  std::map<domain::PlayerId, BackpackPermit> backpack_permits_;
  std::map<domain::PlayerId, std::u16string> survival_nicknames_;
  std::vector<SurvivalBackpack> survival_backpacks_;
  void ObserveSurvivalDefeats(CombatRelay& combat, Time now,
                              MatchObjectiveUpdate& result);
  /** @brief 个人生存本人死亡动画完成后按生命代次广播一次
   * 980；与背包提交许可独立。 */
  void ObserveSurvivalSpectators(const CombatRelay& combat,
                                 MatchObjectiveUpdate& result);
  std::map<domain::PlayerId, std::uint64_t> survival_spectators_;
  [[nodiscard]] MatchObjectiveUpdate SurvivalBackpack(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate DropSurvivalLoot(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate CollectSurvivalLoot(
      domain::PlayerId player, const protocol::Packet& packet,
      CombatRelay& combat, Time now);
  [[nodiscard]] MatchObjectiveUpdate WeaponDrop(domain::PlayerId player,
                                                const protocol::Packet& packet,
                                                CombatRelay& combat, Time now);

  /** @brief 每次死亡/羞辱的服务端随机掉落上限与已接受数量。 */
  struct LootBudget final {
    std::uint32_t maximum{0U};
    std::uint32_t used{0U};
  };

  std::map<std::tuple<domain::PlayerId, std::uint64_t, std::uint16_t>,
           LootBudget>
      loot_budgets_;
  std::map<domain::PlayerId, std::uint32_t> bonus_lucci_;
  [[nodiscard]] MatchObjectiveUpdate Collect(domain::PlayerId player,
                                             const protocol::Packet& packet,
                                             const CombatRelay& combat,
                                             Time now);
  [[nodiscard]] MatchObjectiveUpdate Bomb(domain::PlayerId player,
                                          const protocol::Packet& packet,
                                          const CombatRelay& combat, Time now);
  [[nodiscard]] OutboundMessage BombPacket(domain::PlayerId player,
                                           std::uint32_t state) const;
  void CompleteBombRound(std::uint8_t team, Time now,
                         MatchObjectiveUpdate& result);
  [[nodiscard]] const CombatParticipant* Participant(
      domain::PlayerId player) const noexcept;
  [[nodiscard]] bool ActiveBombOperator(std::optional<domain::PlayerId> player,
                                        const CombatRelay& combat) const;
  [[nodiscard]] bool ScoreLimitReached() const;
  [[nodiscard]] MatchObjectiveUpdate Soccer(domain::PlayerId player,
                                            const protocol::Packet& packet,
                                            CombatRelay& combat, Time now);
  std::optional<std::uint32_t> soccer_ball_;
  std::optional<Time> soccer_due_;
  std::uint8_t soccer_phase_{0U};  ///< 0 比赛中，1 进球庆祝，2 重新开球倒计时。
  std::uint32_t mode_{0U};
  std::uint32_t target_{0U};
  std::vector<CombatParticipant> participants_;
  std::vector<Collectible> collectibles_;
  std::vector<Supply> supplies_;
  std::vector<SupplyEffect> supply_effects_;
  std::map<domain::PlayerId, HeldItem> held_items_;
  std::mt19937 supply_random_{std::random_device{}()};
  std::map<domain::PlayerId, std::uint32_t> scores_;
  std::set<std::vector<std::uint8_t>> drops_;
  /** @brief 同一次羞辱可丢出多个物品，但不能超出原版最大掉落数量。 */
  std::map<domain::PlayerId, std::pair<std::uint64_t, std::uint32_t>>
      humiliation_drops_;
  std::uint32_t serial_{60'000'000U};
  std::array<std::uint32_t, 2U> round_scores_{};
  std::uint32_t round_{0U};
  const BombMap* bomb_map_{nullptr};  ///< 开局冻结的原生发布地图及安装点。
  std::uint32_t bomb_state_{0U};
  std::optional<domain::PlayerId> planter_;  ///< Native 2015/979 PlayerID.
  std::optional<domain::PlayerId> defuser_;
  std::array<float, 3U> bomb_position_{};
  Time action_started_{};
  std::chrono::milliseconds plant_duration_{
      3000};  ///< 原版 GameOption 安装读条。
  std::chrono::milliseconds defuse_duration_{
      5000};  ///< 原版 GameOption 拆除读条。
  std::chrono::milliseconds fuse_duration_{
      30000};  ///< 原版按开局人数冻结的引信时间。
  std::optional<Time> fuse_;
  std::optional<Time>
      bomb_result_due_;  ///< 540110/53E720：爆炸或拆除后保留 2000 ms 原生效果。
  std::uint8_t pending_bomb_winner_{
      0U};  ///< 终态锁定后不再被迟到死亡或 2014 改写。
  std::set<domain::PlayerId>
      bomb_spectators_;  ///< 每回合死亡动画完成后的原生 980 只发送一次。
  std::optional<Time> next_round_;
  std::uint8_t winner_{0U};
};
}  // namespace bf::runtime

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_BATTLE_MATCH_OBJECTIVES_H_
