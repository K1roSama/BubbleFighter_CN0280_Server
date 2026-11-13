#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_MESSAGES_H_

/**
 * @file scene_messages.h
 * @brief 定义 752 客户端 SceneConfiguration、SceneInfo
 * 与房间列表/创建/加入消息。
 */

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "protocol/codec/packet_io.h"

namespace bf::protocol {

inline constexpr std::uint32_t kSceneRandomTeams =
    0x20U;  ///< 配置 +8：890110 的随机组标志。

/** @brief SceneConfiguration 偏移 289 的两个 DWORD 限制项。 */
struct SceneConfigPair final {
  std::uint32_t first{0U};   ///< 第一个尚未命名的原始 DWORD。
  std::uint32_t second{0U};  ///< 第二个尚未命名的原始 DWORD。

  /** @brief 按两个原始字段比较。 */
  bool operator==(const SceneConfigPair&) const = default;
};

/** @brief SceneConfiguration 偏移 302 的标识与 UTF-16 名称项。 */
struct SceneConfigNamedValue final {
  std::uint32_t value{0U};  ///< 尚未确认语义的原始标识。
  std::string name;         ///< 严格 UTF-8 应用文本；线协议使用 UTF-16LE。

  /** @brief 按名称和值比较。 */
  bool operator==(const SceneConfigNamedValue&) const = default;
};

/** @brief 一个轮换地图槽位的MapMeta 配置键和资源目录。 */
struct SceneRotatingMap final {
  std::string
      family_key;  ///< 内存偏移 136/184/232 的MapMeta 配置键或随机选择标记。
  std::string map_key;  ///< 内存偏移 160/208/256 的资源目录键。

  /** @brief 按地图名称、权重和保留字段比较。 */
  bool operator==(const SceneRotatingMap&) const = default;
};

/**
 * @brief 按客户端 `489870/489B60` 固定顺序保存完整 SceneConfiguration。
 * @note
 * 未知字段用原内存偏移命名并原样往返，不能猜成座位数、布尔值或时长。已确认字段使用
 *       业务名；所有动态 vector 最大 128 项，所有文本最大 256 个 UTF-16
 * 代码单元。
 */
struct SceneConfiguration final {
  std::uint32_t mode_id{1U};     ///< 偏移 0：模式标识。
  std::uint32_t submode_id{0U};  ///< 偏移 4：子模式/地图选择值。
  std::uint32_t field_8{0U};  ///< 偏移 8：模式标志；0x20 为随机组，其他位保留。
  std::uint32_t field_12{
      0U};  ///< 偏移 12：玩家主办赏金房间标志，复制到 GameSession+140。
  std::uint32_t field_13{0U};  ///< 偏移 13：打包内存字段，线协议仍为 DWORD。
  std::uint32_t field_14{0U};  ///< 偏移 14：打包内存字段，线协议仍为 DWORD。
  std::uint32_t score_target{0U};  ///< 偏移 16：比分/回合目标，不是座位数。
  std::uint32_t selected_duration_ms{0U};  ///< 偏移 20：当前 UI 选择的时长值。
  std::uint32_t field_24{0U};              ///< 偏移 24：普通模式时长槽。
  std::uint32_t field_28{0U};              ///< 偏移 28：特定模式时长槽。
  std::uint32_t field_32{0U};              ///< 偏移 32：回合/生存模式时长槽。
  std::string family_key;  ///< 偏移 36：MapMeta 配置键或随机选择标记。
  std::string map_key;     ///< 偏移 60：当前地图资源目录（MapMeta.Directory）。
  std::string display_name;     ///< 偏移 84：地图显示名。
  std::string auxiliary_label;  ///< 偏移 108：辅助标签。
  std::uint32_t map_value{0U};  ///< 偏移 132：地图/选择附加值。
  std::array<SceneRotatingMap, 3> rotating_maps;  ///< 三组轮换地图。
  std::uint32_t field_280{0U};             ///< 偏移 280：语义未确认的 DWORD。
  std::uint32_t field_284{0U};             ///< 偏移 284：语义未确认的 DWORD。
  std::uint32_t field_288{0U};             ///< 偏移 288：语义未确认的 DWORD。
  std::vector<SceneConfigPair> field_289;  ///< 偏移 289：DWORD 对 vector。
  std::uint32_t field_301{0U};             ///< 偏移 301：语义未确认的 DWORD。
  std::vector<SceneConfigNamedValue> field_302;  ///< 偏移 302：命名值 vector。
  std::uint32_t field_314{0U};                   ///< 偏移 314：语义未确认。
  std::uint32_t field_315{0U};                   ///< 偏移 315：语义未确认。
  std::uint32_t field_316{1U};                   ///< 偏移 316：构造器默认启用。
  std::uint32_t field_317{0U};                   ///< 偏移 317：语义未确认。
  std::uint32_t field_318{0U};                   ///< 偏移 318：语义未确认。
  std::uint32_t field_319{1U};                   ///< 偏移 319：构造器默认启用。
  std::uint32_t field_320{1U};                   ///< 偏移 320：构造器默认启用。
  std::uint8_t field_324{0U};                    ///< 偏移 324：唯一单字节字段。
  std::uint32_t field_328{0U};                   ///< 偏移 328：语义未确认。
  std::vector<std::string> field_332;  ///< 偏移 332：UTF-16 字符串 vector。
  std::uint32_t field_344{0U};         ///< 偏移 344：语义未确认。
  std::uint32_t field_348{0U};         ///< 偏移 348：语义未确认。
  std::uint32_t field_352{0U};         ///< 偏移 352：语义未确认。
  std::uint32_t field_356{0U};         ///< 偏移 356：语义未确认。
  std::string field_360;               ///< 偏移 360：语义未确认的文本。
  std::string field_384;               ///< 偏移 384：语义未确认的文本。
  std::uint32_t field_408{
      0xFFFFFFFFU};  ///< 偏移 408：未选择哨兵默认为 UINT32_MAX。

