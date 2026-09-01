// Copyright 2026 Timon Gentzsch

#include "include/core/signals.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "include/core/platform.h"
#include "include/core/term.h"

namespace uagent {

volatile sig_atomic_t g_streaming = 0;
volatile sig_atomic_t g_terminal_resized = 0;
std::atomic_flag g_signal_abort = ATOMIC_FLAG_INIT;
std::atomic<bool> g_thread_abort{false};
volatile sig_atomic_t g_child_pgids[kFgMax] = {};
volatile sig_atomic_t g_mcp_pids[kMcpMax] = {};
volatile sig_atomic_t g_bg_pids[kBgMax] = {};
bool g_tty = false;
bool g_color = false;
volatile sig_atomic_t g_signal_tty = 0;

namespace {

// Read by the fatal-signal and suspend handlers. Published before the armed
// flag and cleared after it, so observing the flag means observing the struct.
termios g_cooked_termios{};
termios g_raw_termios{};
volatile sig_atomic_t g_termios_armed = 0;
// Nothing drains the REPL's stdout pipe once the process is exiting or
// stopped, so handlers write to the terminal descriptor the composer saved.
volatile sig_atomic_t g_signal_terminal_fd = STDOUT_FILENO;
constexpr char kBracketedPasteOn[] = "\033[?2004h";

void WriteToTerminal(const char* bytes, size_t size) {
  (void)(write(static_cast<int>(g_signal_terminal_fd), bytes, size) < 0);
}

// Async-signal-safe: write(2) and tcsetattr(3) are both on the POSIX list.
void RestoreTerminalModesFromHandler() {
  if (g_signal_tty) {
    WriteToTerminal(kTerminalRestore, sizeof(kTerminalRestore) - 1);
    WriteToTerminal(kTerminalModeReset, sizeof(kTerminalModeReset) - 1);
  }
  if (g_termios_armed) tcsetattr(STDIN_FILENO, TCSANOW, &g_cooked_termios);
}

constexpr int kChildWakeMax = 32;
volatile sig_atomic_t g_abort_wake_write = -1;
volatile sig_atomic_t g_child_signal_write = -1;
volatile sig_atomic_t g_child_dispatch_write = -1;
volatile sig_atomic_t g_terminal_wake_write = -1;
int g_abort_wake_read = -1;
int g_child_signal_read = -1;
int g_child_dispatch_read = -1;
std::once_flag g_notification_once;
std::once_flag g_sigchld_once;

struct ChildWakeRegistry {
  std::mutex mutex;
  std::array<int, kChildWakeMax> descriptors{};
  std::atomic<bool> stopping{false};
  std::thread dispatcher;

