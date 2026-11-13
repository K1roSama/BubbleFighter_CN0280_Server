#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "application/entry_points.h"
#include "network/room_ds_service.h"

int bf::entry::RunRoomDs(int argc, char** argv) {
  try {
    if (argc != 3 || std::string_view{argv[1]} != "--config") {
      std::cerr
          << "usage: BubbleFighter.Server.exe --mode room --config <private-node-json>\n";
      return 2;
    }
    const auto path = std::filesystem::absolute(argv[2]);
    std::ifstream file{path};
    auto config = nlohmann::json::parse(file);
    file.close();
    for (const auto* key : {"certificate", "private_key", "authority"})
      config[key] = std::filesystem::absolute(path.parent_path() /
                                              config.at(key).get<std::string>())
                        .string();
    bf::infrastructure::network::RoomDsOptions options;
    options.bind_address = config.at("bind_address").get<std::string>();
    options.port = 0U;
    options.tls = {config.at("certificate"), config.at("private_key"),
                   config.at("authority"), config.at("gateway_peer_name")};
    options.bind_timeout =
        std::chrono::milliseconds{config.value("bind_timeout_ms", 30000)};
    options.timing.loading_timeout =
        std::chrono::milliseconds{config.value("loading_timeout_ms", 90000)};
    options.timing.countdown =
        std::chrono::milliseconds{config.value("countdown_ms", 4000)};
    options.timing.match_duration =
        std::chrono::milliseconds{config.value("match_duration_ms", 240000)};
    options.timing.use_room_duration = config.value("use_room_duration", true);
    options.timing.result_transition =
        std::chrono::milliseconds{config.value("result_transition_ms", 2000)};
    options.timing.result_duration =
        std::chrono::milliseconds{config.value("result_duration_ms", 20000)};
    if (options.timing.loading_timeout <= std::chrono::milliseconds::zero() ||
        options.timing.countdown < std::chrono::milliseconds::zero() ||
        options.timing.match_duration <= std::chrono::milliseconds::zero() ||
        options.timing.result_transition < std::chrono::milliseconds::zero() ||
        options.timing.result_duration <= options.timing.result_transition)
      throw std::invalid_argument{"invalid room timing"};
    boost::asio::io_context io{1};
    bf::infrastructure::network::RoomDsService service{io, std::move(options)};
    std::cout
        << nlohmann::json{{"ready", true}, {"port", service.Port()}}.dump()
        << std::endl;
    boost::asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&](auto error, int) {
      if (!error) service.Stop();
    });
    service.Start();
    io.run();
    return 0;
  } catch (const std::exception&) {
    std::cerr << "room process configuration or runtime failure\n";
    return 1;
  }
}
