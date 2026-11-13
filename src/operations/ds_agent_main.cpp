#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <iostream>
#include <string_view>

#include "application/entry_points.h"
#include "operations/process/ds_agent.h"

int bf::entry::RunDsAgent(int argc, char** argv) {
  try {
    if (argc != 3 || std::string_view{argv[1]} != "--config") {
      std::cerr
          << "usage: BubbleFighter.Server.exe --mode agent --config <private-node-json>\n";
      return 2;
    }
    boost::asio::io_context io{1};
    bf::infrastructure::process::DsAgent agent{io, argv[2]};
    boost::asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&](auto error, int) {
      if (!error) agent.Stop();
    });
    agent.Start();
    std::cout << "{\"ready\":true,\"port\":" << agent.Port() << "}"
              << std::endl;
    io.run();
    return 0;
  } catch (const std::exception&) {
    std::cerr << "DS agent configuration or runtime failure\n";
    return 1;
  }
}
