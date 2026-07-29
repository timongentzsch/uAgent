// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SIGNALS_H_
#define UAGENT_INCLUDE_CORE_SIGNALS_H_
// Interrupt state. Ctrl+C aborts an in-flight stream and only exits when
// there is nothing to abort; handlers touch async-signal-safe state only.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

#include "include/core/term.h"

namespace uagent {

inline volatile sig_atomic_t g_streaming = 0;
inline volatile sig_atomic_t g_steering_active = 0;
// argv[0], so the agent can re-invoke itself for a subagent. A bare name is
// resolved by the child's shell via PATH; a relative one still works because
// fork keeps the cwd.
inline std::string g_argv0;

// Signals may only touch sig_atomic_t. Steering runs on an ordinary C++ thread
// and therefore uses a real atomic; abort_requested() joins the two domains.
inline volatile sig_atomic_t g_signal_abort = 0;
inline std::atomic<bool> g_thread_abort{false};
inline constexpr int kFgMax = 16;  // concurrent foreground shells
inline volatile sig_atomic_t g_child_pgids[kFgMax] = {};
inline constexpr int kMcpMax = 64;  // tracked MCP server processes
inline volatile sig_atomic_t g_mcp_pids[kMcpMax] =
    {};  // live MCP pgids — TERMed on exit
inline constexpr int kBgMax = 64;
inline volatile sig_atomic_t g_bg_pids[kBgMax] = {};

// Pid slot tables read by the signal handler. Parallel tool workers claim slots
// concurrently, so writers serialise here; the handler only ever reads.
inline void TrackPid(volatile sig_atomic_t* slots, int count, pid_t pid,
                     bool add) {
  static std::mutex slots_mutex;
  std::lock_guard<std::mutex> lock(slots_mutex);
  for (int i = 0; i < count; ++i) {
    if (add ? slots[i] == 0 : slots[i] == pid) {
      slots[i] = add ? pid : 0;
      return;
    }
  }
}

inline bool AbortRequested() {
  return g_signal_abort != 0 || g_thread_abort.load(std::memory_order_relaxed);
}

inline void RequestAbort() {
  g_thread_abort.store(true, std::memory_order_relaxed);
}

inline void ClearAbort() {
  g_thread_abort.store(false, std::memory_order_relaxed);
  g_signal_abort = 0;
}

inline void SigintHandler(int signal_number) {
  if (signal_number == SIGINT && (g_streaming || g_steering_active)) {
    g_signal_abort = 1;
    return;
  }
  for (int i = 0; i < kFgMax; i++) {
    if (g_child_pgids[i] > 0 &&
        kill(-static_cast<pid_t>(g_child_pgids[i]), SIGKILL) != 0) {
      kill(static_cast<pid_t>(g_child_pgids[i]),
           SIGKILL);  // may not have reached setsid()
    }
  }
  for (int i = 0; i < kBgMax; i++) {
    if (g_bg_pids[i] > 0) {
      kill(-static_cast<pid_t>(g_bg_pids[i]), SIGTERM);
      kill(static_cast<pid_t>(g_bg_pids[i]), SIGTERM);
    }
  }
  for (int i = 0; i < kMcpMax; i++) {
    if (g_mcp_pids[i] > 0) {  // whole group: servers spawn their own workers
      kill(-static_cast<pid_t>(g_mcp_pids[i]), SIGTERM);
      kill(static_cast<pid_t>(g_mcp_pids[i]), SIGTERM);
    }
  }
  if (g_signal_tty) {
    (void)(write(STDOUT_FILENO, kTerminalRestore,
                 sizeof(kTerminalRestore) - 1) < 0);
    (void)(write(STDOUT_FILENO, kTerminalModeReset,
                 sizeof(kTerminalModeReset) - 1) < 0);
  }
  _exit(128 + signal_number);
}

// Run fn with Ctrl+C wired to cancel it instead of exiting the
// program; returns true if the user cancelled. Centralizes the flag dance so
// the reset order can't drift between call sites.
template <class F>
inline bool RunCancellable(F&& fn) {
  g_streaming = 1;
  if (!AbortRequested()) fn();
  g_streaming = 0;
  bool cancelled = AbortRequested();
  ClearAbort();
  return cancelled;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SIGNALS_H_
