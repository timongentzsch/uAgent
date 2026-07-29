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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/term.h"

namespace uagent {

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
  bool tools_changed = false;

  ~McpServer() { Shutdown(); }
  // stdin EOF is the polite stop signal for stdio servers; escalate to
  // SIGTERM, then SIGKILL for the ones that don't watch their stdin.
  // Signals target the whole group (-pid): servers spawn their own workers.
  // Also called the moment a server is detected dead/wedged, so fds close
  // and the child is reaped immediately, not at program exit.
  void Shutdown() {
    alive = false;
    if (in >= 0) {
      close(in);
      in = -1;
    }
    if (out >= 0) {
      close(out);
      out = -1;
    }
    if (pid > 0) {
      int st;
      bool gone = false;
      // Give stdio EOF a brief chance to perform the protocol's polite
      // shutdown before escalating to signals.
      for (int i = 0; i < 4 && !gone; i++) {
        if (waitpid(pid, &st, WNOHANG) != 0) {
          gone = true;
        } else {
          usleep(50 * 1000);
        }
      }
      if (!gone && kill(-pid, SIGTERM) != 0) kill(pid, SIGTERM);
      for (int i = 0; i < 6 && !gone; i++) {
        if (waitpid(pid, &st, WNOHANG) != 0) {
          gone = true;
        } else {
          usleep(50 * 1000);
        }
      }
      if (!gone) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
      }
      for (int i = 0; i < kMcpMax; i++) {  // drop from the SIGINT kill list
        if (g_mcp_pids[i] == pid) g_mcp_pids[i] = 0;
      }
      pid = -1;
    }
  }
};

inline constexpr const char* kChromeMcpName = "chrome-devtools";
inline constexpr const char* kChromeMcpPackage = "chrome-devtools-mcp@latest";

inline json ChromeMcpConfig(const std::string& mode = "isolated") {
  json args = json::array({"-y", kChromeMcpPackage, "--no-usage-statistics",
                           "--no-performance-crux"});
  if (mode == "user") {
    std::string browser_url = EnvStr("UAGENT_CHROME_BROWSER_URL", "");
    if (browser_url.empty()) {
      args.push_back("--auto-connect");
    } else {
      args.push_back("--browser-url");
      args.push_back(std::move(browser_url));
    }
  } else {
    args.push_back("--isolated");
  }
  return {{"command", "npx"},
          {"args", std::move(args)},
          {"__uagent_builtin", "chrome-devtools"},
          {"__uagent_lazy", true},
          {"__uagent_mode", mode}};
}

inline bool McpConfigLazy(const json& config) {
  return JsonValue(config, "__uagent_lazy", false);
}

// Explicit session owner. Destruction closes every transport and reaps every
// server before curl and the rest of the process runtime are torn down.
class McpRuntime {
 public:
  McpRuntime() = default;
  ~McpRuntime() { ShutdownAll(); }
  McpRuntime(const McpRuntime&) = delete;
  McpRuntime& operator=(const McpRuntime&) = delete;

  McpServer* Add(std::unique_ptr<McpServer> server) {
    McpServer* raw = server.get();
    servers_.push_back(std::move(server));
    return raw;
  }

  size_t Size() const { return servers_.size(); }
  const std::vector<std::unique_ptr<McpServer>>& Servers() const {
    return servers_;
  }

  void ShutdownAll() {
    auto reaped = [](McpServer& server) {
      for (int i = 0; i < kMcpMax; ++i) {
        if (g_mcp_pids[i] == server.pid) g_mcp_pids[i] = 0;
      }
      server.pid = -1;
    };
    auto reap_ready = [&](McpServer& server) {
      if (server.pid <= 0) return true;
      int status = 0;
      pid_t result = waitpid(server.pid, &status, WNOHANG);
      if (result == server.pid || (result < 0 && errno == ECHILD)) {
        reaped(server);
        return true;
      }
      return false;
    };
    auto wait_all = [&](int attempts) {
      for (int attempt = 0; attempt < attempts; ++attempt) {
        bool pending = false;
        for (auto& server : servers_) pending = !reap_ready(*server) || pending;
        if (!pending) return;
        usleep(50 * 1000);
      }
    };

    // Close every transport first so all servers observe EOF together.
    for (auto& server : servers_) {
      server->alive = false;
      if (server->in >= 0) {
        close(server->in);
        server->in = -1;
      }
      if (server->out >= 0) {
        close(server->out);
        server->out = -1;
      }
    }
    wait_all(4);
    for (auto& server : servers_) {
      if (server->pid > 0 && kill(-server->pid, SIGTERM) != 0) {
        kill(server->pid, SIGTERM);
      }
    }
    wait_all(6);
    for (auto& server : servers_) {
      if (server->pid > 0) {
        kill(-server->pid, SIGKILL);
        kill(server->pid, SIGKILL);
        int status = 0;
        while (waitpid(server->pid, &status, 0) < 0 && errno == EINTR) {
        }
        reaped(*server);
      }
    }
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

inline bool McpBufferOk(McpServer& s) {
  if (s.response_cap == 0 || s.rbuf.size() <= s.response_cap) return true;
  s.Shutdown();
  return false;
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
  int errfd =
      open(McpLogPath(s.name).c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
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
    for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
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
  for (int i = 0; i < kMcpMax;
       i++) {  // SIGINT idle-exit TERMs these (see core/signals.h)
    if (g_mcp_pids[i] == 0) {
      g_mcp_pids[i] = pid;
      break;
    }
  }
  return true;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_SERVER_H_
