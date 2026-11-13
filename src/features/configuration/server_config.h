#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVER_CONFIG_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVER_CONFIG_H_

/**
 * @file server_config.h
 * @brief 定义服务进程配置结构、严格文件加载和环境约束校验接口。
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "application/error.h"
#include "application/event_logger.h"
#include "features/battle/weapon_master_match.h"
#include "features/configuration/mysql_settings.h"
#include "features/lobby/lobby_minigame.h"
#include "features/rooms/room_runtime.h"
#include "protocol/codec/limits.h"

namespace bf::infrastructure::configuration {

/** @brief 配置适用环境；生产环境会启用额外安全约束。 */
enum class Environment : std::uint8_t {
  kDevelopment,
  kTest,
  kProduction,
};

/** @brief 网络监听与每连接资源边界。 */
struct NetworkConfig final {
  std::string bind_address;    ///< 监听地址；测试配置应使用回环地址。
  std::uint16_t port{0};       ///< TCP 监听端口，不允许为零。
  std::uint16_t clan_port{0};  ///< Nx 战队子连接端口，必须与主端口不同。
  std::size_t io_threads{0};   ///< Asio I/O 工作线程数。
  std::size_t maximum_connections{0};        ///< 同时接受的连接上限。
  protocol::ProtocolLimits protocol_limits;  ///< 单连接包长与发送队列字节上限。
  std::uint16_t equipment_relay_port{27104U};  ///< 原版服饰序列独立连接端口。
  std::string advertised_address;              ///< 下发给客户端的
                                   ///< IPv4；省略时沿用监听地址，支持公网 NAT。
};

/** @brief 解析玩家连接地址，绝不将公网地址用于本机 bind。 */
[[nodiscard]] std::optional<std::array<std::uint8_t, 4U>> AdvertisedIpv4(
    const NetworkConfig& network);

/** @brief MySQL 连接池、数据库任务队列和超时边界。 */
struct DatabaseConfig final {
  mysql::ConnectionSettings connection;
  std::size_t pool_size{0};             ///< 专用数据库工作连接数。
  std::size_t task_queue_capacity{0};   ///< 等待数据库执行的用例任务上限。
  std::uint32_t acquire_timeout_ms{0};  ///< 获取池连接的最长等待毫秒数。
  std::uint32_t expected_schema_version{
      0};  ///< 运行服务允许的 MySQL 结构版本。
};

/** @brief 身份兼容开关；生产环境禁止离线测试身份。 */
struct SecurityConfig final {
  bool allow_offline_test_identity{
      false};  ///< 是否允许绕过正式启动器凭据的本地测试身份。
};

/** @brief 本机运维 HTTP 探针的监听和请求资源边界。 */
struct OperationsConfig final {
  std::string bind_address;  ///< 探针默认回环或管理网络地址。
  std::uint16_t port{0};     ///< HTTP `/live`、`/ready`、`/metrics` 监听端口。
  std::size_t maximum_connections{0};    ///< 同时读取的探针连接上限。
  std::size_t maximum_request_bytes{0};  ///< 单个无正文 HTTP 请求头上限。
};

/** @brief Optional authenticated STUN/TURN; default off preserves server-direct
 * deployment. */
struct IceConfig final {
  std::string bind_address, advertised_address, stun_host, turn_host,
      turn_secret_env;
  std::uint16_t port{0}, stun_port{3478}, turn_port{3478};
  std::uint16_t relay_port_begin{49152}, relay_port_end{50175};
  std::uint32_t max_allocations{1024}, credential_lifetime_seconds{3600};
  bool relay_only{false};
};

/** @brief 房间加载、倒计时、比赛和结果阶段的统一毫秒配置。 */
struct MatchTimingConfig final {
  std::uint32_t loading_timeout_ms{0};  ///< 全员加载完成前的最大等待时间。
  std::uint32_t countdown_ms{0};        ///< 加载完成到正式开局的倒计时。
  std::uint32_t match_duration_ms{0};   ///< 普通模式服务端时限。
  std::uint32_t result_transition_ms{
      0};  ///< 终局展示切换时间，不得超过结果阶段总时间。
  std::uint32_t result_duration_ms{0};  ///< 结果阶段到返回等待房间的总时间。
  bool use_room_duration{
      true};  ///< 使用房间选定时长；无有效选择时使用服务配置。
};

