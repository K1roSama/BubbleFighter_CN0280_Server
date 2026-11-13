#include "features/configuration/server_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "operations/utf8_path.h"

namespace bf::infrastructure::configuration {
namespace {

constexpr std::array<std::string_view, 32> kRequiredKeys{
    "environment",
    "network.bind_address",
    "network.port",
    "network.clan_port",
    "network.io_threads",
    "network.max_connections",
    "network.max_packet_bytes",
    "network.max_send_queue_bytes",
    "database.host",
    "database.name",
    "database.user",
    "database.pool_size",
    "database.task_queue_capacity",
    "database.acquire_timeout_ms",
    "database.query_timeout_ms",
    "database.expected_schema_version",
    "security.allow_offline_test_identity",
    "operations.bind_address",
    "operations.port",
    "operations.max_connections",
    "operations.max_request_bytes",
    "match.loading_timeout_ms",
    "match.countdown_ms",
    "match.duration_ms",
    "match.result_transition_ms",
    "match.result_duration_ms",
    "logging.path",
    "logging.level",
    "logging.queue_capacity",
    "logging.rotate_bytes",
    "logging.retained_files",
    "logging.echo_console",
};

constexpr std::string_view kShutdownKey = "shutdown.grace_timeout_ms";

struct ParsedEntry final {
  std::string_view key;
  std::string_view value;
};

application::ServiceError ConfigurationError(std::string message) {
  return application::ServiceError{
      .code = application::ErrorCode::kInvalidInput,
      .safe_message = std::move(message),
      .correlation_id = {},
      .is_retryable = false,
  };
}

std::string_view Trim(std::string_view value) {
  const auto is_space = [](char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<bool> ParseBool(std::string_view value) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return std::nullopt;
}

// The old public keys used milliseconds, but native 67B160 compares whole
// seconds. Round up fractional seconds; never silently reinterpret 3000ms as
// 3000 seconds.
bool AssignFishingMilliseconds(std::string_view value, std::uint16_t& seconds) {
  const auto parsed = ParseUnsigned(value);
  if (!parsed ||
      *parsed >
          std::uint64_t{std::numeric_limits<std::uint16_t>::max()} * 1000U)
    return false;
  seconds = static_cast<std::uint16_t>((*parsed + 999U) / 1000U);
  return true;
}

std::string_view CanonicalFishingKey(std::string_view key) {
  if (key == "lobby_minigame.fishing_bite_min_ms")
    return "lobby_minigame.fishing_bite_min_seconds";
  if (key == "lobby_minigame.fishing_bite_max_ms")
    return "lobby_minigame.fishing_bite_max_seconds";
  if (key == "lobby_minigame.fishing_hook_window_ms")
    return "lobby_minigame.fishing_hook_window_seconds";
  return key;
}

std::optional<Environment> ParseEnvironment(std::string_view value) {
  if (value == "development") {
    return Environment::kDevelopment;
  }
  if (value == "test") {
    return Environment::kTest;
  }
  if (value == "production") {
    return Environment::kProduction;
  }
  return std::nullopt;
}

std::optional<application::LogLevel> ParseLogLevel(std::string_view value) {
  constexpr std::array<std::pair<std::string_view, application::LogLevel>, 6>
      levels{
          std::pair{std::string_view{"trace"}, application::LogLevel::kTrace},
          std::pair{std::string_view{"debug"}, application::LogLevel::kDebug},
          std::pair{std::string_view{"info"}, application::LogLevel::kInfo},
          std::pair{std::string_view{"warning"},
                    application::LogLevel::kWarning},
          std::pair{std::string_view{"error"}, application::LogLevel::kError},
          std::pair{std::string_view{"critical"},
                    application::LogLevel::kCritical},
      };
  const auto found =
      std::find_if(levels.begin(), levels.end(),
                   [value](const auto& entry) { return entry.first == value; });
  return found == levels.end()
             ? std::nullopt
             : std::optional<application::LogLevel>{found->second};
}

template <typename Target>
bool AssignUnsigned(std::string_view value, Target& target) {
  const auto parsed = ParseUnsigned(value);
  if (!parsed || *parsed > static_cast<std::uint64_t>(
                               std::numeric_limits<Target>::max())) {
    return false;
  }
  target = static_cast<Target>(*parsed);
  return true;
}

/** @brief `item:weight,item:weight`; an empty value clears the pool. */
bool ParseRewardPool(std::string_view value,
                     std::vector<runtime::LobbyMinigameReward>& pool) {
  pool.clear();
  auto remaining = Trim(value);
  while (!remaining.empty()) {
    const auto end = remaining.find(',');
    const auto entry = Trim(remaining.substr(0U, end));
    const auto colon = entry.find(':');
    runtime::LobbyMinigameReward reward;
    if (colon == std::string_view::npos ||
        !AssignUnsigned(Trim(entry.substr(0U, colon)), reward.item_id) ||
        !AssignUnsigned(Trim(entry.substr(colon + 1U)), reward.weight) ||
        reward.item_id == 0U || reward.weight == 0U)
      return false;
    pool.push_back(reward);
    if (end == std::string_view::npos) break;
    remaining.remove_prefix(end + 1U);
  }
  return true;
}

bool AssignValue(ServerConfig& config, const ParsedEntry& entry) {
  const auto key = entry.key;
  const auto value = entry.value;
  if (key == "environment") {
    const auto parsed = ParseEnvironment(value);
    if (!parsed) {
      return false;
    }
    config.environment = *parsed;
  } else if (key == "network.bind_address") {
    config.network.bind_address = value;
  } else if (key == "network.advertised_address") {
    config.network.advertised_address = value;
  } else if (key == "network.port") {
    return AssignUnsigned(value, config.network.port);
  } else if (key == "network.equipment_relay_port") {
    return AssignUnsigned(value, config.network.equipment_relay_port);
  } else if (key == "network.clan_port") {
    return AssignUnsigned(value, config.network.clan_port);
  } else if (key == "network.io_threads") {
    return AssignUnsigned(value, config.network.io_threads);
  } else if (key == "network.max_connections") {
    return AssignUnsigned(value, config.network.maximum_connections);
  } else if (key == "network.max_packet_bytes") {
    return AssignUnsigned(value,
                          config.network.protocol_limits.maximum_packet_bytes);
  } else if (key == "network.max_send_queue_bytes") {
    return AssignUnsigned(
        value, config.network.protocol_limits.maximum_send_queue_bytes);
  } else if (key == "database.host") {
    config.database.connection.host = value;
  } else if (key == "database.port") {
    return AssignUnsigned(value, config.database.connection.port);
  } else if (key == "database.name") {
    config.database.connection.name = value;
  } else if (key == "database.user") {
    config.database.connection.user = value;
  } else if (key == "database.password_env") {
    config.database.connection.password_environment = value;
  } else if (key == "database.password_file") {
    config.database.connection.password_file =
        bf::infrastructure::Utf8Path(value);
  } else if (key == "database.tls") {
    config.database.connection.tls = value;
  } else if (key == "database.ca_file") {
    config.database.connection.ca_file = bf::infrastructure::Utf8Path(value);
  } else if (key == "database.pool_size") {
    return AssignUnsigned(value, config.database.pool_size);
  } else if (key == "database.task_queue_capacity") {
    return AssignUnsigned(value, config.database.task_queue_capacity);
  } else if (key == "database.acquire_timeout_ms") {
    return AssignUnsigned(value, config.database.acquire_timeout_ms);
  } else if (key == "database.query_timeout_ms") {
    return AssignUnsigned(value, config.database.connection.query_timeout_ms);
  } else if (key == "database.expected_schema_version") {
    return AssignUnsigned(value, config.database.expected_schema_version);
  } else if (key == "security.allow_offline_test_identity") {
    const auto parsed = ParseBool(value);
    if (!parsed) {
      return false;
    }
    config.security.allow_offline_test_identity = *parsed;
  } else if (key == "operations.bind_address") {
    config.operations.bind_address = value;
  } else if (key == "operations.port") {
    return AssignUnsigned(value, config.operations.port);
  } else if (key == "operations.max_connections") {
    return AssignUnsigned(value, config.operations.maximum_connections);
  } else if (key == "operations.max_request_bytes") {
    return AssignUnsigned(value, config.operations.maximum_request_bytes);
  } else if (key == "ice.bind_address") {
    config.ice.bind_address = value;
  } else if (key == "ice.advertised_address") {
    config.ice.advertised_address = value;
  } else if (key == "ice.stun_host") {
    config.ice.stun_host = value;
  } else if (key == "ice.turn_host") {
    config.ice.turn_host = value;
  } else if (key == "ice.turn_secret_env") {
    config.ice.turn_secret_env = value;
  } else if (key == "ice.port") {
    return AssignUnsigned(value, config.ice.port);
  } else if (key == "ice.stun_port") {
    return AssignUnsigned(value, config.ice.stun_port);
  } else if (key == "ice.turn_port") {
    return AssignUnsigned(value, config.ice.turn_port);
  } else if (key == "ice.relay_port_begin") {
    return AssignUnsigned(value, config.ice.relay_port_begin);
  } else if (key == "ice.relay_port_end") {
    return AssignUnsigned(value, config.ice.relay_port_end);
  } else if (key == "ice.max_allocations") {
    return AssignUnsigned(value, config.ice.max_allocations);
  } else if (key == "ice.credential_lifetime_seconds") {
    return AssignUnsigned(value, config.ice.credential_lifetime_seconds);
  } else if (key == "ice.relay_only") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.ice.relay_only = *parsed;
  } else if (key == "hide_seek.asset_directory") {
    config.hide_seek_asset_directory = std::string(value);
  } else if (key == "hide_seek.development") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.hide_seek_development = *parsed;
  } else if (key == "match.loading_timeout_ms") {
    return AssignUnsigned(value, config.match_timing.loading_timeout_ms);
  } else if (key == "match.countdown_ms") {
    return AssignUnsigned(value, config.match_timing.countdown_ms);
  } else if (key == "match.duration_ms") {
    return AssignUnsigned(value, config.match_timing.match_duration_ms);
  } else if (key == "match.use_room_duration") {
    const auto parsed = ParseBool(value);
    if (!parsed) {
      return false;
    }
    config.match_timing.use_room_duration = *parsed;
  } else if (key == "match.result_transition_ms") {
    return AssignUnsigned(value, config.match_timing.result_transition_ms);
  } else if (key == "match.result_duration_ms") {
    return AssignUnsigned(value, config.match_timing.result_duration_ms);
  } else if (key == "user_host.enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.user_host_prize.enabled = *parsed;
  } else if (key == "user_host.minimum_amount") {
    return AssignUnsigned(value, config.user_host_prize.minimum_amount);
  } else if (key == "user_host.maximum_amount") {
    return AssignUnsigned(value, config.user_host_prize.maximum_amount);
  } else if (key == "weapon_master.final_level") {
    return AssignUnsigned(value, config.weapon_master.final_level);
  } else if (key == "weapon_master.level_step") {
    return AssignUnsigned(value, config.weapon_master.level_step);
  } else if (key == "weapon_master.pang_experience") {
    return AssignUnsigned(value, config.weapon_master.pang_experience);
  } else if (key == "weapon_master.assist_experience") {
    return AssignUnsigned(value, config.weapon_master.assist_experience);
  } else if (key == "weapon_master.primary_pool") {
    config.weapon_master.primary_pool.clear();
    auto remaining = value;
    for (;;) {
      const auto end = remaining.find(',');
      std::uint32_t weapon{0U};
      if (!AssignUnsigned(Trim(remaining.substr(0U, end)), weapon))
        return false;
      config.weapon_master.primary_pool.push_back(weapon);
      if (end == std::string_view::npos) break;
      remaining.remove_prefix(end + 1U);
    }
  } else if (key == "lobby_minigame.enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.lobby_minigame_enabled = *parsed;
  } else if (key == "lobby_minigame.basketball_enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.lobby_minigame.config.basketball.enabled = *parsed;
  } else if (key == "lobby_minigame.basketball_cost") {
    return AssignUnsigned(value,
                          config.lobby_minigame.config.basketball.cost_lucci);
  } else if (key == "lobby_minigame.basketball_gauge_lead_ms") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.basketball.gauge_lead_ms);
  } else if (key == "lobby_minigame.basketball_hit_window_ms") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.basketball.hit_window_ms);
  } else if (key == "lobby_minigame.basketball_round_end_ms") {
    return AssignUnsigned(value,
                          config.lobby_minigame.config.basketball.round_end_ms);
  } else if (key == "lobby_minigame.clean_shot_percent") {
    return AssignUnsigned(value, config.lobby_minigame.clean_shot_percent);
  } else if (key == "lobby_minigame.clean_shot_rewards") {
    return ParseRewardPool(value, config.lobby_minigame.clean_shot_rewards);
  } else if (key == "lobby_minigame.fishing_enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.lobby_minigame.config.fishing.enabled = *parsed;
  } else if (key == "lobby_minigame.fishing_cost") {
    return AssignUnsigned(value,
                          config.lobby_minigame.config.fishing.cost_lucci);
  } else if (key == "lobby_minigame.fishing_alternate_cost") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.fishing.alternate_cost_lucci);
  } else if (key == "lobby_minigame.fishing_premium_rod_item") {
    auto& item = config.lobby_minigame.config.fishing.premium_rod_item;
    return AssignUnsigned(value, item) && (item == 0U || item == 190083U);
  } else if (key == "lobby_minigame.fishing_free_pass_item") {
    auto& item = config.lobby_minigame.config.fishing.free_pass_item;
    return AssignUnsigned(value, item) && (item == 0U || item == 101606U);
  } else if (key == "lobby_minigame.fishing_cast_ms") {
    return AssignUnsigned(value, config.lobby_minigame.config.fishing.cast_ms);
  } else if (key == "lobby_minigame.fishing_bite_min_ms") {
    return AssignFishingMilliseconds(
        value, config.lobby_minigame.config.fishing.bite_min_seconds);
  } else if (key == "lobby_minigame.fishing_bite_max_ms") {
    return AssignFishingMilliseconds(
        value, config.lobby_minigame.config.fishing.bite_max_seconds);
  } else if (key == "lobby_minigame.fishing_hook_window_ms") {
    return AssignFishingMilliseconds(
        value, config.lobby_minigame.config.fishing.hook_window_seconds);
  } else if (key == "lobby_minigame.fishing_bite_min_seconds") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.fishing.bite_min_seconds);
  } else if (key == "lobby_minigame.fishing_bite_max_seconds") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.fishing.bite_max_seconds);
  } else if (key == "lobby_minigame.fishing_hook_window_seconds") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.fishing.hook_window_seconds);
  } else if (key == "lobby_minigame.fishing_daily_limit") {
    return AssignUnsigned(value, config.lobby_minigame.fishing_daily_limit);
  } else if (key == "lobby_minigame.fishing_catch_percent") {
    return AssignUnsigned(value, config.lobby_minigame.fishing_catch_percent);
  } else if (key == "lobby_minigame.fishing_rewards") {
    return ParseRewardPool(value, config.lobby_minigame.fishing_rewards);
  } else if (key == "lobby_minigame.fortune_enabled") {
    const auto parsed = ParseBool(value);
    if (!parsed) return false;
    config.lobby_minigame.config.fortune.enabled = *parsed;
  } else if (key == "lobby_minigame.fortune_cost") {
    return AssignUnsigned(
        value, config.lobby_minigame.config.fortune.daily_cost_lucci);
  } else if (key == "lobby_minigame.fortune_lucky_items") {
    return ParseRewardPool(value, config.lobby_minigame.fortune_lucky_items);
  } else if (key == "life_attendance.journal_directory") {
    if (value.empty()) return false;
    config.life_attendance_journal_directory = std::filesystem::path{value};
  } else if (key == "logging.path") {
    config.logging.path = std::filesystem::path{value};
  } else if (key == "logging.level") {
    const auto parsed = ParseLogLevel(value);
    if (!parsed) {
      return false;
    }
    config.logging.minimum_level = *parsed;
  } else if (key == "logging.queue_capacity") {
    return AssignUnsigned(value, config.logging.queue_capacity);
  } else if (key == "logging.rotate_bytes") {
    return AssignUnsigned(value, config.logging.rotate_bytes);
  } else if (key == "logging.retained_files") {
    return AssignUnsigned(value, config.logging.retained_files);
  } else if (key == "logging.echo_console") {
    const auto parsed = ParseBool(value);
    if (!parsed) {
      return false;
    }
    config.logging.echo_console = *parsed;
  } else if (key == kShutdownKey) {
    return AssignUnsigned(value, config.shutdown.grace_timeout_ms);
  } else if (key == "service.role") {
    config.service.role = value;
  } else if (key == "service.region_id") {
    return AssignUnsigned(value, config.service.region_id);
  } else if (key == "service.game_address") {
    config.service.game_address = value;
  } else if (key == "service.game_port") {
    return AssignUnsigned(value, config.service.game_port);
  } else if (key == "service.game_clan_port") {
    return AssignUnsigned(value, config.service.game_clan_port);
  } else {
    return false;
  }
  return true;
}

