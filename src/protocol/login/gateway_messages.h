#ifndef BUBBLEFIGHTER_SERVER_PROTOCOL_LOGIN_GATEWAY_MESSAGES_H_
#define BUBBLEFIGHTER_SERVER_PROTOCOL_LOGIN_GATEWAY_MESSAGES_H_

/**
 * @file gateway_messages.h
 * @brief 定义 0.2.80.1 网关登录前已验证消息的类型化编解码接口。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf::protocol {

inline constexpr std::uint32_t kSupportedClientBuild =
    752U;  ///< 当前逆向样本唯一验证的 build。
inline constexpr std::size_t kMaximumHandshakeTextCodeUnits =
    4096U;  ///< C2S1 单字符串上限。
inline constexpr std::size_t kMaximumLoginAddresses =
    10U;  ///< C2S3 地址数量上限。
inline constexpr std::size_t kMaximumBootstrapValues =
    128U;  ///< C2S102 数组上限。
inline constexpr std::size_t kMaximumTutorialTextCodeUnits =
    64U;  ///< C2S107 单字符串上限。
inline constexpr std::size_t kMaximumLegacyMenuTextCodeUnits =
    64U;  ///< C2S2022 文本上限。

/** @brief C2S1 补丁握手；未知文本保持原顺序，不擅自赋予业务含义。 */
struct PatchHandshakeRequest final {
  std::uint32_t build;  ///< 客户端构建号；当前仅验证 752。
  std::array<std::u16string, 7U> first_texts;  ///< B8E6E0 的前七个字符串。
  std::uint32_t reserved;                      ///< 两组字符串之间的原始 DWORD。
  std::array<std::u16string, 15U> last_texts;  ///< B8E6E0 的后十五个字符串。
};

/** @brief C2S21 客户端环境报告。 */
struct EnvironmentReport final {
  std::u16string
      text;  ///< 已验证 UTF-16，长度在 frame 特例与消息解析中双重受限。
};

/** @brief C2S3 登录请求；三个字符串的具体客户端业务名称尚未全部恢复。 */
struct LoginRequest final {
  std::array<std::u16string, 3U>
      texts;  ///< 第一项可承载启动器票据或离线测试身份。
  std::vector<std::uint32_t>
      ipv4_addresses;        ///< 客户端按 u32 写入的 IPv4 地址数组。
  std::uint32_t client_key;  ///< 979CE0 尾部 client key。
  std::uint32_t mode;        ///< 979CE0 尾部模式字段。
};

/** @brief C2S102 登录后初始化请求。 */
struct PostLoginBootstrapRequest final {
  std::uint32_t first_value;          ///< B8D040 的首个尚未命名 DWORD。
  std::uint32_t second_value;         ///< B8D040 的第二个尚未命名 DWORD。
  std::vector<std::uint32_t> values;  ///< 最多 128 项的 DWORD vector。
};

/** @brief C2S18 网关退出请求。 */
struct GatewayExitRequest final {
  std::uint32_t
      main_app_state_is_one;  ///< C54810 写出的四字节布尔来源；兼容层保留原值。
};

/** @brief S2C16 创建角色阶段的中心人数/路由确认结果。 */
struct CharacterCreationCenterResponse final {
  std::uint32_t
      result;  ///< 0 表示中心路由已就绪；非零错误码由原客户端本地提示。
};

/** @brief C2S31 首次角色昵称可用性检查。 */
struct NicknameCheckRequest final {
  std::u16string
      nickname;  ///< 候选昵称；业务层另做宽度、字符集、NFKC 和唯一性校验。
};

/** @brief C2S32 首次角色原子创建请求。 */
struct CreateCharacterRequest final {
  std::u16string nickname;  ///< 已通过昵称检查后再次发送的候选昵称。
  std::uint32_t
      fighter_id;  ///< 原版四个选项使用 1、2、11、12；codec 不代替领域校验。
  std::uint32_t reserved;     ///< B8ED10 中尚未命名的 DWORD。
  std::u16string extra_text;  ///< B8ED10 中保留的额外字符串。
  std::uint32_t region;       ///< B8ED10 尾部地区 DWORD。
};

/**
 * @brief C2S107
 * 原生单人场景请求，prologue按资源profile归类103，其他任务仍保护。
 * @note 线格式为13个MapMeta/rule
 * DWORD、1字节map_rule_flags、3个UTF-16字段和Difficulty；
 *       不含mode103编号或权威成绩。原资源99999秒是clock
 * sentinel，不得作为竞技比赛时长。
 */
