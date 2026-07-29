// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TERM_H_
#define UAGENT_INCLUDE_CORE_TERM_H_
// Terminal colors and the blocking-call spinner. Every accessor returns an
// empty string when stdout is not a TTY, so callers need no conditionals.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace uagent {

inline bool g_tty = false;
inline volatile sig_atomic_t g_signal_tty = 0;
inline constexpr char kTerminalRestore[] = "\033[0m\033[39m\033[49m";
// Separate from TERMINAL_RESTORE, which RST() emits mid-stream as a pure SGR
// reset.
inline constexpr char kTerminalModeReset[] = "\033[?2004l";
inline const char* DIM() { return g_tty ? "\033[2m" : ""; }
inline const char* RST() { return g_tty ? kTerminalRestore : ""; }
inline const char* CYAN() { return g_tty ? "\033[36m" : ""; }
inline const char* YEL() { return g_tty ? "\033[33m" : ""; }
inline const char* RED() { return g_tty ? "\033[31m" : ""; }
inline const char* BOLD() { return g_tty ? "\033[1m" : ""; }
inline const char* BoldOff() { return g_tty ? "\033[22m" : ""; }
inline const char* ITAL() { return g_tty ? "\033[3m" : ""; }
inline const char* ItalOff() { return g_tty ? "\033[23m" : ""; }
inline const char* FgDfl() {
  return g_tty ? "\033[39m" : "";
}  // default foreground
inline bool LightUi() {
  static const bool kLight = [] {
    const char* theme = getenv("UAGENT_THEME");
    if (theme && strcmp(theme, "light") == 0) return true;
    if (theme && strcmp(theme, "dark") == 0) return false;
    const char* value = getenv("COLORFGBG");
    const char* bg = value ? strrchr(value, ';') : nullptr;
    return bg && strtol(bg + 1, nullptr, 10) >= 7;
  }();
  return kLight;
}
inline const char* PANEL() {
  return !g_tty      ? ""
         : LightUi() ? "\033[38;5;234m\033[48;5;255m"
                     : "\033[38;5;255m\033[48;5;234m";
}
inline const char* PanelMuted() {
  return !g_tty      ? ""
         : LightUi() ? "\033[38;5;243m\033[48;5;255m"
                     : "\033[38;5;244m\033[48;5;234m";
}
inline void PanelClearLine() {
  if (!g_tty) return;
  fputs(PANEL(), stdout);
  fputs("\r\033[2K\r", stdout);
  fflush(stdout);
}
inline void TerminalRestore() {
  if (!g_tty) return;
  fputs(kTerminalRestore, stdout);
  fputs(kTerminalModeReset, stdout);
  fflush(stdout);
}
inline void TerminalClearToEnd() {
  if (!g_tty) return;
  fputs("\r\033[K", stdout);
  fflush(stdout);
}
// Wraps pasted text in \e[200~ … \e[201~ so a multi-line paste arrives as one
// unit.
inline void BracketedPaste(bool on) {
  if (!g_tty) return;
  fputs(on ? "\033[?2004h" : kTerminalModeReset, stdout);
  fflush(stdout);
}

// Animates while a call blocks with nothing to print. stop() is idempotent and
// wakes the thread immediately — it runs on the first-streamed-byte path.
class TerminalSpinner {
 public:
  explicit TerminalSpinner(bool enabled = true, std::string label = "working")
      : label_(std::move(label)) {
    Start(enabled);
  }

  void Start(bool enabled = true) {
    if (thread_.joinable() || !enabled || !g_tty) return;
    done_ = false;
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!done_) {
        printf("\r%s%c %s%s", DIM(), "|/-\\"[frame_], label_.c_str(), RST());
        fflush(stdout);
        frame_ = (frame_ + 1) & 3;
        wake_.wait_for(lock, std::chrono::milliseconds(100),
                       [this] { return done_; });
      }
    });
  }

  ~TerminalSpinner() { Stop(); }
  TerminalSpinner(const TerminalSpinner&) = delete;
  TerminalSpinner& operator=(const TerminalSpinner&) = delete;

  void Stop() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wake_.notify_one();
    thread_.join();
    TerminalClearToEnd();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_ = false;
  int frame_ = 0;
  std::string label_;
  std::thread thread_;
};

// code colors (256-color, readable on dark and light themes; glamour-inspired)
inline const char* CODE() {
  return g_tty ? "\033[38;5;203m" : "";
}  // inline `code`
inline const char* CodeBlk() {
  return g_tty ? "\033[38;5;110m" : "";
}  // fenced block body
inline const char* MATH() { return g_tty ? "\033[38;5;141m" : ""; }  // LaTeX

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TERM_H_
