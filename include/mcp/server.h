// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_SERVER_H_
#define UAGENT_INCLUDE_MCP_SERVER_H_
// The MCP server process: its buffers, its spawn, and the runtime that
// owns every session so transports close before curl is torn down.

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/child_env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/time.h"

extern char** environ;

namespace uagent {

// Give stdio EOF a brief chance to perform the protocol's polite shutdown
// before escalating to signals, then again before the kill.
inline constexpr auto kMcpEofGrace = std::chrono::milliseconds(200);
inline constexpr auto kMcpTermGrace = std::chrono::milliseconds(300);

struct McpServer;
inline void McpShutdown(McpServer& server);

struct McpServer {
  std::string name;
  pid_t pid = -1;
  int in = -1,
      out = -1;  // in: we write (server's stdin) · out: we read (its stdout)
  bool alive = false;
  std::string rbuf;  // partial line from the server
  int64_t next_id = 1;
  size_t response_cap = 16 * 1024 * 1024;
  json config;
  json roots = json::array();
  bool tools_changed = false;

  ~McpServer() { Shutdown(); }

  // Closing our ends is the polite stop signal for a stdio server.
  void CloseTransport() {
    alive = false;
    if (in >= 0) {
      close(in);
      in = -1;
    }
    if (out >= 0) {
      close(out);
      out = -1;
    }
  }

  // Signals target the whole group (-pid): servers spawn their own workers.
  void Escalate(int signal_number) {
    if (pid > 0 && kill(-pid, signal_number) != 0) kill(pid, signal_number);
  }

  // True once the child is gone and dropped from the SIGINT kill list.
  bool Reaped() {
    if (pid <= 0) return true;
    int status = 0;
    pid_t result = WaitPid(pid, &status, WNOHANG);
    if (result != pid && !(result < 0 && errno == ECHILD)) return false;
    TrackPid(g_mcp_pids, kMcpMax, pid, /*add=*/false);
    pid = -1;
    return true;
  }

  void ReapBlocking() {
    if (pid <= 0) return;
    int status = 0;
    WaitPid(pid, &status);
    TrackPid(g_mcp_pids, kMcpMax, pid, /*add=*/false);
    pid = -1;
  }

  // Also called the moment a server is detected dead/wedged, so fds close and
  // the child is reaped immediately, not at program exit.
  void Shutdown() { McpShutdown(*this); }
};

// One shutdown ladder for one or many servers: close every transport first so
// they all observe EOF together, then escalate in lockstep. Batching is the
// reason ShutdownAll cannot simply loop over one-server shutdowns.
template <class Servers>
inline void McpShutdownGroup(const Servers& servers) {
  auto wait_all = [&](std::chrono::milliseconds grace) {
    auto deadline = std::chrono::steady_clock::now() + grace;
    for (;;) {
      bool pending = false;
      for (McpServer* server : servers) pending = !server->Reaped() || pending;
      if (!pending) return;
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return;
      pollfd child_event = {ChildSignalFd(), POLLIN, 0};
      int ready = poll(&child_event, 1, PollTimeoutMs(deadline));
      if (ready > 0 && (child_event.revents & POLLIN)) DrainChildSignal();
    }
  };
  for (McpServer* server : servers) server->CloseTransport();
  wait_all(kMcpEofGrace);
  for (McpServer* server : servers) server->Escalate(SIGTERM);
  wait_all(kMcpTermGrace);
  for (McpServer* server : servers) {
    if (server->pid <= 0) continue;
    kill(-server->pid, SIGKILL);
    kill(server->pid, SIGKILL);
    server->ReapBlocking();
  }
}

inline void McpShutdown(McpServer& server) {
  McpServer* one[] = {&server};
  McpShutdownGroup(one);
}

// Explicit session owner. Destruction closes every transport and reaps every
// server before curl and the rest of the process runtime are torn down.
class McpRuntime {
 public:
  McpRuntime() = default;
  ~McpRuntime() { ShutdownAll(); }
  McpRuntime(const McpRuntime&) = delete;
  McpRuntime& operator=(const McpRuntime&) = delete;

  void Add(std::unique_ptr<McpServer> server) {
    servers_.push_back(std::move(server));
  }

  const std::vector<std::unique_ptr<McpServer>>& Servers() const {
    return servers_;
  }

  void ShutdownAll() {
    std::vector<McpServer*> live;
    live.reserve(servers_.size());
    for (auto& server : servers_) live.push_back(server.get());
    McpShutdownGroup(live);
    servers_.clear();
  }

