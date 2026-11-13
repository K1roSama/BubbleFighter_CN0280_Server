#include "network/ice_network_service.h"

#include <juice/juice.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <utility>

namespace bf::infrastructure::network {
namespace {
std::uint64_t SecondsNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string Encode(std::span<const unsigned char> bytes) {
  std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const auto length =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                      bytes.data(), static_cast<int>(bytes.size()));
  if (length < 0) throw std::runtime_error("TURN credential encoding failed");
  out.resize(static_cast<std::size_t>(length));
  return out;
}
}  // namespace

class IceNetworkService::Impl {
 public:
  configuration::IceConfig config;
  juice_server_t* server = nullptr;
  std::string secret;
  std::map<domain::PlayerId, application::IceLease> cached;

  explicit Impl(configuration::IceConfig value) : config(std::move(value)) {
    juice_set_log_level(
        JUICE_LOG_LEVEL_NONE);  // SDP and credentials must never enter logs.
    if (!config.turn_secret_env.empty()) {
#ifdef _MSC_VER
      char* data = nullptr;
      std::size_t length = 0;
      if (_dupenv_s(&data, &length, config.turn_secret_env.c_str()) || !data)
        throw std::runtime_error(
            "TURN shared secret environment variable is missing");
      secret.assign(data, length ? length - 1 : 0);
      std::free(data);
#else
      const char* data = std::getenv(config.turn_secret_env.c_str());
      if (!data)
        throw std::runtime_error(
            "TURN shared secret environment variable is missing");
      secret = data;
#endif
      if (secret.size() < 32 || secret.size() > 512)
        throw std::runtime_error(
            "TURN shared secret must contain 32..512 bytes");
    }
    if (config.port) {
      juice_server_config_t options{};
      options.bind_address = config.bind_address.c_str();
      options.external_address = config.advertised_address.empty()
                                     ? nullptr
                                     : config.advertised_address.c_str();
      options.port = config.port;
      options.realm = "BubbleFighter";
      options.max_allocations = static_cast<int>(config.max_allocations);
      options.max_peers = 64;
      options.relay_port_range_begin = config.relay_port_begin;
      options.relay_port_range_end = config.relay_port_end;
      server = juice_server_create(&options);
      if (!server)
        throw std::runtime_error("authenticated ICE server could not start");
      if (config.stun_host.empty()) {
        config.stun_host = config.advertised_address.empty()
                               ? config.bind_address
                               : config.advertised_address;
        config.stun_port = juice_server_get_port(server);
      }
      if (config.turn_host.empty()) {
        config.turn_host = config.stun_host;
        config.turn_port = juice_server_get_port(server);
      }
    }
  }

  ~Impl() {
    if (server) juice_server_destroy(server);
  }

  std::optional<application::IceLease> Issue(domain::PlayerId player) {
    if (!player.IsValid() ||
        (config.stun_host.empty() && config.turn_host.empty()))
      return std::nullopt;
    const auto now = SecondsNow();
    const auto found = cached.find(player);
    if (found != cached.end() && found->second.expires_at > now + 300U)
      return found->second;
    if (cached.size() >= 16384) {
      std::erase_if(cached, [now](const auto& row) {
        return row.second.expires_at <= now;
      });
      if (cached.size() >= 16384) return std::nullopt;
    }
    application::IceLease lease{
        .stun_host = config.stun_host,
        .turn_host = config.turn_host,
        .username = {},
        .password = {},
        .stun_port = config.stun_port,
        .turn_port = config.turn_port,
        .expires_at = now + config.credential_lifetime_seconds,
        .relay_only = config.relay_only};
    if (!lease.turn_host.empty()) {
      lease.username = std::to_string(lease.expires_at) + ":" +
                       std::to_string(player.Value());
      if (!secret.empty()) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        std::size_t size = 0;
        if (!EVP_Q_mac(
                nullptr, "HMAC", nullptr, "SHA1", nullptr, secret.data(),
                secret.size(),
                reinterpret_cast<const unsigned char*>(lease.username.data()),
                lease.username.size(), digest.data(), digest.size(), &size))
          throw std::runtime_error("TURN credential generation failed");
        lease.password = Encode({digest.data(), size});
      } else {
        std::random_device random;
        std::array<unsigned char, 32> bytes{};
        for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
        lease.password = Encode(bytes);
      }
      if (server) {
        juice_server_credentials_t credential{};
        credential.username = lease.username.c_str();
        credential.password = lease.password.c_str();
        credential.allocations_quota = 64;
        if (juice_server_add_credentials(
                server, &credential,
                static_cast<unsigned long>(config.credential_lifetime_seconds) *
                    1000UL) != 0)
          return std::nullopt;
      } else if (secret.empty())
        return std::nullopt;
    }
    cached[player] = lease;
    return lease;
  }
};

IceNetworkService::IceNetworkService(configuration::IceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

IceNetworkService::~IceNetworkService() = default;

std::optional<application::IceLease> IceNetworkService::Issue(
    domain::PlayerId player) {
  return impl_->Issue(player);
}
}  // namespace bf::infrastructure::network
