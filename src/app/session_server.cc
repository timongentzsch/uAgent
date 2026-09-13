// Copyright 2026 Timon Gentzsch
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/app/session.h"
#include "include/core/child_env.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/core/platform.h"

namespace uagent::session {
namespace {
Fd Socket(const std::string& path, bool listen) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) return {};
  std::copy(path.begin(), path.end(), address.sun_path);
  Fd socket(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!socket) return {};
  fcntl(socket.Get(), F_SETFD, FD_CLOEXEC);
  if (listen) {
    unlink(path.c_str());  // caller holds the runtime lease
    if (bind(socket.Get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) ||
        chmod(path.c_str(), 0600) || ::listen(socket.Get(), 16)) {
      return {};
    }
  } else if (connect(socket.Get(), reinterpret_cast<sockaddr*>(&address),
                     sizeof(address))) {
    return {};
  }
  fcntl(socket.Get(), F_SETFL, O_NONBLOCK);
  return socket;
}
}  // namespace
std::string SocketPath(const std::string& path) {
  // AF_UNIX paths are limited to 104 bytes on macOS, independently of HOME.
  return "/tmp/uagent-" + std::to_string(geteuid()) + "-" +
         HashHex(GlobalBase()) + "/" + HashHex(path) + ".sock";
}
Connection Connect(const std::string& path) {
  Connection result;
  const std::string address = SocketPath(path);
  struct stat info{};
  if (lstat(std::filesystem::path(address).parent_path().c_str(), &info) ||
      !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077) || lstat(address.c_str(), &info) ||
      !S_ISSOCK(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & 0077)) {
    return result;
  }
  result.socket = Socket(address, false);
  if (!result.socket) return result;
  // The server's first frame is a small, newline-terminated handshake. Read
  // exactly through that newline, leaving snapshot/event frames on the socket.
  std::string line;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (line.size() < 1024 && std::chrono::steady_clock::now() < deadline) {
    pollfd ready{result.socket.Get(), POLLIN, 0};
    if (poll(&ready, 1, 100) <= 0) continue;
    char byte;
    if (read(result.socket.Get(), &byte, 1) != 1) break;
    if (byte == '\n') {
      json hello = json::parse(line, nullptr, false);
      result.generation = JsonValue(hello, "generation", "");
      result.pid = JsonValue(hello, "pid", -1);
      if (JsonValue(hello, "v", 0) == kProtocol &&
          JsonValue(hello, "session_id", "") == HashHex(path) &&
          OpaqueId(result.generation) && result.pid > 0) {
        return result;
      }
      break;
    }
    line += byte;
  }
  result.socket.Reset();
  return result;
}
Connection Open(const std::string& executable, const std::string& cwd,
                const std::string& path, const std::string& title,
                const Options& options, std::string& error) {
  auto connected = Connect(path);
  if (connected.socket) return connected;
  const auto folder = std::filesystem::path(SocketPath(path)).parent_path();
  CreatePrivateDirectories(folder);
  struct stat info{};
  if (lstat(folder.c_str(), &info) || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077)) {
    error = "session runtime directory must be private";
    return {};
  }
  const std::string launch = folder.string() + "/" + RandomToken(16) + ".json";
  Pipe owner;
  const bool delegated = options.overrides.contains("UAGENT_DEPTH");
  if (delegated && !owner.Open()) {
    error = "cannot create collaborator lifetime pipe";
    return {};
  }
  json config = {{"owner_fd", delegated ? 3 : -1},
                 {"overrides", options.overrides},
                 {"yolo", options.yolo},
                 {"debug", options.debug},
                 {"debug_path", options.debug_path},
                 {"trust_project", options.trust_project}};
  if (!AtomicWriteFile(launch, JsonDump(config), 0600, false, error)) return {};
  std::vector<std::string> args{
      executable, "--session-worker", cwd, path, HashHex(path), title, launch};
#ifdef __linux__
  // A service restart kills its cgroup even after setsid(). Give the runtime
  // a user scope so its lifetime belongs to the session, not the web service.
  if (!delegated && getenv("INVOCATION_ID")) {
    args.insert(args.begin(), {"systemd-run", "--user", "--scope", "--quiet",
                               "--collect", "--expand-environment=no",
                               "--unit=uagent-session-" + HashHex(path) + "-" +
                                   RandomToken(4),
                               "--"});
  }