 private:
  std::vector<std::unique_ptr<McpServer>> servers_;
};

// One voice for server status: notes are dim and bulleted, errors are red.
inline void McpNote(const std::string& name, const std::string& msg) {
  std::string safe_name = TerminalSafe(name);
  std::string safe_msg = TerminalSafe(msg);
  printf("%s· mcp: %s — %s%s\n", DIM(), safe_name.c_str(), safe_msg.c_str(),
         RST());
}
inline void McpError(const std::string& name, const std::string& msg) {
  std::string safe_name = TerminalSafe(name);
  std::string safe_msg = TerminalSafe(msg);
  printf("%smcp: %s — %s%s\n", RED(), safe_name.c_str(), safe_msg.c_str(),
         RST());
}

inline std::string McpLogPath(const std::string& name) {
  return UagentDir(kMcpDir) + "/" + SafeFileComponent(name) + "-" +
         std::to_string(getpid()) + ".log";
}

// Where a failing server's own diagnostics went. Appended to every error the
// model or user sees, so the next step is always obvious.
inline std::string McpStderrHint(const std::string& name) {
  return " (stderr: " + McpLogPath(name) + ")";
}

inline bool McpBufferOk(McpServer& s) {
  if (s.response_cap == 0 || s.rbuf.size() <= s.response_cap) return true;
  s.Shutdown();
  return false;
}

// Append one available chunk of server stdout to the read buffer. Returns
// false when the server closed or the response cap was hit. McpWrite's
// opportunistic drain tolerates EOF, so it passes eof_is_fatal=false.
inline bool McpFillBuffer(McpServer& s, bool eof_is_fatal = true) {
  char buffer[1 << 16];
  ssize_t count;
  do {
    count = read(s.out, buffer, sizeof buffer);
  } while (count < 0 && errno == EINTR);
  if (count <= 0) {
    if (!eof_is_fatal) return true;
    s.Shutdown();
    return false;
  }
  s.rbuf.append(buffer, static_cast<size_t>(count));
  return McpBufferOk(s);
}

// Take one complete newline-terminated message out of the read buffer.
inline bool McpTakeLine(McpServer& s, std::string& line) {
  size_t newline = s.rbuf.find('\n');
  if (newline == std::string::npos) return false;
  line = s.rbuf.substr(0, newline);
  s.rbuf.erase(0, newline + 1);
  return true;
}

inline bool McpSpawn(
    McpServer& s, const std::string& cmd, const std::vector<std::string>& args,
    const std::vector<std::pair<std::string, std::string>>& env,
    const std::string& cwd, size_t log_bytes) {
  int inp[2], outp[2];  // inp: us -> server stdin, outp: server stdout -> us
  if (pipe(inp) != 0) return false;
  if (pipe(outp) != 0) {
    close(inp[0]);
    close(inp[1]);
    return false;
  }
  // parent ends must not leak into later-spawned servers (a leaked write end
  // would keep a sibling's stdin open forever, defeating EOF shutdown)
  fcntl(inp[1], F_SETFD, FD_CLOEXEC);
  fcntl(outp[0], F_SETFD, FD_CLOEXEC);
  // nonblocking writes: a blocking write() of a large request would ignore
  // the drain in mcp_write and reintroduce the two-full-pipes deadlock
  fcntl(inp[1], F_SETFL, O_NONBLOCK);
  int errfd = open(McpLogPath(s.name).c_str(), O_CREAT | O_WRONLY | O_TRUNC,
                   kPrivateFileMode);
  ChildEnvironment child_environment(env);
  pid_t pid = fork();
  if (pid < 0) {
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    if (errfd >= 0) close(errfd);
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);  // own group: terminal Ctrl+C must not kill the server
    dup2(inp[0], 0);
    dup2(outp[1], 1);
    if (errfd >= 0) {
      dup2(errfd, 2);
      close(errfd);
    }
    close(inp[0]);
    close(inp[1]);
    close(outp[0]);
    close(outp[1]);
    struct rlimit file_limit = {static_cast<rlim_t>(log_bytes),
                                static_cast<rlim_t>(log_bytes)};
    setrlimit(RLIMIT_FSIZE, &file_limit);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      dprintf(STDERR_FILENO, "cannot chdir to %s: %s\n", cwd.c_str(),
              strerror(errno));
      _exit(126);
    }
    environ = child_environment.Data();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(cmd.c_str()));
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    signal(SIGINT, SIG_DFL);
    execvp(cmd.c_str(), argv.data());
    _exit(127);
  }
  close(inp[0]);
  close(outp[1]);
  if (errfd >= 0) close(errfd);
  s.pid = pid;
  s.in = inp[1];
  s.out = outp[0];
  s.alive = true;
  // SIGINT idle-exit TERMs these (see core/signals.h)
  TrackPid(g_mcp_pids, kMcpMax, pid, /*add=*/true);
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_SERVER_H_
