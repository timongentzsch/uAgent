// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SIGNALS_H_
#define UAGENT_INCLUDE_CORE_SIGNALS_H_
// Interrupt state. Ctrl+C aborts an in-flight stream and only exits when
// there is nothing to abort; handlers touch async-signal-safe state only.

#include <signal.h>
#include <sys/types.h>

#include <atomic>
#include <csignal>
#include <string>

namespace uagent {

extern volatile sig_atomic_t g_streaming;
extern volatile sig_atomic_t g_terminal_resized;
// argv[0], so the agent can re-invoke itself for a subagent. A bare name is
// resolved by the child's shell via PATH; a relative one still works because
// fork keeps the cwd.
void SetExecutablePath(std::string path);
const std::string& ExecutablePath();

// Signals may only touch sig_atomic_t. Steering runs on an ordinary C++ thread
// and therefore uses a real atomic; abort_requested() joins the two domains.
extern volatile sig_atomic_t g_signal_abort;
extern std::atomic<bool> g_thread_abort;
inline constexpr int kFgMax = 16;  // concurrent foreground shells
extern volatile sig_atomic_t g_child_pgids[kFgMax];
inline constexpr int kMcpMax = 64;  // tracked MCP server processes
extern volatile sig_atomic_t g_mcp_pids[kMcpMax];
inline constexpr int kBgMax = 64;
extern volatile sig_atomic_t g_bg_pids[kBgMax];

// Pid slot tables read by the signal handler. Parallel tool workers claim slots
// concurrently, so writers serialise here; the handler only ever reads.
void TrackPid(volatile sig_atomic_t* slots, int count, pid_t pid, bool add);

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

void SigintHandler(int signal_number);
void InstallSigwinchHandler();

// Run fn with Ctrl+C wired to cancel it instead of exiting the program. The
// caller owns the abort flag because an outer operation may need to observe it.
template <class F>
inline bool RunCancellable(F&& fn) {
  g_streaming = 1;
  if (!AbortRequested()) fn();
  g_streaming = 0;
  return AbortRequested();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SIGNALS_H_