struct TutorialStartReport final {
  std::array<std::uint32_t, 13U>
      values;  ///< 原MapMeta/rule数值；不能据客户端值授分或改教程时限。
  std::uint8_t
      kind;  ///< map_rule_flags；prologue为0x10，不等同SceneConfiguration.field8。
  std::u16string relation_key;  ///< Directory，如rel_prologue_01。
  std::u16string display_text;  ///< 本地化显示名称，不作为权限或奖励依据。
  std::u16string
      mission_key;  ///< 原MapMeta键；prologue或尚未恢复的sin_mission族。
  std::uint32_t trailing_value;  ///< Difficulty；不是stageNum或客户端进度。
};

/**
 * @brief C2S2022 菜单辅助文本上报的已验证线格式。
 * @note 94 份原版样本均为 DWORD、UTF-16 字符串、DWORD；它同时出现在大厅和广场，
 *       但字段业务语义尚未恢复，因此只保留顺序和原值，不据此更新玩家状态。
 */
struct LegacyMenuTextReport final {
  std::uint32_t first_value;  ///< 首个未命名 DWORD；样本包含 0、99 至 102。
  std::u16string text;  ///< 客户端上报文本；现有样本为 4 至 8 个 UTF-16 码元。
  std::uint32_t
      trailing_value;  ///< 尾部未命名 DWORD；保留包括补码负值在内的原始位模式。
};

/**
 * @brief C2S959 菜单辅助状态上报的已验证线格式。
 * @note 10 份原版样本均为三个 DWORD，出现在大厅和广场；字段语义尚未恢复。
 */
struct LegacyMenuStateReport final {
  std::array<std::uint32_t, 3U>
      values;  ///< 三个未命名 DWORD 原值，不作为服务端权威状态。
};

/** @brief S2C2 补丁检查结果。 */
struct PatchHandshakeResponse final {
  bool needs_patch;          ///< false 时当前客户端继续进入阶段 10。
  std::u16string patch_url;  ///< 当前兼容服务在无需补丁时发送空字符串。
};

/** @brief S2C17 三组进程/环境字符串列表。 */
struct ProcessProbeResponse final {
  std::array<std::vector<std::u16string>, 3U>
      groups;  ///< B9D230 固定读取的三个 vector。
};

/** @brief S2C19 的单个网关端点记录。 */
struct GatewayEndpoint final {
  std::uint32_t first_value{1U};   ///< 客户端结构第一个未命名 DWORD。
  std::uint32_t second_value{1U};  ///< 客户端结构第二个未命名 DWORD。
  std::uint32_t third_value{1U};   ///< 客户端结构第三个未命名 DWORD。
  std::array<std::uint8_t, 4U>
      ipv4_address;    ///< 按网络字节顺序写入的 IPv4 地址。
  std::uint16_t port;  ///< 按协议小端写入的网关端口。
};

/** @brief S2C15 昵称检查结果。 */
struct NicknameCheckResponse final {
  std::uint32_t result;     ///< 当前兼容约定 0 成功、非零失败。
  std::u16string nickname;  ///< 原样关联到本次检查的候选昵称。
};

/** @brief S2C3 登录成功主体及紧随其后的 LoginNational 子结构。 */
struct LoginSuccessResponse final {
  std::u16string account_name;               ///< 账号显示名。
  std::uint32_t player_id;                   ///< 协议边界的 32 位旧 uid。
  std::u16string nickname;                   ///< 玩家显示昵称。
  std::uint32_t initial_fighter_id;          ///< HUD 回退角色；不得为零。
  std::array<std::uint32_t, 3U> reserved{};  ///< 473F20 尚未命名的三个 DWORD。
  double server_date_ole;           ///< Windows OLE Automation 日期天数。
  std::uint32_t main_reserved{0U};  ///< 主登录结构日期后的 DWORD。
  std::uint32_t national_result{
      0U};  ///< LoginNational 结果；0 时继续读取字符串。
  std::u16string national_message;  ///< national_result 为 0 时存在的字符串。
};

/** @brief S2C3 登录失败的短结构。 */
struct LoginFailureResponse final {
  std::uint32_t result;  ///< 非零主结果码。
  std::uint32_t detail;  ///< 失败路径额外读取的 DWORD。
};

