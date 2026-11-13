#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_MATCH_TRANSITION_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_MATCH_TRANSITION_MESSAGES_H_

/**
 * @file match_transition_messages.h
 * @brief 定义等待房间配置、开局和地图加载阶段的已恢复消息。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "features/battle/match_reward.h"
#include "protocol/battle/weapon_master_messages.h"
#include "protocol/scenes/scene_messages.h"

namespace bf::protocol {

inline constexpr std::size_t kMatchRuleFieldCount =
    13U;  ///< 489DD0 固定规则 DWORD 数。
inline constexpr std::size_t kMapAssetVectorLimit =
    4096U;  ///< 单组地图资产最大元素数。

/** @brief C2S503 普通地图开局请求。 */
struct StartMatchRequest final {
  bool force{false};            ///< 是否跳过非房主准备检查。
  std::string map_key;          ///< 地图资源键，不能为空。
  std::string display_name;     ///< 地图显示名。
  std::string mode_key;         ///< MapMeta 配置键或随机选择标记，不能为空。
  std::uint32_t map_value{0U};  ///< 地图元数据原值。
  std::array<std::uint32_t, kMatchRuleFieldCount>
      rules{};                  ///< 489DD0 十三个规则字段。
  std::uint8_t final_flag{0U};  ///< 489DD0 尾部未命名单字节。

  /** @brief 按普通开局请求的全部字段比较。 */
  bool operator==(const StartMatchRequest&) const = default;
};

/** @brief C2S504 轮换地图开局请求。 */
struct StartRotatingMatchRequest final {
  bool force{false};                     ///< 是否跳过非房主准备检查。
  std::array<std::string, 3U> map_keys;  ///< 三个轮换地图键，首项为本轮地图。
  std::string display_name;              ///< 本轮地图显示名。
  std::string mode_key;         ///< MapMeta 配置键或随机选择标记，不能为空。
  std::uint32_t map_value{0U};  ///< 地图元数据原值。
  std::array<std::uint32_t, kMatchRuleFieldCount>
      rules{};                  ///< 489DD0 十三个规则字段。
  std::uint8_t final_flag{0U};  ///< 489DD0 尾部未命名单字节。

  /** @brief 按轮换开局请求的全部字段比较。 */
  bool operator==(const StartRotatingMatchRequest&) const = default;
};

/**
 * @brief C2S513 房间设置对话框提交值。
 * @note 字符串顺序是显示名、地图键、辅助标签、家族键，与 SceneConfiguration
 * 内存顺序不同。
 */
struct RoomConfigurationRequest final {
  std::string display_name;                ///< SceneConfiguration 偏移 84。
  std::string map_key;                     ///< SceneConfiguration 偏移 60。
  std::string auxiliary_label;             ///< SceneConfiguration 偏移 108。
  std::string family_key;                  ///< SceneConfiguration 偏移 36。
  bool option_319{false};                  ///< 偏移 319 的 DWORD 布尔值。
  bool option_320{false};                  ///< 偏移 320 的 DWORD 布尔值。
  std::uint32_t selected_duration_ms{0U};  ///< 当前时长及模式相关时长槽。
  std::uint32_t score_target{0U};          ///< 偏移 16 的比分/回合目标。
  std::array<std::uint32_t, 4U>
      reserved{};                ///< 对话框中间四个尚未映射的 DWORD。
  std::uint32_t map_value{0U};   ///< 偏移 132 的地图元数据。
  std::uint32_t mode_id{0U};     ///< 偏移 0 的模式编号。
  std::uint32_t submode_id{0U};  ///< 偏移 4 的子模式编号。
  bool automatic_option{false};  ///< 偏移 12 的 DWORD 布尔值。

  /** @brief 按房间配置请求的全部字段比较。 */
  bool operator==(const RoomConfigurationRequest&) const = default;
};

/** @brief C2S514 更新三个轮换地图槽之一。 */
struct RotatingMapSelectionRequest final {
  std::uint32_t index{0U};  ///< 仅允许 0、1、2。
  std::string family_key;   ///< 目标槽的模式族键。
  std::string map_key;      ///< 目标槽的地图键。

  /** @brief 按轮换地图选择字段比较。 */
  bool operator==(const RotatingMapSelectionRequest&) const = default;
};