bool IsKnownKey(std::string_view key) {
  constexpr std::array<std::string_view, 13> ice_keys{
      "ice.bind_address",     "ice.advertised_address",
      "ice.stun_host",        "ice.turn_host",
      "ice.turn_secret_env",  "ice.port",
      "ice.stun_port",        "ice.turn_port",
      "ice.relay_port_begin", "ice.relay_port_end",
      "ice.max_allocations",  "ice.credential_lifetime_seconds",
      "ice.relay_only"};
  if (std::find(ice_keys.begin(), ice_keys.end(), key) != ice_keys.end())
    return true;
  return key == "database.port" || key == "database.password_env" ||
         key == "database.password_file" || key == "database.tls" ||
         key == "database.ca_file" || key == "hide_seek.asset_directory" ||
         key == "hide_seek.development" ||
         key == "life_attendance.journal_directory" ||
         key == "network.equipment_relay_port" ||
         key == "network.advertised_address" || key == kShutdownKey ||
         key == "match.use_room_duration" || key == "service.role" ||
         key == "user_host.enabled" || key == "user_host.minimum_amount" ||
         key == "user_host.maximum_amount" ||
         key == "weapon_master.final_level" ||
         key == "weapon_master.level_step" ||
         key == "weapon_master.pang_experience" ||
         key == "weapon_master.assist_experience" ||
         key == "weapon_master.primary_pool" || key == "service.region_id" ||
         key == "service.game_address" || key == "service.game_port" ||
         key == "service.game_clan_port" ||
         key.starts_with("lobby_minigame.") ||
         std::find(kRequiredKeys.begin(), kRequiredKeys.end(), key) !=
             kRequiredKeys.end();
}

}  // namespace

