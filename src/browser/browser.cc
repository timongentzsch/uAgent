// Copyright 2026 Timon Gentzsch

#include "include/browser/browser.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/browser/broker.h"
#include "include/core/config_registry.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/strings.h"
#include "include/transport/session.h"

namespace uagent::browser {
namespace {
constexpr size_t kMaxPacket = size_t{12} * 1024 * 1024;

bool Transfer(int fd, char* bytes, size_t count, bool write, int timeout_ms) {
  while (count) {
    pollfd wait{fd, 0, 0};
    wait.events = static_cast<decltype(wait.events)>(write ? POLLOUT : POLLIN);
    if (poll(&wait, 1, timeout_ms) <= 0) return false;
    ssize_t n = write ? ::write(fd, bytes, count) : ::read(fd, bytes, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    count -= static_cast<size_t>(n);
  }
  return true;
}

bool Packet(int fd, json& value, bool write, int timeout_ms) {
  std::string body;
  if (write) {
    body = JsonDump(value);
    if (body.size() > kMaxPacket) return false;
  }
  uint32_t length = static_cast<uint32_t>(body.size());
  unsigned char header[4] = {static_cast<unsigned char>(length >> 24),
                             static_cast<unsigned char>(length >> 16),
                             static_cast<unsigned char>(length >> 8),
                             static_cast<unsigned char>(length)};
  if (!Transfer(fd, reinterpret_cast<char*>(header), 4, write, timeout_ms)) {
    return false;
  }
  if (!write) {
    length = (static_cast<uint32_t>(header[0]) << 24) |
             (static_cast<uint32_t>(header[1]) << 16) |
             (static_cast<uint32_t>(header[2]) << 8) | header[3];
    if (length > kMaxPacket) return false;
    body.resize(length);
  }
  if (!Transfer(fd, body.data(), body.size(), write, timeout_ms)) return false;
  if (!write) value = json::parse(body, nullptr, false);
  return write || !value.is_discarded();
}

Fd Connect() { return ConnectUnix(SocketPath()); }

std::string& HostDataDirectory() {
  static std::string path;
  return path;
}
}  // namespace

void SetDataDirectory(std::string path) {
  HostDataDirectory() = std::move(path);
}

std::string DataDirectory() {
  if (!HostDataDirectory().empty()) return HostDataDirectory();
  return SettingText(Cfg("UAGENT_BROWSER_DATA"));
}
std::string SocketPath() {
  auto base = DataDirectory();
  return base.empty() ? "" : base + "/service.sock";
}
std::string RfbPath(const std::string& instance) {
  if (instance.size() != 1 || instance[0] < '0' || instance[0] > '4') return "";
  auto base = DataDirectory();
  if (instance != "0") base += "/p" + instance;
  return base.empty() ? "" : base + "/display.sock";
}

std::string Origin(const std::string& url) {
  const std::string lower = AsciiLower(url);
  const std::string scheme = lower.starts_with("https://")  ? "https"
                             : lower.starts_with("http://") ? "http"
                                                            : "";
  if (scheme.empty()) return "";
  std::string host = lower.substr(scheme.size() + 3);
  // A backslash ends the host as a slash does, as browsers read it.
  host = host.substr(0, host.find_first_of("/?#\\"));
  host = host.substr(
      host.rfind('@') == std::string::npos ? 0 : host.rfind('@') + 1);
  // Only what any browser reads alike: letters, digits, dots and dashes, an
  // IPv6 address in brackets, and a port up to 65535 with no leading zero.
  const size_t bracket = host.rfind(']');
  const size_t colon =
      host.find(':', bracket == std::string::npos ? 0 : bracket);
  const std::string name = host.substr(0, colon);
  const std::string port =
      colon == std::string::npos ? "" : host.substr(colon + 1);
  const bool literal = name.starts_with('[');
  if (name.empty() ||
      name.find_first_not_of(literal
                                 ? "[]0123456789abcdef:."
                                 : "abcdefghijklmnopqrstuvwxyz0123456789.-") !=
          std::string::npos ||
      (literal && (name.size() < 3 || !name.ends_with(']') ||
                   name.find_first_of("[]", 1) != name.size() - 1)) ||
      (colon != std::string::npos &&
       (port.empty() || port.size() > 5 || port[0] == '0' ||
        port.find_first_not_of("0123456789") != std::string::npos ||
        (port.size() == 5 && port > "65535")))) {
    return "";
  }
  if (port == (scheme == "https" ? "443" : "80")) host.resize(colon);
  return scheme + "://" + host;
}

bool EnsureDataDirectory(const std::string& path) {
  std::filesystem::path input(path);
  if (!input.is_absolute() || input != input.lexically_normal()) return false;
  std::filesystem::path current;
  for (const auto& part : input) {
    if (part == "..") return false;
    current /= part;
    struct stat info{};
    if (lstat(current.c_str(), &info) != 0) {
      if (errno != ENOENT || mkdir(current.c_str(), kPrivateDirMode) != 0 ||
          lstat(current.c_str(), &info) != 0) {
        return false;
      }
    }
    if (!S_ISDIR(info.st_mode)) return false;
  }
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 && info.st_uid == geteuid() &&
         (info.st_mode & kGroupOtherModeBits) == 0;
}

ServiceProcess::~ServiceProcess() {
  owner.Reset();
  if (pid <= 0) return;
  // Let the service finish both child shutdowns and release the profile lock.
  if (ReapPidFor(pid, nullptr,
                 std::chrono::milliseconds(kServiceShutdownGraceMs))) {
    return;
  }
  // One that does not heed the request is not waited for without end.
  kill(pid, SIGTERM);
  if (!ReapPidFor(pid, nullptr,
                  std::chrono::milliseconds(kChildShutdownGraceMs))) {
    kill(pid, SIGKILL);
    WaitPid(pid, nullptr);
  }
}

json Request(const json& command, int timeout_ms) {
  Fd fd = Connect();
  if (!fd) return {{"error", "browser service unavailable"}};
  json copy = command, answer;
  if (!Packet(fd.Get(), copy, true, timeout_ms) ||
      !Packet(fd.Get(), answer, false, timeout_ms)) {
    return {{"error", "browser service timed out or disconnected"}};
  }
  return answer;
}

bool StartService(const std::string& executable, ServiceProcess& process,
                  std::string& error) {
  if (DataDirectory().empty()) return true;
  if (!EnsureDataDirectory(DataDirectory()) ||
      SocketPath().size() >= sizeof(sockaddr_un::sun_path)) {
    error = "browser data directory must be private and have a short path";
    return false;
  }
  int owner[2];
  if (pipe(owner) != 0) {
    error = "cannot create browser service owner pipe";
    return false;
  }
  Fd parent(owner[1]), child(owner[0]);
  if (!CloseOnExec(parent.Get()) || !CloseOnExec(child.Get())) {
    error = "cannot isolate browser service owner pipe";
    return false;
  }
  Fd child_source(fcntl(child.Get(), F_DUPFD_CLOEXEC, 5));
  if (!child_source) {
    error = "cannot duplicate browser service owner pipe";
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, child_source.Get(), 3);
  posix_spawn_file_actions_addclose(&actions, child_source.Get());
  posix_spawn_file_actions_addclose(&actions, parent.Get());
  char* args[] = {const_cast<char*>(executable.c_str()),
                  const_cast<char*>("--browser-service"), nullptr};
  std::vector<std::string> environment = {
      "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
      "HOME=" + DataDirectory(),
      "UAGENT_BROWSER_DATA=" + DataDirectory(),
      "UAGENT_INTERNAL_RUNTIME_DIR=" + RuntimeDir(),
      "LANG=C.UTF-8",
      "TMPDIR=/tmp"};
  // Pages read the user's clock and language, not the container's UTC and
  // English: Chrome takes its zone from TZ and its languages from LANGUAGE.
  for (const char* name : {"TZ", "LANGUAGE"}) {
    if (const char* value = getenv(name); value && *value) {
      environment.push_back(std::string(name) + "=" + value);
    }
  }
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto& setting : environment) envp.push_back(setting.data());
  envp.push_back(nullptr);
  pid_t pid = -1;
  int result = posix_spawnp(&pid, executable.c_str(), &actions, nullptr, args,
                            envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) {
    error = std::string("cannot start browser service: ") + strerror(result);
    return false;
  }
  child.Reset();
  process.pid = pid;
  process.owner = std::move(parent);
  for (int waited = 0; waited < kServiceStartMs; waited += kPollTickMs) {
    if (Request({{"op", "ping"}}, kPingMs).value("ok", false)) {
      return true;
    }
    poll(nullptr, 0, kPollTickMs);
  }
  process.owner.Reset();
  error = "browser service did not start";
  return false;
}

int ServiceMain(int owner_fd) {
  const std::string path = SocketPath();
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) ||
      !EnsureDataDirectory(DataDirectory()) || !CloseOnExec(owner_fd)) {
    return 2;
  }
  Fd listener = ListenUnix(path, session::kSocketBacklog);
  if (!listener) return 2;
  auto runtime = std::make_shared<Broker>(DataDirectory());
  auto active = std::make_shared<std::atomic<int>>(0);
  // An idle Chrome is checked once a minute.
  constexpr int kOwnerPollMs = 60000;
  auto maintenance = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(kOwnerPollMs);
  for (;;) {
    pollfd wait[] = {{owner_fd, POLLIN | POLLHUP, 0},
                     {listener.Get(), POLLIN, 0}};
    int ready = poll(wait, 2, kOwnerPollMs);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (wait[0].revents) break;
    // A request in progress is work; skip the check rather than wait on it.
    if (std::chrono::steady_clock::now() >= maintenance) {
      runtime->Maintain();
      maintenance = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kOwnerPollMs);
    }
    if (!(wait[1].revents & POLLIN)) continue;
    Fd client(accept(listener.Get(), nullptr, nullptr));
    if (!client || !CloseOnExec(client.Get())) continue;
    if (active->fetch_add(1) >= 8) {
      --*active;
      continue;
    }
    std::thread([runtime, active, client = std::move(client)]() mutable {
      json request;
      if (Packet(client.Get(), request, false, kActionMs)) {
        json answer;
        answer = runtime->Execute(request);
        Packet(client.Get(), answer, true, kActionMs);
      }
      --*active;
    }).detach();
  }
  runtime->Shutdown();
  unlink(path.c_str());
  return 0;
}
}  // namespace uagent::browser