/** @brief C2S701 客户端加载完成的四个原始 DWORD。 */
struct LoadingCompleteRequest final {
  std::array<std::uint32_t, 4U> values{};  ///< 前置倒计时、完成值及两个保留值。

  /** @brief 按加载完成请求字段比较。 */
  bool operator==(const LoadingCompleteRequest&) const = default;
};

/** @brief 地图资产报告中的一个有限三维坐标。 */
struct MapAssetPosition final {
  float x{0.0F};  ///< X 坐标。
  float y{0.0F};  ///< Y 坐标。
  float z{0.0F};  ///< Z 坐标。

  /** @brief 按地图资源位置向量比较。 */
  bool operator==(const MapAssetPosition&) const = default;
};

/**
 * @brief C2S702 地图对象数量与十四组异构资产 vector。
 * @note 各组语义尚未完全恢复，按读取顺序编号；字段类型来自客户端
 * 7ED6F0，不能合并成裸字节。
 */
struct MapAssetsRequest final {
  std::uint32_t object_count{0U};          ///< 客户端报告的地图对象总数原值。
  std::vector<MapAssetPosition> field_0;   ///< 第 0 组：三个 float。
  std::vector<std::uint32_t> field_1;      ///< 第 1 组：DWORD。
  std::vector<std::uint8_t> field_2;       ///< 第 2 组：单字节。
  std::vector<std::uint32_t> field_3;      ///< 第 3 组：DWORD。
  std::vector<std::uint32_t> field_4;      ///< 第 4 组：DWORD。
  std::vector<std::uint32_t> field_5;      ///< 第 5 组：DWORD。
  std::vector<MapAssetPosition> field_6;   ///< 第 6 组：三个 float。
  std::vector<std::uint32_t> field_7;      ///< 第 7 组：DWORD。
  std::vector<std::uint32_t> field_8;      ///< 第 8 组：DWORD。
  std::vector<std::uint32_t> field_9;      ///< 第 9 组：DWORD。
  std::vector<MapAssetPosition> field_10;  ///< 第 10 组：三个 float。
  std::vector<std::uint32_t> field_11;     ///< 第 11 组：DWORD。
  std::vector<std::uint32_t> field_12;     ///< 第 12 组：DWORD。
  std::vector<std::string> field_13;       ///< 第 13 组：UTF-16 字符串。

  /** @brief 按全部地图资源组比较。 */
  bool operator==(const MapAssetsRequest&) const = default;
};

/** @brief C2S709 声明本地战斗角色对象 ID。 */
struct PlayerObjectIdentityRequest final {
  std::uint32_t object_id{0U};  ///< 必须由房间运行时按席位推导并复核。
};

/** @brief S2C510 从等待房间切换到地图的参数。 */
struct StartSceneResponse final {
  std::string map_key;            ///< 会话地图资源键。
  std::string mode_key;           ///< 会话MapMeta 配置键或随机选择标记。
  std::uint32_t stage{3U};        ///< 当前已验证固定值 3。
  std::uint32_t round_index{0U};  ///< 轮换地图的零基回合索引。
  std::int8_t field_0{0};         ///< 尚未命名的有符号字节。
  std::uint8_t field_1{0U};       ///< 尚未命名的无符号字节。
  std::uint32_t field_2{0U};      ///< 尚未命名的 DWORD。
  std::uint8_t field_3{0U};       ///< 尾部尚未命名字节。
};

/** @brief S2C702 三个地图录制/加载可选标志。 */
struct LoadingOptionsResponse final {
  bool first{false};   ///< 第一个 DWORD 布尔值。
  bool second{false};  ///< 第二个 DWORD 布尔值。
  bool third{false};   ///< 第三个 DWORD 布尔值。
};

/** @brief S2C704 正式开始比赛所需的时长与场景值。 */
struct MatchBeginResponse final {
  std::uint32_t duration_ms{0U};  ///< 比赛时长毫秒。
  std::uint32_t scene_id{0U};     ///< 写入客户端全局场景值的 DWORD。
  std::uint32_t reserved{0U};     ///< 客户端 manager 字段，当前为零。
};

