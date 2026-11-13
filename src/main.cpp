#include <windows.h>

#include <conio.h>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "application/entry_points.h"
#include "operations/async_file_logger.h"

namespace {

void PrintUsage() {
  std::cout
      << "BubbleFighter MySQL server\n"
         "Double-click: load config/server.conf beside BubbleFighter.Server.exe.\n"
         "Usage: BubbleFighter.Server.exe [--mode <mode>] <options>\n"
         "  combined  [--config <server.conf>] [--login-config <login.conf>] (default)\n"
         "  game      [--config <game.conf>] [--room-ds-config <gateway.json>]\n"
         "  login     --config <login.conf>\n"
         "  migrate   --config <database.conf> --migrations <directory>\n"
         "  admin     <command> [--config <database.conf>]\n"
         "  room      --config <node.json>\n"
         "  agent     --config <node.json>\n"
         "Service options: --check-config, --check-database, --run-for-ms <0..3600000>\n";
}

std::filesystem::path ExecutableDirectory() {
  std::array<wchar_t, 32768U> path{};
  const auto length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size())
    throw std::runtime_error{"Cannot locate the server executable"};
  return std::filesystem::path{std::wstring_view{path.data(), length}}
      .parent_path();
}

int Run(int argc, wchar_t** wide_argv) {
  if (argc == 2 && std::wstring_view{wide_argv[1]} == L"--help") {
    PrintUsage();
    return 0;
  }
  std::wstring_view mode = L"combined";
  int first_argument = 1;
  if (argc > 1 && std::wstring_view{wide_argv[1]} == L"--mode") {
    if (argc < 3) {
      PrintUsage();
      return 2;
    }
    mode = wide_argv[2];
    first_argument = 3;
  }
  if (mode != L"combined" && mode != L"game" && mode != L"login" &&
      mode != L"migrate" && mode != L"admin" && mode != L"room" &&
      mode != L"agent") {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Arguments] Unknown server mode. Use --help to list available modes.");
    return 2;
  }

  std::vector<std::wstring> wide_values{wide_argv[0]};
  for (int i = first_argument; i < argc; ++i)
    wide_values.emplace_back(wide_argv[i]);
  if (wide_values.size() == 2U && wide_values[1] == L"--help" &&
      mode != L"admin") {
    PrintUsage();
    return 0;
  }
  const auto has_option = [&](std::wstring_view option) {
    for (std::size_t i = 1U; i < wide_values.size(); ++i)
      if (wide_values[i] == option) return true;
    return false;
  };
  if ((mode == L"combined" || mode == L"game") && !has_option(L"--config")) {
    std::filesystem::current_path(ExecutableDirectory());
    wide_values.emplace_back(L"--config");
    wide_values.emplace_back(L"config/server.conf");
    if (!has_option(L"--login-config") &&
        std::filesystem::is_regular_file("config/login.conf")) {
      wide_values.emplace_back(L"--login-config");
      wide_values.emplace_back(L"config/login.conf");
    }
  }
  std::vector<wchar_t*> wide_arguments;
  for (auto& value : wide_values) wide_arguments.push_back(value.data());
  const int count = static_cast<int>(wide_arguments.size());
  wide_arguments.push_back(nullptr);
  if (mode == L"migrate")
    return bf::entry::RunMigrations(count, wide_arguments.data());

  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_arguments[i],
                            -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return 2;
    std::string value(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_arguments[i],
                             -1, value.data(), length, nullptr, nullptr))
      return 2;
    value.pop_back();
    arguments.push_back(std::move(value));
  }
  std::vector<char*> pointers;
  for (auto& argument : arguments) pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  if (mode == L"admin") return bf::entry::RunAdmin(count, pointers.data());
  if (mode == L"login") return bf::entry::RunLogin(count, pointers.data());
  if (mode == L"room") return bf::entry::RunRoomDs(count, pointers.data());
  if (mode == L"agent") return bf::entry::RunDsAgent(count, pointers.data());
  return bf::entry::RunGame(count, pointers.data());
}

void KeepStartupFailureVisible(int argc, int result) {
  if (argc != 1 || result == 0) return;
  std::array<DWORD, 2U> processes{};
  DWORD input_mode{};
  if (GetConsoleProcessList(processes.data(),
                            static_cast<DWORD>(processes.size())) != 1 ||
      !GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode))
    return;
  bf::infrastructure::logging::WriteConsoleLog(
      bf::application::LogLevel::kWarning,
      "[Startup] Check config/server.conf, config/mysql.password and the server logs. Press any key to close this window.");
  static_cast<void>(_getwch());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {  // NOLINT(bugprone-exception-escape)
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleTitleW(L"BubbleFighter Server");
  int result = 1;
  try {
    result = Run(argc, argv);
  } catch (const std::exception&) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Startup] The server command failed. Check the configuration and runtime environment.");
  } catch (...) {
    bf::infrastructure::logging::WriteConsoleLog(
        bf::application::LogLevel::kError,
        "[Startup] The server command failed. Check the configuration and runtime environment.");
  }
  KeepStartupFailureVisible(argc, result);
  return result;
}