  /** @brief 按全部房间配置字段比较。 */
  bool operator==(const SceneConfiguration&) const = default;
};

/** @brief 客户端 `48A2C0/48A4A0` 完整场景描述。 */
struct SceneInfo final {
  std::uint16_t scene_id{1U};  ///< 服务端分配的房间/场景短标识。
  std::uint32_t kind{0U};      ///< 等待房间为 2，加载中为 3，加载完成为 4。
  std::string name{
      "Local Plaza"};  ///< 场景/房间显示名，最多 64 个 UTF-16 代码单元。
  std::uint16_t occupants{1U};       ///< 当前玩家数。
  std::uint16_t observers{0U};       ///< 当前观战人数。
  std::uint16_t capacity{12U};       ///< 玩家容量。
  std::uint32_t locked{0U};          ///< 是否带房间口令的 DWORD 标志。
  std::string field_40;              ///< 等待房间列表显示的房主昵称。
  std::uint32_t scene_type{3U};      ///< 1 主菜单、2 房间、3 自由广场。
  SceneConfiguration configuration;  ///< 嵌套完整场景配置。
  std::uint32_t field_68{0U};        ///< 语义未确认，服务端创建时清零。
  std::uint64_t key{0U};             ///< 场景键；房间当前使用服务端 scene_id。
  std::string field_80;              ///< 语义未确认的文本。
  std::uint32_t field_104{0U};       ///< 语义未确认的尾 DWORD。
  std::uint32_t field_108{0U};       ///< 语义未确认的尾 DWORD。
  std::uint32_t field_112{0U};       ///< 语义未确认的尾 DWORD。
  std::uint32_t field_116{0U};       ///< 语义未确认的尾 DWORD。

