#ifndef BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_MYSQL_SETTINGS_H_
#define BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_MYSQL_SETTINGS_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace bf::infrastructure::mysql {
struct ConnectionSettings {
  std::string host{"127.0.0.1"};
  std::uint16_t port{3306};
  std::string name;
  std::string user;
  std::string password_environment;
  std::filesystem::path password_file;
  std::string tls{"required"};
  std::filesystem::path ca_file;
  std::uint32_t query_timeout_ms{3000};
};

}  // namespace bf::infrastructure::mysql

#endif  // BUBBLEFIGHTER_SERVER_FEATURES_CONFIGURATION_MYSQL_SETTINGS_H_