/** @brief S2C729 中一名开局冻结玩家的原生成绩行。 */
struct NativeScorePlayer final {
  std::uint32_t player_id{0U};           ///< 原生 DWORD 玩家编号。
  std::uint8_t seat{0U};                 ///< 36 位占位掩码中的稳定席位。
  std::uint8_t team{1U};                 ///< 团队1/2；13/23个人阵营1..16。
  std::string nickname;                  ///< 开局昵称；离场后仍保留。
  std::uint32_t level{1U};               ///< 已确认等级快照。
  domain::MatchPerformance performance;  ///< 已确认泡封、击爆、死亡和救援。
  std::optional<domain::AppliedMatchReward>
      reward;              ///< 仅使用已提交事务回执，绝不预测奖励。
  std::uint32_t mode{1U};  ///< 冻结模式决定目标积分字段。
  bool surviving{true};    ///< 个人生存榜优先排列仍在场的存活者。
  std::optional<WeaponMasterProgress>
      weapon_master;  ///< 仅模式 26 使用，禁止以全零尾部冒充经验。
};

/**
 * @brief 编码 S2C729 原生
 * ScoreData；内部按席位排序，保留稀疏掩码和独立领奖台位置。
 * @param players 开局玩家完整名单，1～36 名，不允许重复席位或玩家。
 * @param winner 实时榜使用 255；终局为团队 1/2 或平局/个人局 0。
 * @note
 * 此报文只公布确认战绩；尚未取得奖励事务回执的增量字段保持零，不预测发奖。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNativeScoreResponse(
    std::span<const NativeScorePlayer> players, std::uint8_t winner,
    std::optional<std::array<std::uint32_t, 2U>> round_scores = std::nullopt);

/** @brief 编码 S2C705；感染模式末字段是结束原因（1=正常，2=超时），不是胜方。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMatchStopResponse(
    std::uint8_t winner, std::uint32_t mode = 1U, bool timed_out = false);

/** @brief 严格解析 C2S503 普通开局请求。 */
[[nodiscard]] StartMatchRequest DecodeStartMatchRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S504 轮换地图开局请求。 */
[[nodiscard]] StartRotatingMatchRequest DecodeStartRotatingMatchRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S513 房间设置，并验证三个 DWORD 布尔值。 */
[[nodiscard]] RoomConfigurationRequest DecodeRoomConfigurationRequest(
    std::span<const std::uint8_t> body);

/** @brief 将已验证 C2S513 字段映射到完整配置，同时保留未编辑字段。 */
void ApplyRoomConfiguration(SceneConfiguration& destination,
                            const RoomConfigurationRequest& request);

/** @brief 按当前选择、原生模式时长槽和服务回退解析本局时长（毫秒）。 */
[[nodiscard]] std::uint32_t ConfiguredMatchDurationMs(
    const SceneConfiguration& configuration,
    std::uint32_t fallback_ms) noexcept;

/** @brief 严格解析 C2S514，并限制轮换索引为 0～2。 */
[[nodiscard]] RotatingMapSelectionRequest DecodeRotatingMapSelectionRequest(
    std::span<const std::uint8_t> body);

/** @brief 只更新所选轮换槽的家族键和地图键。 */
void ApplyRotatingMapSelection(SceneConfiguration& destination,
                               const RotatingMapSelectionRequest& request);

/** @brief 严格解析 C2S701 的四个 DWORD。 */
[[nodiscard]] LoadingCompleteRequest DecodeLoadingCompleteRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S702，并限制 vector、文本和坐标值。 */
[[nodiscard]] MapAssetsRequest DecodeMapAssetsRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S709 的战斗对象 ID。 */
[[nodiscard]] PlayerObjectIdentityRequest DecodePlayerObjectIdentityRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C505 完整 SceneConfiguration 更新。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomConfigurationResponse(
    const SceneConfiguration& configuration);

/** @brief 编码 S2C510 地图、模式和轮次切换值。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeStartSceneResponse(
    const StartSceneResponse& response);

/** @brief 编码 S2C701 需要激活的成员席位。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeActivateSlotResponse(
    std::uint32_t slot);

/** @brief 编码 S2C702 三个 DWORD 布尔标志。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLoadingOptionsResponse(
    const LoadingOptionsResponse& response);

/** @brief 编码 S2C703 开局前倒计时毫秒。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCountdownResponse(
    std::uint32_t duration_ms);

/** @brief 编码 S2C704 比赛时长、场景值和保留 DWORD。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeMatchBeginResponse(
    const MatchBeginResponse& response);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_BATTLE_MATCH_TRANSITION_MESSAGES_H_