/** @brief 有界异步日志输出、轮转和控制台回显配置。 */
struct LoggingConfig final {
  std::filesystem::path path;  ///< 日志文件路径，不得指向玩家数据文件。
  application::LogLevel minimum_level{
      application::LogLevel::kInfo};  ///< 最低记录级别。
  std::size_t queue_capacity{0};      ///< 待写日志记录上限。
  std::uint64_t rotate_bytes{0};      ///< 单个日志文件达到该大小后轮转。
  std::size_t retained_files{0};      ///< 保留的历史轮转文件数量。
  bool echo_console{false};           ///< 是否同时输出到标准错误流。
};

/** @brief 有界停机等待策略。 */
struct ShutdownConfig final {
  std::uint32_t grace_timeout_ms{0};  ///< 排空关键任务的最长等待毫秒数。
};

/**
 * @brief 完整、经过语义校验后才能交给服务装配层的配置值。
 * @note 配置对象由进程入口按值拥有，运行组件只复制自身需要的不可变字段。
 */
/** @brief 独立认证与游戏入口的角色和固定大区配置；默认兼容原单进程部署。 */
struct ServiceConfig final {
  std::string role{"standalone"};    ///< standalone、login 或 game。
  std::uint32_t region_id{1U};       ///< 本入口允许签发或消费票据的大区。
  std::string game_address;          ///< 登录服务广告给客户端的非零 IPv4。
  std::uint16_t game_port{0U};       ///< 登录服务广告的游戏端口。
  std::uint16_t game_clan_port{0U};  ///< 登录服务广告的家族端口。
};

/** @brief 保留单机兼容配置，同时声明登录/游戏进程边界。 */
struct ServerConfig final {
  Environment environment{Environment::kDevelopment};  ///< 当前运行环境。
  NetworkConfig network;                               ///< 网络边界。
  DatabaseConfig database;                             ///< MySQL 边界。
  SecurityConfig security;                             ///< 身份安全边界。
  OperationsConfig operations;     ///< 本机健康、就绪和指标端点。
  MatchTimingConfig match_timing;  ///< 单实例房间阶段时限。
  std::filesystem::path
      hide_seek_asset_directory;  ///< Empty leaves hide-seek unavailable.
  bool hide_seek_development{
      false};  ///< Explicit isolated native acceptance only.
  runtime::UserHostPrizeRules
      user_host_prize;  ///< 显式启用本服赏金策略一；未知特殊卡分支不开放。
  runtime::WeaponMasterRules
      weapon_master;  ///< 本服武器大师规则；不冒充原服经验表。
  bool lobby_minigame_enabled{
      true};  ///< false 时不下发 xxLMG.dch，客户端显示准备中。
  runtime::LobbyMinigameRules
      lobby_minigame;       ///< 广场小游戏价格/概率/奖池；默认免费无物品。
  LoggingConfig logging;    ///< 日志资源边界。
  ShutdownConfig shutdown;  ///< 停机边界。
  ServiceConfig service;    ///< 进程角色和大区入口。
  std::filesystem::path
      life_attendance_journal_directory;  ///< 出席日志目录；相对配置文件解析，默认同目录life-attendance。
  IceConfig ice;  ///< Optional NAT traversal provider.
};

/** @brief 严格配置加载成功值或可安全展示的失败结果。 */
using ConfigurationResult =
    std::variant<ServerConfig, application::ServiceError>;

/**
 * @brief 从 UTF-8 key=value 文件加载并校验完整配置。
 * @param path 配置文件路径。
 * @return 成功时为 ServerConfig；失败时为不包含配置值的安全错误。
 * @note
 * 未知键、重复键、缺少键、无效数字和无效布尔值都会失败，避免拼写错误静默生效。
 */
[[nodiscard]] ConfigurationResult LoadServerConfig(
    const std::filesystem::path& path);

/**
 * @brief 校验由测试或其他受控来源构造的配置对象。
 * @param config 待校验配置。
 * @return 合法时为空字符串，否则返回不包含秘密值的中文原因。
 */
[[nodiscard]] std::string ValidateServerConfig(const ServerConfig& config);

/** @brief 将环境转换为稳定英文名称。 */
[[nodiscard]] std::string_view EnvironmentName(
    Environment environment) noexcept;

}  // namespace bf::infrastructure::configuration

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_SERVER_CONFIG_H_
