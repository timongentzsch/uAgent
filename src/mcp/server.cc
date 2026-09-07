// Copyright 2026 Timon Gentzsch

#include "include/mcp/server.h"

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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/core/child_env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"

extern char** environ;

namespace uagent {
namespace {
void McpShutdownGroup(const std::vector<McpServer*>& servers) {
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
  }
  // A child in uninterruptible kernel sleep may ignore SIGKILL for an
  // unbounded period. Leave its pid for a later shutdown/reap attempt instead
  // of wedging the current turn or process teardown.
  wait_all(kMcpKillGrace);
}

}  // namespace

void McpServer::CloseTransport() {
  alive = false;
  in.Reset();
  out.Reset();
}

void McpServer::Escalate(int signal_number) {
  if (pid > 0 && kill(-pid, signal_number) != 0) kill(pid, signal_number);
}

bool McpServer::Reaped() {
  if (pid <= 0) return true;
  int status = 0;
  pid_t result = WaitPid(pid, &status, WNOHANG);
  if (result != pid && !(result < 0 && errno == ECHILD)) return false;
  TrackPid(g_mcp_pids, kMcpMax, pid, /*add=*/false);
  pid = -1;
  return true;
}

void McpRuntime::ShutdownAll() {
  std::vector<McpServer*> live;
  live.reserve(servers_.size());
  for (auto& server : servers_) live.push_back(server.get());
  McpShutdownGroup(live);
  servers_.clear();
}

void McpShutdown(McpServer& server) { McpShutdownGroup({&server}); }

void McpNote(const std::string& name, const std::string& msg) {
  Emit(NoticeEvent(PresentationStatus::kNeutral,
                   "· mcp: " + name + " — " + msg));
}

void McpError(const std::string& name, const std::string& msg) {
  Emit(NoticeEvent(PresentationStatus::kFailed, "mcp: " + name + " — " + msg));
}

std::string McpLogPath(const std::string& name) {
  return UagentDir(kMcpDir) + "/" + SafeFileComponent(name) + "-" +
         std::to_string(getpid()) + ".log";
}

std::string McpStderrHint(const std::string& name) {
  return " (stderr: " + McpLogPath(name) + ")";
}

bool McpBufferOk(McpServer& s) {
  if (s.response_cap == 0 || s.rbuf.size() <= s.response_cap) return true;
  s.Shutdown();
  return false;
}

bool McpFillBuffer(McpServer& s, bool eof_is_fatal) {
  char buffer[1 << 16];
  ssize_t count;
  do {
    count = read(s.out.Get(), buffer, sizeof buffer);
  } while (count < 0 && errno == EINTR);
  if (count <= 0) {
    if (!eof_is_fatal) return true;
    s.Shutdown();
    return false;
  }
  s.rbuf.append(buffer, static_cast<size_t>(count));
  return McpBufferOk(s);
}

bool McpTakeLine(McpServer& s, std::string& line) {
  size_t newline = s.rbuf.find('\n');
  if (newline == std::string::npos) return false;
  line = s.rbuf.substr(0, newline);
  s.rbuf.erase(0, newline + 1);
  return true;
}

bool McpSpawn(McpServer& s, const std::string& cmd,
              const std::vector<std::string>& args,
              const std::vector<std::pair<std::string, std::string>>& env,
              const std::string& cwd, size_t log_bytes) {
  int inp[2], outp[2];  // inp: us -> server stdin, outp: server stdout -> us
  if (pipe(inp) != 0) return false;
  Fd in_read(inp[0]), in_write(inp[1]);
  // Every early return from here closes all four ends.
  if (pipe(outp) != 0) return false;
  Fd out_read(outp[0]), out_write(outp[1]);
  // parent ends must not leak into later-spawned servers (a leaked write end
  // would keep a sibling's stdin open forever, defeating EOF shutdown)
  fcntl(in_write.Get(), F_SETFD, FD_CLOEXEC);
  fcntl(out_read.Get(), F_SETFD, FD_CLOEXEC);
  // nonblocking writes: a blocking write() of a large request would ignore
  // the drain in mcp_write and reintroduce the two-full-pipes deadlock
  fcntl(in_write.Get(), F_SETFL, O_NONBLOCK);
  Fd errfd(open(McpLogPath(s.name).c_str(), O_CREAT | O_WRONLY | O_TRUNC,
                kPrivateFileMode));
  ChildEnvironment child_environment(env);
  // Build every allocation-backed child input before fork. µAgent is
  // multithreaded here, so the child may only use async-signal-safe operations
  // before exec; allocator locks inherited from another thread can deadlock.
  std::vector<char*> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char*>(cmd.c_str()));
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    // Between fork and exec: explicit moves only, and every path ends in
    // _exit, so no destructor is relied upon.
    setpgid(0, 0);  // own group: terminal Ctrl+C must not kill the server
    dup2(in_read.Get(), 0);
    dup2(out_write.Get(), 1);
    if (errfd) {
      dup2(errfd.Get(), 2);
      close(errfd.Get());
    }
    close(in_read.Get());
    close(in_write.Get());
    close(out_read.Get());
    close(out_write.Get());
    struct rlimit file_limit = {static_cast<rlim_t>(log_bytes),
                                static_cast<rlim_t>(log_bytes)};
    setrlimit(RLIMIT_FSIZE, &file_limit);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    environ = child_environment.Data();
    signal(SIGINT, SIG_DFL);
    execvp(cmd.c_str(), argv.data());
    _exit(127);
  }
  in_read.Reset();
  out_write.Reset();
  errfd.Reset();
  s.pid = pid;
  s.in = std::move(in_write);
  s.out = std::move(out_read);
  s.alive = true;
  // SIGINT idle-exit TERMs these (see core/signals.h)
  TrackPid(g_mcp_pids, kMcpMax, pid, /*add=*/true);
  return true;
}

}  // namespace uagent
