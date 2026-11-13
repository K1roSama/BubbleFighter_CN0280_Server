#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_LOBBY_MINIGAME_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_LOBBY_MINIGAME_MESSAGES_H_

/** @file lobby_minigame_messages.h
 *  @brief 广场小游戏使用独立 WORD 子编号，不能按主 opcode 推断请求与响应结构。
 *
 *  C2S/S2C571 carry requests and replies, C2S/S2C572 carry visual sync; both
 * start with a u16 sub-ID that 6628D0 registers with
 * SubPacketDispatchEntry1Impl handlers. The 42-byte configuration is S2C1500
 * dynamic-config section 3 (`xxLMG.dch`, decoder 661EF0) and the per-day record
 * is S2C134 `gspRepLmgUserRecord` (B854F0).
 */

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf::protocol {

inline constexpr std::uint16_t kLobbyMinigameRequestOpcode = 571U;
inline constexpr std::uint16_t kLobbyMinigameSyncOpcode = 572U;
inline constexpr std::uint16_t kLobbyMinigameRecordOpcode = 134U;
inline constexpr std::uint16_t kLobbyMinigameConfigSection = 3U;

/** @brief 配置必须按 42 字节原生字段逐项编码，不能复制内存结构体。 */
struct LobbyMinigameConfig final {
  /** @brief 费用与计时字段保持原生 WORD 范围。 */
  struct Basketball final {
    bool enabled{false};
    std::uint16_t cost_lucci{
        0U};  ///< +66, charged per shot (C2S571/1 echoes it).
    std::uint16_t gauge_lead_ms{0U};  ///< +68, 67E230 gauge lead-in.
    std::uint16_t hit_window_ms{
        0U};  ///< +70, 67F0C0 shot counts as hit inside this window.
    std::uint16_t round_end_ms{
        0U};  ///< +72, (+72 - +68) / 1000 seconds shown by 67D5E0.
  } basketball;

  /** @brief 票据字段是 DWORD 模板编号，不能当作玩家库存实例。 */
  struct Fishing final {
    bool enabled{false};
    std::uint16_t cost_lucci{0U};  ///< +78, multiplied by (today's count + 1).
    std::uint16_t alternate_cost_lucci{
        0U};  ///< +80, used by 666C40 for the alternate price.
    std::uint16_t cast_ms{0U};  ///< +82, 67A8A0 float timer.
    std::uint16_t bite_min_seconds{
        0U};  ///< +84, inclusive random lower bound in whole seconds.
    std::uint16_t bite_max_seconds{
        0U};  ///< +86, exclusive upper bound, or the same fixed value.
    std::uint16_t hook_window_seconds{
        0U};  ///< +88, inclusive final second relative to the bite.
    std::uint32_t premium_rod_item{0U};  ///< +92，高级鱼竿模板；0表示未启用。
    std::uint32_t free_pass_item{0U};    ///< +96，免费券模板；0表示未启用。
  } fishing;

  /** @brief 每日费用保持原生 WORD，重复请求的幂等性由服务端账本保证。 */
  struct Fortune final {
    bool enabled{false};
    std::uint16_t daily_cost_lucci{
        0U};  ///< +102, charged once per day (667170 prompt).
  } fortune;
};

/** @brief 原666C40/6800E0生成的计费回显；不是钓点，不授予消费权限。 */
struct LobbyFishingTerms final {
  bool premium_rod{false};            ///< 是否使用高级鱼竿。
  bool use_free_pass{false};          ///< 是否使用免费券。
  std::uint16_t base_cost{0U};        ///< 原配置基础价格，不是乘次数后的金额。
  std::uint16_t completed_count{0U};  ///< 客户端已知当日完成次数。
  friend bool operator==(const LobbyFishingTerms&,
                         const LobbyFishingTerms&) = default;
};

/** @brief 子编号决定变长请求字段，不得把不同动作共用同一固定载荷。 */
enum class LobbyMinigameRequestKind : std::uint16_t {
  kShootBasketball = 1U,  ///< 680E80: u16 cost, u32 hit, u32 reserved.
  kStartFishing = 10U,    ///< 670910: terms.
  kLiftFishingRod = 11U,  ///< 6811E0: terms, u32 hooked.
  kFortune = 12U,         ///< 6812B0: u16 cost, u64 paper, u32 reserved.
};

/** @brief displayed_cost 是客户端回显值，不能作为可信扣款金额。 */
struct LobbyMinigameRequest final {
  LobbyMinigameRequestKind kind{LobbyMinigameRequestKind::kShootBasketball};
  std::uint16_t displayed_cost{0U};
  bool hit{false};
  LobbyFishingTerms fishing;
  bool hooked{false};
  std::uint64_t fortune_paper{0U};
};

/** @brief 视觉子编号只能在同一广场转发，不能产生资产或完成记录。 */
enum class LobbyMinigameSyncKind : std::uint16_t {
  kShootFire = 100U,    ///< 671220 -> 66CEF0, shot animation type.
  kChangeState = 101U,  ///< 6813A0 -> 66D0D0, gamer state.
  kCastFishing = 200U,  ///< 6712F0 -> 66CFC0, rod visual.
  kLiftFishing = 201U,  ///< 6713C0 -> 66D050.
};

/** @brief 视觉同步不携带可信发送者身份，广播时必须补认证玩家编号。 */
struct LobbyMinigameSync final {
  LobbyMinigameSyncKind kind{LobbyMinigameSyncKind::kShootFire};
  std::uint32_t value{0U};
};

/** @brief 16-bit packed fortune paper cells (category, condition, line), at
 * most four. */
[[nodiscard]] bool ValidFortunePaper(std::uint64_t paper) noexcept;

[[nodiscard]] std::vector<std::uint8_t> EncodeLobbyMinigameConfig(
    const LobbyMinigameConfig& config);
[[nodiscard]] std::vector<std::uint8_t> EncodeLobbyMinigameRecord(
    std::uint16_t fishing_count, std::uint64_t fortune_paper);
[[nodiscard]] LobbyMinigameRequest DecodeLobbyMinigameRequest(
    std::span<const std::uint8_t> body);
[[nodiscard]] LobbyMinigameSync DecodeLobbyMinigameSync(
    std::span<const std::uint8_t> body);

/** @brief S2C571/1 (6718A0): result; >=10 adds lucci balance; 12 adds the
 * clean-shot item. */
[[nodiscard]] std::vector<std::uint8_t> EncodeBasketballReply(
    std::uint32_t result, std::uint32_t lucci, std::uint32_t item);
/** @brief S2C571/10 (671800): result, >=20 starts fishing, <=9 is 66E480
 * failure text. */
[[nodiscard]] std::vector<std::uint8_t> EncodeStartFishingReply(
    std::uint32_t result);
/** @brief S2C571/11 (477B60): result; >=10 adds paid, balance, count and 0/1/2
 * items. */
[[nodiscard]] std::vector<std::uint8_t> EncodeLiftFishingReply(
    std::uint32_t result, std::int32_t paid, std::uint32_t lucci,
    std::uint16_t count, std::span<const std::uint32_t> items);
/** @brief S2C571/12 (671650): result and paper; 10 adds lucci balance and lucky
 * item. */
[[nodiscard]] std::vector<std::uint8_t> EncodeFortuneReply(std::uint32_t result,
                                                           std::uint64_t paper,
                                                           std::uint32_t lucci,
                                                           std::uint32_t item);
/** @brief S2C572 (9763C0): source player, sub-ID and the relayed DWORD. */
[[nodiscard]] std::vector<std::uint8_t> EncodeLobbyMinigameSync(
    std::uint32_t player, const LobbyMinigameSync& sync);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_LOBBY_LOBBY_MINIGAME_MESSAGES_H_
