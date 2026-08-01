// Copyright 2026 Timon Gentzsch

#include "include/core/signals.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <string>
#include <utility>

#include "include/core/term.h"

namespace uagent {

volatile sig_atomic_t g_streaming = 0;
volatile sig_atomic_t g_steering_active = 0;
volatile sig_atomic_t g_terminal_resized = 0;
volatile sig_atomic_t g_signal_abort = 0;
std::atomic<bool> g_thread_abort{false};
volatile sig_atomic_t g_child_pgids[kFgMax] = {};
volatile sig_atomic_t g_mcp_pids[kMcpMax] = {};
volatile sig_atomic_t g_bg_pids[kBgMax] = {};
bool g_tty = false;
volatile sig_atomic_t g_signal_tty = 0;

namespace {

std::string& MutableExecutablePath() {
  static std::string path;
  return path;
}

}  // namespace

void SetExecutablePath(std::string path) {
  MutableExecutablePath() = std::move(path);
}

const std::string& ExecutablePath() { return MutableExecutablePath(); }

void TrackPid(volatile sig_atomic_t* slots, int count, pid_t pid, bool add) {
  static std::mutex slots_mutex;
  std::lock_guard<std::mutex> lock(slots_mutex);
  for (int index = 0; index < count; ++index) {
    if (add ? slots[index] == 0 : slots[index] == pid) {
      slots[index] = add ? pid : 0;
      return;
    }
  }
}

void SigintHandler(int signal_number) {
  if (signal_number == SIGINT && (g_streaming || g_steering_active)) {
    g_signal_abort = 1;
    return;
  }
  for (int index = 0; index < kFgMax; ++index) {
    if (g_child_pgids[index] > 0 &&
        kill(-static_cast<pid_t>(g_child_pgids[index]), SIGKILL) != 0) {
      kill(static_cast<pid_t>(g_child_pgids[index]), SIGKILL);
    }
  }
  for (int index = 0; index < kBgMax; ++index) {
    if (g_bg_pids[index] <= 0) continue;
    pid_t pid = static_cast<pid_t>(g_bg_pids[index]);
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
  for (int index = 0; index < kMcpMax; ++index) {
    if (g_mcp_pids[index] <= 0) continue;
    pid_t pid = static_cast<pid_t>(g_mcp_pids[index]);
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
  }
  if (g_signal_tty) {
    (void)(write(STDOUT_FILENO, kTerminalRestore,
                 sizeof(kTerminalRestore) - 1) < 0);
    (void)(write(STDOUT_FILENO, kTerminalModeReset,
                 sizeof(kTerminalModeReset) - 1) < 0);
  }
  _exit(128 + signal_number);
}

void SigwinchHandler(int) { g_terminal_resized = 1; }

}  // namespace uagent