#endif
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
  }
  if (delegated) {
    posix_spawn_file_actions_adddup2(&actions, owner.read.Get(), 3);
  }
  pid_t pid = -1;
  // This is the same application continuing in its session process. Keep
  // provider credential references and user limits; tool children apply their
  // separate, restricted environment policy when they are dispatched.
  std::optional<ChildEnvironment> environment;
  if (delegated) environment.emplace();
  int status =
      posix_spawnp(&pid, args.front().c_str(), &actions, nullptr, argv.data(),
                   environment ? environment->Data() : ProcessEnvironment());
  posix_spawn_file_actions_destroy(&actions);
  if (status) {
    unlink(launch.c_str());
    error = "cannot start session runtime: " + std::string(strerror(status));
    return {};
  }
  // Reap only our child. Detaching the client must not stop its runtime.
  std::thread([pid] { WaitPid(pid, nullptr); }).detach();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    connected = Connect(path);
    if (connected.socket) {
      connected.owner = std::move(owner.write);
      return connected;
    }
    poll(nullptr, 0, 20);
  }
  unlink(launch.c_str());
  error = "session runtime did not become ready";
  return {};
}

struct Server::State {
  struct Client {
    Fd fd;
    std::string input, output;
    size_t sent = 0;
  };
  Fd listener;
  FileLease lease;
  Pipe wake;
  std::thread thread;
  std::atomic<bool> stopped{false};
  std::mutex mutex;
  std::deque<json> pending;
  size_t pending_bytes = 0;
  std::string path, id, generation;
  std::function<bool(const json&)> command;
  json snapshot;
  std::deque<std::string> replay;
  size_t replay_bytes = 0;
  bool replay_gap = false;
  uint64_t sequence = 0;
  std::vector<Client> clients;