  /** @brief 按完整场景列表快照比较。 */
  bool operator==(const SceneInfo&) const = default;
};

/** @brief C2S401 房间列表分页请求。 */
struct RoomListRequest final {
  std::uint16_t page{0U};       ///< 零基页码。
  std::uint16_t page_size{0U};  ///< 客户端通常为 10；服务端接受 1～100。
};

/** @brief C2S402 完整场景模板和独立口令。 */
struct CreateRoomRequest final {
  SceneInfo
      requested_scene;   ///< 仅配置/显示字段可采用，身份与人数由服务端覆盖。
  std::string password;  ///< 最多 32 个 UTF-16 代码单元，不得记录原文。
};

/** @brief C2S403 加入房间请求。 */
struct JoinRoomRequest final {
  std::uint32_t scene_id{0U};      ///< 线协议中的房间 DWORD 标识。
  std::string password;            ///< 独立房间口令，不得记录原文。
  std::uint32_t entry_option{0U};  ///< 0 为普通加入；其他入口尚未支持。
};

/** @brief C2S114 LeaveHistory 的原生三 DWORD SessionKey。 */
struct QuickJoinHistory final {
  std::uint32_t channel{0U};   ///< 离场时的频道。
  std::uint32_t scene_id{0U};  ///< 旧 SceneInfo 的短编号，线上扩为 DWORD。
  std::uint32_t field_68{0U};  ///< 旧 SceneInfo 附加标识。
};

/** @brief C2S114 普通快速进入；不包含频道 80 使用的 C2S113 战队队列。 */
struct QuickJoinRequest final {
  std::uint32_t mode_id{0U};      ///< 客户端选中的模式。
  std::uint32_t size_option{3U};  ///< 1：3v3 以下，2：4v4 以上，3：全部。
  std::vector<QuickJoinHistory>
      history;  ///< 原生客户端最多保留 11 项离房历史。
};

inline constexpr std::size_t kMaximumPlazaUserList =
    1000U;  ///< 每包本服上限，原生逐项无分页。

/** @brief S2C113：昵称、普通等级及 ArenaLevel 两个 DWORD。 */
struct PlazaUserListEntry final {
  std::string nickname;            ///< 当前广场玩家的真实昵称。
  std::uint32_t level{1U};         ///< 已发布成长规则计算的普通等级。
  std::uint32_t arena_first{0U};   ///< 尚无竞技场赛季成绩时保持原生初始零。
  std::uint32_t arena_second{0U};  ///< ArenaLevel 第二字段，同样为未定级零。
};

/** @brief 编码 B9D520/45C1D0 的数量和逐人资料，昵称最多 12 个 UTF16 单元。 */
[[nodiscard]] std::vector<std::uint8_t> EncodePlazaUserListResponse(
    std::span<const PlazaUserListEntry> players);

/** @brief 严格解析 88D480 的两个 DWORD 和三 DWORD 历史项。 */
[[nodiscard]] QuickJoinRequest DecodeQuickJoinRequest(
    std::span<const std::uint8_t> body);
/** @brief S2C120：结果、附加值零、空 UTF16 文本；成功后必须接实际场景。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeQuickJoinResponse(
    std::uint32_t result);

/** @brief 从 reader 当前位置解析完整 SceneConfiguration，不要求负载结束。 */
[[nodiscard]] SceneConfiguration DecodeSceneConfiguration(PacketReader& reader);

/** @brief 按固定线顺序把完整 SceneConfiguration 追加到 writer。 */
void EncodeSceneConfiguration(PacketWriter& writer,
                              const SceneConfiguration& configuration);

/** @brief 从 reader 当前位置解析完整 SceneInfo，不要求负载结束。 */
[[nodiscard]] SceneInfo DecodeSceneInfo(PacketReader& reader);

/** @brief 按固定线顺序把完整 SceneInfo 追加到 writer。 */
void EncodeSceneInfo(PacketWriter& writer, const SceneInfo& scene);

/** @brief 严格解析 C2S401 并拒绝无效页大小和尾随字节。 */
[[nodiscard]] RoomListRequest DecodeRoomListRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S402；SceneInfo 后必须且只能有一个独立口令。 */
[[nodiscard]] CreateRoomRequest DecodeCreateRoomRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S403 房间、口令和入口选项。 */
[[nodiscard]] JoinRoomRequest DecodeJoinRoomRequest(
    std::span<const std::uint8_t> body);

/**
 * @brief 编码 S2C101：页码、has-next、保留零和 SceneInfo vector。
 * @param page 客户端请求对应的零起始页码。
 * @param has_next 查询层确认是否仍有下一页。
 * @param scenes 当前页最多 100 项的场景快照。
 */
[[nodiscard]] std::vector<std::uint8_t> EncodeRoomListResponse(
    std::uint32_t page, bool has_next, std::span<const SceneInfo> scenes);

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_SCENES_SCENE_MESSAGES_H_