std::optional<std::array<std::uint8_t, 4U>> AdvertisedIpv4(
    const NetworkConfig& network) {
  std::string_view text = network.advertised_address.empty()
                              ? network.bind_address
                              : network.advertised_address;
  std::array<std::uint8_t, 4U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    const auto dot = text.find('.');
    if ((index < 3U) != (dot != std::string_view::npos)) return std::nullopt;
    const auto value = ParseUnsigned(text.substr(0U, dot));
    if (!value || *value > 255U) return std::nullopt;
    result[index] = static_cast<std::uint8_t>(*value);
    if (dot != std::string_view::npos) text.remove_prefix(dot + 1U);
  }
  if (result[0] == 0U || result[0] >= 224U) return std::nullopt;
  return result;
}

ConfigurationResult LoadServerConfig(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return ConfigurationError("Unable to open the server configuration file");
  }

  ServerConfig config;
  std::unordered_set<std::string> seen_keys;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    const auto separator = trimmed.find('=');
    if (separator == std::string_view::npos) {
      return ConfigurationError("Configuration line " +
                                std::to_string(line_number) + ": missing '='");
    }
    const auto key = Trim(trimmed.substr(0, separator));
    const auto value = Trim(trimmed.substr(separator + 1));
    if (key.empty() || !IsKnownKey(key)) {
      return ConfigurationError("Configuration line " +
                                std::to_string(line_number) +
                                ": unknown option");
    }
    if (!seen_keys.emplace(CanonicalFishingKey(key)).second) {
      return ConfigurationError("Configuration line " +
                                std::to_string(line_number) +
                                ": duplicate option");
    }
    if (!AssignValue(config, ParsedEntry{.key = key, .value = value})) {
      return ConfigurationError("Configuration line " +
                                std::to_string(line_number) +
                                ": invalid value");
    }
  }
  if (!input.eof()) {
    return ConfigurationError("Failed to read the server configuration file");
  }

  for (const auto key : kRequiredKeys) {
    if (seen_keys.find(std::string{key}) == seen_keys.end()) {
      return ConfigurationError("Required configuration option is missing: " +
                                std::string{key});
    }
  }
  if (seen_keys.find(std::string{kShutdownKey}) == seen_keys.end()) {
    return ConfigurationError("Required configuration option is missing: " +
                              std::string{kShutdownKey});
  }

  if (!config.database.connection.password_file.empty() &&
      config.database.connection.password_file.is_relative()) {
    config.database.connection.password_file =
        std::filesystem::absolute(path).parent_path() /
        config.database.connection.password_file;
  }
  if (const auto reason = ValidateServerConfig(config); !reason.empty()) {
    return ConfigurationError(reason);
  }
  return config;
}