/** @brief 严格解析 C2S1，并拒绝截断、超长 UTF-16 和尾随字段。 */
[[nodiscard]] PatchHandshakeRequest DecodePatchHandshakeRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S21；上限与长帧兼容分支一致。 */
[[nodiscard]] EnvironmentReport DecodeEnvironmentReport(
    std::span<const std::uint8_t> body,
    std::size_t maximum_code_units = 524'288U);

/** @brief 严格解析 C2S3，并将地址 vector 限制为十项。 */
[[nodiscard]] LoginRequest DecodeLoginRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S102，并限制尾部 DWORD vector 的数量。 */
[[nodiscard]] PostLoginBootstrapRequest DecodePostLoginBootstrapRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S18 的单个 DWORD，不把非零值擅自规范化。 */
[[nodiscard]] GatewayExitRequest DecodeGatewayExitRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格校验 C2S22 创建角色中心查询必须为空载荷。 */
void DecodeCharacterCreationCenterRequest(std::span<const std::uint8_t> body);

/** @brief 严格校验 C2S33 创建角色阶段确认必须为空载荷。 */
void DecodeCharacterCreationCenterAck(std::span<const std::uint8_t> body);

/** @brief 严格校验 C2S521 客户端保护/状态就绪报告必须为空载荷。 */
void DecodeClientStatusReadyRequest(std::span<const std::uint8_t> body);

/** @brief 严格校验 C2S522 客户端保护状态更新报告必须为空载荷。 */
void DecodeClientStatusUpdate(std::span<const std::uint8_t> body);

/**
 * @brief 严格校验 C2S922 菜单辅助模块就绪信号必须为空载荷。
 * @note
 * 现有原版抓包只证明该信号在菜单初始化后出现且服务端无需响应；具体子系统语义尚未恢复。
 */
void DecodeMenuAuxiliaryReadyReport(std::span<const std::uint8_t> body);

/**
 * @brief 严格解析 C2S2022 的 DWORD、UTF-16 字符串、DWORD 结构。
 * @note 仅验证已捕获的线格式；调用方不得据此推断未恢复的业务含义。
 */
[[nodiscard]] LegacyMenuTextReport DecodeLegacyMenuTextReport(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S959 的三个 DWORD，并拒绝截断或尾随字段。 */
[[nodiscard]] LegacyMenuStateReport DecodeLegacyMenuStateReport(
    std::span<const std::uint8_t> body);

/**
 * @brief 严格解析 C2S107 的已验证结构，并将每个 UTF-16 字符串限制为 64 个码元。
 * @note 本函数只证明载荷结构合法；教程流程、完成条件和奖励均未恢复。
 */
[[nodiscard]] TutorialStartReport DecodeTutorialStartReport(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S31 候选昵称，不在协议层实现 Unicode 业务规则。 */
[[nodiscard]] NicknameCheckRequest DecodeNicknameCheckRequest(
    std::span<const std::uint8_t> body);

/** @brief 严格解析 C2S32 的昵称、角色、保留字段、额外字符串和地区。 */
[[nodiscard]] CreateCharacterRequest DecodeCreateCharacterRequest(
    std::span<const std::uint8_t> body);

/** @brief 编码 S2C2 的 DWORD 结果和补丁 URL。 */
[[nodiscard]] std::vector<std::uint8_t> EncodePatchHandshakeResponse(
    const PatchHandshakeResponse& response);

/** @brief 编码 S2C17 的三个 UTF-16 vector，并校验所有计数可表示为 u32。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeProcessProbeResponse(
    const ProcessProbeResponse& response);

/** @brief 编码 S2C19 网关 vector；IP 保持网络字节序，其他整数保持协议小端。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeGatewayListResponse(
    std::span<const GatewayEndpoint> endpoints);

/** @brief 编码 S2C14 空负载，通知客户端进入原版首次角色阶段。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCharacterCreationRequired();

/** @brief 编码 S2C16 创建角色中心确认结果。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeCharacterCreationCenterResponse(
    const CharacterCreationCenterResponse& response);

/** @brief 编码 S2C15 昵称检查结果。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNicknameCheckResponse(
    const NicknameCheckResponse& response);

/** @brief 编码 S2C3 成功结构；拒绝非有限日期和零回退角色。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLoginSuccessResponse(
    const LoginSuccessResponse& response);

/** @brief 编码 S2C3 失败短结构；result 必须非零。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeLoginFailureResponse(
    const LoginFailureResponse& response);

/** @brief 编码 S2C109 的零类型字节和 UTF-16 提示文本。 */
[[nodiscard]] std::vector<std::uint8_t> EncodeNoticeResponse(
    std::u16string_view message);

/** @brief 将 Unix 秒数转换为客户端使用的 OLE Automation 日期天数。 */
[[nodiscard]] double UnixSecondsToOleDate(double unix_seconds) noexcept;

}  // namespace bf::protocol

#endif  // BUBBLEFIGHTER_SERVER_PROTOCOL_LOGIN_GATEWAY_MESSAGES_H_
