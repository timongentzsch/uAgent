// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_SIGNALS_H_
#define UAGENT_INCLUDE_CORE_SIGNALS_H_
// Interrupt state. Ctrl+C aborts an in-flight stream and only exits when
// there is nothing to abort; handlers touch async-signal-safe state only.

#include <signal.h>
#include <sys/types.h>
#include <termios.h>

#include <atomic>
#include <csignal>
#include <string>

namespace uagent {

// Shared with signal handlers: lock-free atomics, which are async-signal-safe.
using SignalFlag = std::atomic<int>;
extern SignalFlag g_streaming;
extern SignalFlag g_terminal_resized;
// argv[0], so the agent can re-invoke itself for a child.
void SetExecutablePath(std::string path);
const std::string& ExecutablePath();

// Identity of a file on disk: device, inode, size and modification time.
std::string FileIdentity(const std::string& path);

// True once per replacement of the executable since startup.
bool ExecutableReplaced();

extern std::atomic_flag g_signal_abort;
extern std::atomic<bool> g_thread_abort;
inline constexpr int kMcpMax = 64;  // tracked MCP server processes
extern SignalFlag g_mcp_pids[kMcpMax];
// Supervised processes, foreground and background, that SIGINT kills.
inline constexpr int kActivityMax = 64;
extern SignalFlag g_activity_pids[kActivityMax];

// Claims or frees a pid slot. Writers serialise here; the handler only reads.
void TrackPid(SignalFlag* slots, int count, pid_t pid, bool add);

// While a LocalAbort is alive, its thread cancels through its own flag and
// neither sees nor clears the turn's abort (title generation, for one).
extern thread_local std::atomic<bool>* g_local_abort;
class LocalAbort {
 public:
  explicit LocalAbort(std::atomic<bool>& flag) : prior_(g_local_abort) {
    g_local_abort = &flag;
  }
  ~LocalAbort() { g_local_abort = prior_; }
  LocalAbort(const LocalAbort&) = delete;
  LocalAbort& operator=(const LocalAbort&) = delete;

 private:
  std::atomic<bool>* prior_;
};

inline bool AbortRequested() {
  if (g_local_abort) return g_local_abort->load(std::memory_order_relaxed);
  return g_signal_abort.test(std::memory_order_relaxed) ||
         g_thread_abort.load(std::memory_order_relaxed);
}

void RequestAbort();
void ClearAbort();
// Wakes condition-variable process waits without requesting an abort.
void WakeProcessWaits();
// An idle SIGINT, read once: the first half of the quit gesture.
bool TakeIdleInterrupt();
// Armed only while an interactive session is idle; otherwise SIGINT kills.
void SetQuitGesture(bool enabled);
// Persistent runtimes unwind through application teardown on TERM/HUP. With
// `stop_started`, a delegated child also stops what it started at once.
void SetGracefulShutdown(bool enabled, bool stop_started = false);
bool ShutdownRequested();
// Drain a stale abort byte only while the flag is clear, then re-arm if a
// concurrent request arrived during the drain.
void NormalizeAbortWake();

// Pollable signal bridges; initialize before handlers are installed. SIGCHLD
// writes the child-event pipe and every registered wake descriptor.
void InitializeSignalNotifications();
int AbortWakeFd();
int ChildSignalFd();
// Single-consumer process-wide child-event stream used by MCP shutdown.
void DrainChildSignal();
bool RegisterChildWakeFd(int fd, bool add);
// SIGWINCH writes here, so an idle event loop redraws.
void SetTerminalWakeFd(int fd);

// Terminal modes the fatal-signal and suspend handlers restore; without them
// a signal exit leaves the shell with ECHO and ICANON cleared.
void ArmTerminalModes(const termios& cooked, const termios& raw);
void DisarmTerminalModes();
// Where a handler writes its restoring escapes: the REPL's stdout is a pipe.
void SetSignalTerminalFd(int fd);

void SigintHandler(int signal_number);
void InstallSigchldHandler();
void InstallSigwinchHandler();
// Ctrl+Z drops raw mode and bracketed paste before stopping; SIGCONT re-arms
// them and asks the event loop for a repaint.
void InstallSuspendHandlers();

// Runs fn with Ctrl+C wired to cancel it. The abort flag stays set for the
// caller, since an outer operation may need to observe it.
template <class F>
inline bool RunCancellable(F&& fn) {
  const bool shared = g_local_abort == nullptr;
  if (shared) g_streaming = 1;
  if (!AbortRequested()) fn();
  if (shared) g_streaming = 0;
  return AbortRequested();
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_SIGNALS_H_
