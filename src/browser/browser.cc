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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/browser/runtime.h"
#include "include/core/platform.h"

namespace uagent::browser {
namespace {
constexpr size_t kMaxPacket = size_t{12} * 1024 * 1024;

bool CloseOnExec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

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
}  // namespace

std::string DataDirectory() {
  const char* configured = getenv("UAGENT_BROWSER_DATA");
  return configured && *configured ? configured : "";
}
std::string SocketPath() {
  auto base = DataDirectory();
  return base.empty() ? "" : base + "/service.sock";
}
std::string RfbPath() {
  auto base = DataDirectory();
  return base.empty() ? "" : base + "/display.sock";
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
      if (errno != ENOENT || mkdir(current.c_str(), 0700) != 0 ||
          lstat(current.c_str(), &info) != 0) {
        return false;
      }
    }
    if (!S_ISDIR(info.st_mode)) return false;
  }
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 && info.st_uid == geteuid() &&
         (info.st_mode & 0077) == 0;
}

ServiceProcess::~ServiceProcess() {
  owner.Reset();
  if (pid <= 0) return;
  // Let the service finish both child shutdowns and release the profile lock.
  if (ReapPidFor(pid, nullptr,
                 std::chrono::milliseconds(kServiceShutdownGraceMs))) {
    return;
  }
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
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
      "HOME=" + DataDirectory(), "UAGENT_BROWSER_DATA=" + DataDirectory(),
      "LANG=C.UTF-8", "TMPDIR=/tmp"};
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
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (Request({{"op", "ping"}}, 100).value("ok", false)) return true;
    poll(nullptr, 0, 20);
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
  Fd listener(socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener || !CloseOnExec(listener.Get())) return 2;
  unlink(path.c_str());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path.c_str(), path.size() + 1);
  mode_t old = umask(0077);
  int bound = bind(listener.Get(), reinterpret_cast<sockaddr*>(&address),
                   sizeof(address));
  umask(old);
  if (bound != 0 || listen(listener.Get(), 16) != 0) return 2;
  chmod(path.c_str(), 0600);
  auto runtime = std::make_shared<Runtime>();
  auto mutex = std::make_shared<std::mutex>();
  auto active = std::make_shared<std::atomic<int>>(0);
  for (;;) {
    pollfd wait[] = {{owner_fd, POLLIN | POLLHUP, 0},
                     {listener.Get(), POLLIN, 0}};
    if (poll(wait, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (wait[0].revents) break;
    if (!(wait[1].revents & POLLIN)) continue;
    Fd client(accept(listener.Get(), nullptr, nullptr));
    if (!client || !CloseOnExec(client.Get())) continue;
    if (active->fetch_add(1) >= 8) {
      --*active;
      continue;
    }
    std::thread([runtime, mutex, active, client = std::move(client)]() mutable {
      json request;
      if (Packet(client.Get(), request, false, 30000)) {
        json answer;
        {
          std::lock_guard lock(*mutex);
          answer = runtime->Execute(request);
        }
        Packet(client.Get(), answer, true, 30000);
      }
      --*active;
    }).detach();
  }
  {
    std::lock_guard lock(*mutex);
    runtime->Shutdown();
  }
  unlink(path.c_str());
  return 0;
}
}  // namespace uagent::browser
