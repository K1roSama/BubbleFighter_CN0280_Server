#include "operations/process/child_process.h"

#include <array>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// PSAPI needs Windows SDK types before its declarations.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

namespace bf::infrastructure::process {
class ChildProcess::Impl final {
 public:
#ifdef _WIN32
  HANDLE process{nullptr};
  HANDLE job{nullptr};
  HANDLE output{nullptr};
  DWORD pid{0U};

  ~Impl() {
    if (job) {
      TerminateJobObject(job, 1U);
      CloseHandle(job);
    }
    if (process) {
      WaitForSingleObject(process, 3000U);
      CloseHandle(process);
    }
    if (output) CloseHandle(output);
  }
#else
  pid_t pid{-1};
  int output{-1};
  std::optional<int> exit_code;

  ~Impl() {
    if (pid > 0 && !exit_code) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
    if (output >= 0) close(output);
  }
#endif
};

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() = default;

namespace {
#ifdef _WIN32
std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (!size) throw std::invalid_argument{"invalid UTF-8 process argument"};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::wstring Quote(const std::wstring& text) {
  std::wstring result{L"\""};
  std::size_t slashes = 0U;
  for (const auto c : text) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'"')
      result.append(slashes * 2U + 1U, L'\\');
    else
      result.append(slashes, L'\\');
    result += c;
    slashes = 0U;
  }
  result.append(slashes * 2U, L'\\');
  result += L'"';
  return result;
}
#endif
}  // namespace

std::unique_ptr<ChildProcess> ChildProcess::Launch(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    std::uint64_t memory_limit_bytes) {
  const auto path = std::filesystem::absolute(executable);
  if (!std::filesystem::is_regular_file(path) ||
      memory_limit_bytes < 16U * 1024U * 1024U)
    throw std::invalid_argument{"invalid child executable or memory limit"};
  auto impl = std::make_unique<Impl>();
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE write_pipe{nullptr};
  if (!CreatePipe(&impl->output, &write_pipe, &security, 0U))
    throw std::runtime_error{"cannot create child startup pipe"};
  if (!SetHandleInformation(impl->output, HANDLE_FLAG_INHERIT, 0U)) {
    CloseHandle(write_pipe);
    throw std::runtime_error{"cannot protect parent pipe"};
  }
  impl->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  limits.ProcessMemoryLimit = static_cast<SIZE_T>(memory_limit_bytes);
  if (!impl->job ||
      !SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    CloseHandle(write_pipe);
    throw std::runtime_error{"cannot configure child job"};
  }
  auto command = Quote(path.wstring());
  for (const auto& argument : arguments)
    command += L" " + Quote(Wide(argument));
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = write_pipe;
  startup.StartupInfo.hStdError = write_pipe;
  HANDLE input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    CloseHandle(write_pipe);
    throw std::runtime_error{"cannot create child input handle"};
  }
  startup.StartupInfo.hStdInput = input;
  SIZE_T attribute_bytes{0U};
  InitializeProcThreadAttributeList(nullptr, 1U, 0U, &attribute_bytes);
  std::vector<std::byte> attributes(attribute_bytes);
  startup.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U,
                                         &attribute_bytes)) {
    CloseHandle(write_pipe);
    CloseHandle(input);
    throw std::runtime_error{"cannot initialize child handle list"};
  }
  HANDLE inherited[] = {write_pipe, input};
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0U,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                 sizeof(inherited), nullptr, nullptr)) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(write_pipe);
    CloseHandle(input);
    throw std::runtime_error{"cannot restrict child handle inheritance"};
  }
  PROCESS_INFORMATION info{};
  const auto started = CreateProcessW(
      path.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
      nullptr, nullptr, &startup.StartupInfo, &info);
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  CloseHandle(write_pipe);
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  if (!started) throw std::runtime_error{"cannot start room process"};
  impl->process = info.hProcess;
  impl->pid = info.dwProcessId;
  if (!AssignProcessToJobObject(impl->job, info.hProcess)) {
    TerminateProcess(info.hProcess, 1U);
    CloseHandle(info.hThread);
    throw std::runtime_error{"cannot assign child to job"};
  }
  if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
    CloseHandle(info.hThread);
    throw std::runtime_error{"cannot resume child"};
  }
  CloseHandle(info.hThread);
#else
  int pipes[2];
  if (pipe(pipes) != 0) throw std::runtime_error{"cannot create child pipe"};
  std::vector<std::string> storage{path.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  const auto child = fork();
  if (child == 0) {
    close(pipes[0]);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    const rlimit limit{memory_limit_bytes, memory_limit_bytes};
    if (setrlimit(RLIMIT_AS, &limit) != 0) _exit(126);
    execv(path.c_str(), argv.data());
    _exit(127);
  }
  close(pipes[1]);
  if (child < 0) {
    close(pipes[0]);
    throw std::runtime_error{"cannot fork child"};
  }
  impl->pid = child;
  impl->output = pipes[0];
  fcntl(impl->output, F_SETFL, O_NONBLOCK);
  fcntl(impl->output, F_SETFD, FD_CLOEXEC);
#endif
  return std::unique_ptr<ChildProcess>{new ChildProcess(std::move(impl))};
}

std::string ChildProcess::ReadOutput() {
  if (!impl_) return {};
  std::array<char, 4096U> bytes{};
#ifdef _WIN32
  DWORD available = 0U;
  if (!PeekNamedPipe(impl_->output, nullptr, 0U, nullptr, &available,
                     nullptr) ||
      !available)
    return {};
  DWORD read = 0U;
  if (!ReadFile(impl_->output, bytes.data(), static_cast<DWORD>(bytes.size()),
                &read, nullptr))
    return {};
  return {bytes.data(), read};
#else
  const auto read = ::read(impl_->output, bytes.data(), bytes.size());
  return read > 0 ? std::string{bytes.data(), static_cast<std::size_t>(read)}
                  : std::string{};
#endif
}

ChildProcessMetrics ChildProcess::Metrics() {
  if (!impl_) return {.exit_code = 0};
  ChildProcessMetrics result;
#ifdef _WIN32
  result.process_id = impl_->pid;
  DWORD code = 0U;
  if (GetExitCodeProcess(impl_->process, &code) && code != STILL_ACTIVE)
    result.exit_code = static_cast<int>(code);
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  if (GetProcessMemoryInfo(impl_->process,
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                           sizeof(memory)))
    result.private_bytes = memory.PrivateUsage;
  DWORD handles = 0U;
  if (GetProcessHandleCount(impl_->process, &handles)) result.handles = handles;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetProcessTimes(impl_->process, &created, &exited, &kernel, &user)) {
    const auto ticks = [](FILETIME value) {
      return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
             value.dwLowDateTime;
    };
    result.cpu_microseconds = (ticks(kernel) + ticks(user)) / 10U;
  }
#else
  result.process_id = static_cast<std::uint64_t>(impl_->pid);
  if (!impl_->exit_code) {
    int status = 0;
    if (waitpid(impl_->pid, &status, WNOHANG) == impl_->pid)
      impl_->exit_code =
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
  result.exit_code = impl_->exit_code;
#endif
  return result;
}

void ChildProcess::Terminate() noexcept { impl_.reset(); }
}  // namespace bf::infrastructure::process