  ~ChildWakeRegistry() {
    stopping.store(true, std::memory_order_relaxed);
    WakeDescriptor(g_child_dispatch_write);
    if (dispatcher.joinable()) dispatcher.join();
  }
};

ChildWakeRegistry* g_child_wakes = nullptr;

void InitializeNotificationsOnce() {
  int abort_pipe[2] = {-1, -1};
  if (OpenNonblockingPipe(abort_pipe)) {
    g_abort_wake_read = abort_pipe[0];
    g_abort_wake_write = abort_pipe[1];
  }
  int child_pipe[2] = {-1, -1};
  if (OpenNonblockingPipe(child_pipe)) {
    g_child_signal_read = child_pipe[0];
    g_child_signal_write = child_pipe[1];
  }
  int dispatch_pipe[2] = {-1, -1};
  if (OpenNonblockingPipe(dispatch_pipe)) {
    g_child_dispatch_read = dispatch_pipe[0];
    g_child_dispatch_write = dispatch_pipe[1];
    static ChildWakeRegistry child_wakes;
    g_child_wakes = &child_wakes;
    g_child_wakes->dispatcher = std::thread([] {
      pollfd event = {g_child_dispatch_read, POLLIN, 0};
      while (!g_child_wakes->stopping.load(std::memory_order_relaxed)) {
        int ready;
        do {
          ready = poll(&event, 1, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) continue;
        DrainDescriptor(g_child_dispatch_read);
        if (g_child_wakes->stopping.load(std::memory_order_relaxed)) break;
        std::lock_guard<std::mutex> lock(g_child_wakes->mutex);
        for (int descriptor : g_child_wakes->descriptors) {
          if (descriptor > 0) WakeDescriptor(descriptor);
        }
      }
    });
  }
}

std::string& MutableExecutablePath() {
  static std::string path;
  return path;
}

std::string& StartupExecutableIdentity() {
  static std::string identity;
  return identity;
}

}  // namespace

std::string FileIdentity(const std::string& path) {
  struct stat info{};
  if (path.empty() || stat(path.c_str(), &info) != 0) return std::string();
  return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
         std::to_string(info.st_size) + ":" + std::to_string(info.st_mtime);
}

bool ExecutableReplaced() {
  const std::string& startup = StartupExecutableIdentity();
  if (startup.empty()) return false;
  std::string current = FileIdentity(ExecutablePath());
  // A missing file is a move in flight, not a finished install; saying nothing
  // is better than telling someone to restart into a binary that is not there.
  if (current.empty() || current == startup) return false;
  StartupExecutableIdentity() = current;
  return true;
}

void SetExecutablePath(std::string path) {
  MutableExecutablePath() = std::move(path);
  StartupExecutableIdentity() = FileIdentity(MutableExecutablePath());
}

const std::string& ExecutablePath() { return MutableExecutablePath(); }

void InitializeSignalNotifications() {
  std::call_once(g_notification_once, InitializeNotificationsOnce);
}

int AbortWakeFd() {
  InitializeSignalNotifications();
  return g_abort_wake_read;
}

int ChildSignalFd() {
  InitializeSignalNotifications();
  return g_child_signal_read;
}

void DrainChildSignal() { DrainDescriptor(ChildSignalFd()); }

bool RegisterChildWakeFd(int fd, bool add) {
  InitializeSignalNotifications();
  if (!g_child_wakes || fd < 0) return false;
  std::lock_guard<std::mutex> lock(g_child_wakes->mutex);
  for (int& descriptor : g_child_wakes->descriptors) {
    if (add ? descriptor == 0 : descriptor == fd) {
      descriptor = add ? fd : 0;
      return true;
    }
  }
  return false;
}

void SetTerminalWakeFd(int fd) {
  g_terminal_wake_write = static_cast<sig_atomic_t>(fd);
}

void RequestAbort() {
  InitializeSignalNotifications();
  g_thread_abort.store(true, std::memory_order_relaxed);
  WakeDescriptor(g_abort_wake_write);
}

void ClearAbort() {
  g_thread_abort.store(false, std::memory_order_relaxed);
  g_signal_abort.clear(std::memory_order_relaxed);
}

void NormalizeAbortWake() {
  if (AbortRequested()) return;
  DrainDescriptor(AbortWakeFd());
  if (AbortRequested()) WakeDescriptor(g_abort_wake_write);
}

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

std::atomic_flag g_signal_idle_interrupt = ATOMIC_FLAG_INIT;
volatile sig_atomic_t g_quit_gesture = 0;

void SetQuitGesture(bool enabled) { g_quit_gesture = enabled ? 1 : 0; }

bool TakeIdleInterrupt() {
  bool seen = g_signal_idle_interrupt.test(std::memory_order_relaxed);
  g_signal_idle_interrupt.clear(std::memory_order_relaxed);
  return seen;
}

void SigintHandler(int signal_number) {
  if (signal_number == SIGINT && !g_streaming && g_quit_gesture) {
    // Nothing to kill: the composer asks before the next press exits.
    g_signal_idle_interrupt.test_and_set(std::memory_order_relaxed);
    return;
  }
  if (signal_number == SIGINT && g_streaming) {
    g_signal_abort.test_and_set(std::memory_order_relaxed);
    WakeDescriptor(g_abort_wake_write);
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
  }
  for (int index = 0; index < kMcpMax; ++index) {
    if (g_mcp_pids[index] <= 0) continue;
    pid_t pid = static_cast<pid_t>(g_mcp_pids[index]);
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
  }
  RestoreTerminalModesFromHandler();
  _exit(128 + signal_number);
}

void ArmTerminalModes(const termios& cooked, const termios& raw) {
  g_cooked_termios = cooked;
  g_raw_termios = raw;
  std::atomic_signal_fence(std::memory_order_release);
  g_termios_armed = 1;
}

void DisarmTerminalModes() {
  g_termios_armed = 0;
  std::atomic_signal_fence(std::memory_order_release);
}

void SetSignalTerminalFd(int fd) {
  g_signal_terminal_fd = static_cast<sig_atomic_t>(fd < 0 ? STDOUT_FILENO : fd);
}

namespace {

void TstpHandler(int);

void ContHandler(int) {
  // The default disposition was restored before the stop, so re-arm first.
  struct sigaction action{};
  action.sa_handler = TstpHandler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTSTP, &action, nullptr);
  if (g_termios_armed) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_raw_termios);
    if (g_signal_tty) {
      WriteToTerminal(kBracketedPasteOn, sizeof(kBracketedPasteOn) - 1);
    }
  }
  // The terminal may have been resized while we were stopped, and the repaint
  // path is the same either way.
  g_terminal_resized = 1;
  WakeDescriptor(g_terminal_wake_write);
}

void TstpHandler(int) {
  RestoreTerminalModesFromHandler();
  signal(SIGTSTP, SIG_DFL);
  raise(SIGTSTP);
}

void SigchldHandler(int) {
  WakeDescriptor(g_child_signal_write);
  WakeDescriptor(g_child_dispatch_write);
}

void SigwinchHandler(int) {
  g_terminal_resized = 1;
  WakeDescriptor(g_terminal_wake_write);
}

}  // namespace

void InstallSigchldHandler() {
  std::call_once(g_sigchld_once, [] {
    struct sigaction action{};
    action.sa_handler = SigchldHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &action, nullptr);
  });
}

void InstallSigwinchHandler() {
  struct sigaction action{};
  action.sa_handler = SigwinchHandler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGWINCH, &action, nullptr);
}

void InstallSuspendHandlers() {
  struct sigaction stop{};
  stop.sa_handler = TstpHandler;
  sigemptyset(&stop.sa_mask);
  sigaction(SIGTSTP, &stop, nullptr);
  struct sigaction resume{};
  resume.sa_handler = ContHandler;
  sigemptyset(&resume.sa_mask);
  resume.sa_flags = SA_RESTART;
  sigaction(SIGCONT, &resume, nullptr);
}

}  // namespace uagent