  void Run() {
    auto drain_deadline = std::chrono::steady_clock::time_point::max();
    for (;;) {
      if (stopped &&
          drain_deadline == std::chrono::steady_clock::time_point::max()) {
        drain_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      }
      std::vector<pollfd> watches{{listener.Get(), POLLIN, 0},
                                  {wake.read.Get(), POLLIN, 0}};
      for (const auto& client : clients) {
        watches.push_back(
            {client.fd.Get(),
             static_cast<int16_t>(
                 POLLIN | (client.sent < client.output.size() ? POLLOUT : 0)),
             0});
      }
      if (poll(watches.data(), static_cast<nfds_t>(watches.size()),
               stopped ? 10 : -1) < 0 &&
          errno != EINTR) {
        break;
      }
      wake.Drain();
      // Drain before accepting: a new client gets one coherent snapshot and
      // its ordered suffix, never a snapshot that overtakes buffered events.
      std::deque<json> frames;
      {
        std::lock_guard lock(mutex);
        frames.swap(pending);
        pending_bytes = 0;
      }
      for (auto& frame : frames) {
        frame["sequence"] = ++sequence;
        std::string line = JsonDump(frame) + '\n';
        if (JsonValue(frame, "kind", "") == "state" &&
            (snapshot.is_null() || JsonValue(frame, "checkpoint", false))) {
          snapshot = frame;
          replay_gap = false;
          replay.clear();
          replay_bytes = 0;
        } else if (JsonValue(frame, "kind", "") != "outcome") {
          replay.push_back(line);
          replay_bytes += line.size();
          if (replay_bytes > kQueueBytes) {
            replay.clear();
            replay_bytes = 0;
            replay_gap = true;
            // Refresh at the authoritative channel; no file tailing.
            command({{"v", kProtocol},
                     {"session_id", id},
                     {"generation", generation},
                     {"kind", "refresh"},
                     {"request_id", RandomToken(16)}});
          }
        }
        for (auto& client : clients) client.output += line;
      }
      for (size_t i = 0; i < clients.size(); ++i) {
        auto& client = clients[i];
        const auto events = watches[i + 2].revents;
        if (events & (POLLERR | POLLNVAL)) client.fd.Reset();
        if (events & (POLLIN | POLLHUP)) {
          char buffer[8192];
          ssize_t bytes = read(client.fd.Get(), buffer, sizeof buffer);
          if (bytes > 0) {
            client.input.append(buffer, static_cast<size_t>(bytes));
          } else if (bytes == 0 || (errno != EINTR && errno != EAGAIN)) {
            client.fd.Reset();
          }
          size_t newline;
          while (client.fd &&
                 (newline = client.input.find('\n')) != std::string::npos) {
            if (newline > kCommandBytes) {
              client.fd.Reset();
              break;
            }
            json value =
                json::parse(client.input.substr(0, newline), nullptr, false);
            client.input.erase(0, newline + 1);
            if (!value.is_object() || JsonValue(value, "v", 0) != kProtocol ||
                !command(value)) {
              client.fd.Reset();
            }
          }
        }
        if (client.input.size() > kCommandBytes ||
            client.output.size() - client.sent > kQueueBytes) {
          client.fd.Reset();
        }
        if (client.fd && client.sent < client.output.size()) {
          ssize_t bytes =
              write(client.fd.Get(), client.output.data() + client.sent,
                    client.output.size() - client.sent);
          if (bytes > 0) {
            client.sent += static_cast<size_t>(bytes);
          } else if (bytes < 0 && errno != EINTR && errno != EAGAIN) {
            client.fd.Reset();
          }
          if (client.sent == client.output.size()) {
            client.output.clear();
            client.sent = 0;
          } else if (client.sent >= 65536) {
            client.output.erase(0, client.sent);
            client.sent = 0;
          }
        }
      }
      std::erase_if(clients, [](const auto& client) { return !client.fd; });
      if (stopped &&
          (std::chrono::steady_clock::now() >= drain_deadline ||
           std::all_of(clients.begin(), clients.end(), [](const auto& client) {
             return client.output.empty();
           }))) {
        break;
      }
      if (!stopped && (watches[0].revents & POLLIN)) {
        Fd fd(accept(listener.Get(), nullptr, nullptr));
        if (fd && clients.size() < 16) {
          fcntl(fd.Get(), F_SETFL, O_NONBLOCK);
          fcntl(fd.Get(), F_SETFD, FD_CLOEXEC);
          Client client{std::move(fd),
                        {},
                        JsonDump({{"v", kProtocol},
                                  {"kind", "hello"},
                                  {"pid", getpid()},
                                  {"session_id", id},
                                  {"generation", generation}}) +
                            '\n'};
          if (!snapshot.is_null()) client.output += JsonDump(snapshot) + '\n';
          if (replay_gap) {
            client.output += JsonDump({{"v", kProtocol},
                                       {"kind", "gap"},
                                       {"session_id", id},
                                       {"generation", generation}}) +
                             '\n';
          }
          for (const auto& event : replay) client.output += event;
          clients.push_back(std::move(client));
        }
      }
    }
  }
};
Server::Server() : state_(std::make_unique<State>()) {}
Server::~Server() {
  state_->stopped = true;
  state_->wake.Wake();
  if (state_->thread.joinable()) state_->thread.join();
  if (state_->listener) unlink(state_->path.c_str());
}
bool Server::Start(const std::string& path, const std::string& generation,
                   std::function<bool(const json&)> command) {
  auto& state = *state_;
  state.path = SocketPath(path);
  state.id = HashHex(path);
  state.generation = generation;
  state.command = std::move(command);
  std::string error;
  if (!state.lease.Acquire(state.path + ".lock", error) || !state.wake.Open()) {
    return false;
  }
  state.listener = Socket(state.path, true);
  if (!state.listener) return false;
  state.thread = std::thread([&state] { state.Run(); });
  return true;
}
void Server::Publish(json frame) {
  std::lock_guard lock(state_->mutex);
  size_t bytes = JsonEstimatedBytes(frame);
  if (bytes > kFrameBytes) {
    frame = {{"v", kProtocol},
             {"kind", "gap"},
             {"session_id", state_->id},
             {"generation", state_->generation},
             {"reason", "frame exceeds transport limit"}};
    bytes = JsonEstimatedBytes(frame);
  }
  // Backpressure must never pause tools or inference. A gap asks consumers to
  // refresh rather than silently pretending that dropped events were delivered.
  if (state_->pending_bytes + bytes > kQueueBytes) {
    state_->pending.clear();
    state_->pending.push_back({{"v", kProtocol},
                               {"kind", "gap"},
                               {"session_id", state_->id},
                               {"generation", state_->generation}});
    state_->pending_bytes = 256;
  }
  state_->pending_bytes += bytes;
  state_->pending.push_back(std::move(frame));
  state_->wake.Wake();
}
}  // namespace uagent::session