std::string ValidateServerConfig(const ServerConfig& config) {
  if (config.ice.port && config.ice.bind_address.empty())
    return "ice.port requires ice.bind_address";
  if (config.ice.relay_port_begin < 1024U ||
      config.ice.relay_port_end < config.ice.relay_port_begin)
    return "invalid ICE relay port range";
  if (!config.ice.max_allocations || config.ice.max_allocations > 16384U ||
      config.ice.credential_lifetime_seconds < 600U ||
      config.ice.credential_lifetime_seconds > 86400U)
    return "invalid ICE allocation limit or credential lifetime";
  if (!config.ice.turn_host.empty() && !config.ice.port &&
      config.ice.turn_secret_env.empty())
    return "external TURN requires a shared secret environment variable";
  if (config.user_host_prize.minimum_amount == 0U ||
      config.user_host_prize.minimum_amount >
          config.user_host_prize.maximum_amount ||
      config.user_host_prize.maximum_amount > 2147483647U)
    return "user_host amount bounds must be positive, ordered and within the native signed integer range";
  try {
    const runtime::WeaponMasterMatch rules{config.weapon_master, 0U};
  } catch (const std::invalid_argument&) {
    return "Invalid weapon_master experience rules or verified primary weapon pool";
  }
  const auto& minigame = config.lobby_minigame;
  if (minigame.clean_shot_percent > 100U ||
      minigame.fishing_catch_percent > 100U ||
      minigame.config.fishing.bite_min_seconds >
          minigame.config.fishing.bite_max_seconds ||
      (minigame.config.fishing.enabled &&
       minigame.config.fishing.cast_ms == 0U) ||
      minigame.config.basketball.gauge_lead_ms >
          minigame.config.basketball.round_end_ms) {
    return "lobby_minigame probabilities must be in "
           "0..100; fishing bite bounds must be ordered, enabled fishing timers must be positive, and basketball charge time must not exceed the round duration";
  }
  if (config.service.role != "standalone" && config.service.role != "login" &&
      config.service.role != "game") {
    return "service.role must be standalone, login or game";
  }
  if (config.service.region_id == 0U ||
      config.service.region_id > 2147483647U) {
    return "service.region_id must be a positive signed 32-bit integer";
  }
  if (config.service.role != "standalone" &&
      config.security.allow_offline_test_identity) {
    return "Offline test identities are not permitted in split deployments";
  }
  if (config.service.role == "login") {
    auto address = std::string_view{config.service.game_address};
    std::array<std::uint32_t, 4U> octets{};
    for (std::size_t index = 0; index < octets.size(); ++index) {
      const auto dot = address.find('.');
      const auto part = address.substr(0, dot);
      const auto parsed = ParseUnsigned(part);
      if (!parsed || *parsed > 255U ||
          (part.size() > 1U && part.front() == '0') ||
          (index < 3U && dot == std::string_view::npos) ||
          (index == 3U && dot != std::string_view::npos)) {
        return "service.game_address must be a client-accessible IPv4 address";
      }
      octets[index] = static_cast<std::uint32_t>(*parsed);
      if (dot != std::string_view::npos) address.remove_prefix(dot + 1U);
    }
    if (octets[0] == 0U || octets[0] >= 224U ||
        config.service.game_port == 0U || config.service.game_clan_port == 0U ||
        config.service.game_port == config.service.game_clan_port) {
      return "The login service requires valid, distinct game and clan ports and a unicast IPv4 address";
    }
    if (config.service.game_address == config.network.bind_address &&
        (config.service.game_port == config.network.port ||
         config.service.game_clan_port == config.network.port)) {
      return "The login port cannot be used as a game or clan destination port";
    }
  }
  if (config.network.bind_address.empty() ||
      std::any_of(config.network.bind_address.begin(),
                  config.network.bind_address.end(),
                  [](unsigned char character) { return character <= 0x20U; })) {
    return "network.bind_address must not be empty or contain whitespace or control characters";
  }
  if (config.network.port == 0U) {
    return "network.port must not be zero";
  }
  if (!config.network.advertised_address.empty() &&
      !AdvertisedIpv4(config.network)) {
    return "network.advertised_address must be a client-accessible numeric unicast IPv4 address";
  }
  if (config.network.clan_port == 0U ||
      config.network.clan_port == config.network.port) {
    return "network.clan_port must be nonzero and differ from network.port";
  }
  if (config.network.equipment_relay_port == 0U ||
      config.network.equipment_relay_port == config.network.port ||
      config.network.equipment_relay_port == config.network.clan_port ||
      config.network.equipment_relay_port == config.operations.port)
    return "network.equipment_relay_port must be nonzero and not conflict with another listener";
  if (config.operations.bind_address != "127.0.0.1" &&
      config.operations.bind_address != "::1") {
    return "operations.bind_address must be the loopback address 127.0.0.1 or ::1";
  }
  if (config.operations.port == 0U ||
      config.operations.port == config.network.port ||
      config.operations.port == config.network.clan_port) {
    return "operations.port must be nonzero and differ from the game and clan ports";
  }
  if (config.operations.maximum_connections == 0U ||
      config.operations.maximum_connections > 1024U) {
    return "operations.max_connections must be between 1 and 1024";
  }
  if (config.operations.maximum_request_bytes < 512U ||
      config.operations.maximum_request_bytes > 65536U) {
    return "operations.max_request_bytes must be between 512 and 65536";
  }
  if (config.network.io_threads == 0U || config.network.io_threads > 64U) {
    return "network.io_threads must be between 1 and 64";
  }
  if (config.network.maximum_connections == 0U ||
      config.network.maximum_connections > 100000U) {
    return "network.max_connections is outside the allowed range";
  }
  if (const auto error =
          protocol::ValidateProtocolLimits(config.network.protocol_limits)) {
    return error->safe_message;
  }
  const auto& database = config.database.connection;
  if (database.host.empty() || database.user.empty() || database.name.empty() ||
      database.port == 0U) {
    return "MySQL connections require host, port, name and user";
  }
  if (!database.password_environment.empty() &&
      !database.password_file.empty()) {
    return "database.password_env and database.password_file are mutually exclusive";
  }
  const bool loopback_db = database.host == "localhost" ||
                           database.host == "127.0.0.1" ||
                           database.host == "::1";
  if (!((database.tls == "required" && loopback_db) ||
        (database.tls == "verify_identity" && !database.ca_file.empty()))) {
    return "MySQL loopback connections require TLS; remote connections require verify_identity and a CA file";
  }
  if (config.database.pool_size == 0U || config.database.pool_size > 64U) {
    return "database.pool_size must be between 1 and 64";
  }
  if (config.database.task_queue_capacity < config.database.pool_size ||
      config.database.task_queue_capacity > 1000000U) {
    return "database.task_queue_capacity must be at least the pool size and not exceed the global limit";
  }
  if (config.database.acquire_timeout_ms == 0U ||
      config.database.acquire_timeout_ms > 600000U ||
      config.database.connection.query_timeout_ms == 0U ||
      config.database.connection.query_timeout_ms > 600000U) {
    return "Database timeouts must be between 1 and 600000 milliseconds";
  }
  if (config.logging.path.empty()) {
    return "logging.path must not be empty";
  }
  constexpr std::uint32_t maximum_match_phase_ms = 3'600'000U;
  if (config.match_timing.loading_timeout_ms == 0U ||
      config.match_timing.loading_timeout_ms > maximum_match_phase_ms ||
      config.match_timing.countdown_ms > maximum_match_phase_ms ||
      config.match_timing.match_duration_ms == 0U ||
      config.match_timing.match_duration_ms > maximum_match_phase_ms ||
      config.match_timing.result_transition_ms > maximum_match_phase_ms ||
      config.match_timing.result_duration_ms == 0U ||
      config.match_timing.result_duration_ms > maximum_match_phase_ms ||
      config.match_timing.result_transition_ms >
          config.match_timing.result_duration_ms) {
    return "Match phase durations must not exceed one hour; the result transition must not exceed the result phase duration";
  }
  if (config.logging.queue_capacity == 0U ||
      config.logging.queue_capacity > 1000000U) {
    return "logging.queue_capacity is outside the allowed range";
  }
  if (config.logging.rotate_bytes < 4096U ||
      config.logging.rotate_bytes > (4ULL << 30U)) {
    return "logging.rotate_bytes must be between 4096 bytes and 4 GiB";
  }
  if (config.logging.retained_files == 0U ||
      config.logging.retained_files > 100U) {
    return "logging.retained_files must be between 1 and 100";
  }
  if (config.shutdown.grace_timeout_ms == 0U ||
      config.shutdown.grace_timeout_ms > 600000U) {
    return "shutdown.grace_timeout_ms must be between 1 and 600000 milliseconds";
  }
  if (config.environment == Environment::kProduction &&
      config.security.allow_offline_test_identity) {
    return "Offline test identities are not permitted in the production environment";
  }
  return {};
}

std::string_view EnvironmentName(Environment environment) noexcept {
  switch (environment) {
    case Environment::kDevelopment:
      return "development";
    case Environment::kTest:
      return "test";
    case Environment::kProduction:
      return "production";
  }
  return "unknown";
}

}  // namespace bf::infrastructure::configuration
