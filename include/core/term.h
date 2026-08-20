// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TERM_H_
#define UAGENT_INCLUDE_CORE_TERM_H_
// Terminal colors and the blocking-call spinner. Every accessor returns an
// empty string when stdout is not a TTY, so callers need no conditionals.

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "include/core/strings.h"

namespace uagent {

extern bool g_tty;
extern volatile sig_atomic_t g_signal_tty;
// True while the REPL owns a pinned composer, which paints its own status row
// and must not be raced by the spinner thread. State-free header: the flag
// itself lives in src/core/term.cc, like the activity registry below.
void SetPersistentComposer(bool active);
bool PersistentComposer();
inline constexpr char kTerminalRestore[] = "\033[0m\033[39m\033[49m";
// Separate from TERMINAL_RESTORE, which RST() emits mid-stream as a pure SGR
// reset.
inline constexpr char kTerminalModeReset[] = "\033[?2004l";
inline const char* DIM() { return g_tty ? "\033[2m" : ""; }
inline const char* RST() { return g_tty ? kTerminalRestore : ""; }
inline const char* CYAN() { return g_tty ? "\033[36m" : ""; }
inline const char* BLUE() { return g_tty ? "\033[38;5;68m" : ""; }
inline const char* MUTED() { return g_tty ? "\033[90m" : ""; }
inline const char* YEL() { return g_tty ? "\033[33m" : ""; }
inline const char* RED() { return g_tty ? "\033[31m" : ""; }
inline const char* GREEN() { return g_tty ? "\033[32m" : ""; }
inline const char* BOLD() { return g_tty ? "\033[1m" : ""; }
// The band behind an echoed user turn, so a prompt is findable in scrollback.
inline const char* InputBg() { return g_tty ? "\033[48;5;250m" : ""; }
// Erase to end of line. With background-colour-erase this extends the current
// background to the right edge, which is what turns the echo into a band.
inline const char* EraseToEol() { return g_tty ? "\033[K" : ""; }
inline const char* BoldOff() { return g_tty ? "\033[22m" : ""; }
inline const char* ITAL() { return g_tty ? "\033[3m" : ""; }
inline const char* ItalOff() { return g_tty ? "\033[23m" : ""; }
inline const char* FgDfl() {
  return g_tty ? "\033[39m" : "";
}  // default foreground
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
// The transient activity registry: the label (or rolling reasoning ticker)
// shown in the status row while a call is in flight. The state itself is
// implementation-owned; see src/core/term.cc.
uint64_t BeginTerminalActivity(std::string label);
void UpdateTerminalActivity(uint64_t id, std::string label);

// Switch an activity to rolling-ticker mode: `text` is the bounded,
// newline-collapsed reasoning buffer, kept behind the caller's static
// `prefix`. Each status frame renders the prefix plus a window of the buffer.
void SetTerminalActivityRolling(uint64_t id, const std::string& prefix,
                                const std::string& text);
void EndTerminalActivity(uint64_t id);

// The newest activity's static label, and whether it is in rolling mode.
std::string CurrentTerminalActivity();
bool CurrentTerminalActivityRolling();

// Render the newest activity for one animation frame, bounded to `columns`.
std::string RenderCurrentTerminalActivity(size_t columns);

// Animates while a call blocks with nothing to print. stop() is idempotent and
// wakes the thread immediately — it runs on the first-streamed-byte path.
class TerminalSpinner {
 public:
  explicit TerminalSpinner(bool enabled = true, std::string label = "working",
                           std::chrono::steady_clock::time_point started =
                               std::chrono::steady_clock::now())
      : started_(started == std::chrono::steady_clock::time_point()
                     ? std::chrono::steady_clock::now()
                     : started),
        label_(std::move(label)) {
    Start(enabled);
  }

  void Start(bool enabled = true) {
    if (active_ || !enabled || !g_tty) return;
    active_ = true;
    activity_id_ = BeginTerminalActivity(label_);
    if (PersistentComposer()) return;
    done_ = false;
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!done_) {
        double elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started_)
                             .count();
        const std::string shown =
            rolling_ ? RenderCurrentTerminalActivity(TerminalWidth(14))
                     : label_;
        printf("\r%s%c %s · %s%s%s", DIM(), "|/-\\"[frame_], shown.c_str(),
               FmtDuration(elapsed).c_str(), EraseToEol(), RST());
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

  void SetLabel(std::string label) {
    rolling_ = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      label_ = std::move(label);
    }
    UpdateTerminalActivity(activity_id_, label_.c_str());
    wake_.notify_one();
  }

  // Switch to rolling-ticker mode: `text` is the bounded, newline-collapsed
  // reasoning buffer, kept behind the caller's static `prefix`. Each status
  // frame renders the prefix plus a sliding window of the buffer.
  void SetRolling(const std::string& prefix, const std::string& text) {
    rolling_ = true;
    SetTerminalActivityRolling(activity_id_, prefix, text);
    wake_.notify_one();
  }

  void Stop() {
    if (!active_) return;
    if (thread_.joinable()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
      }
      wake_.notify_one();
      thread_.join();
      TerminalClearToEnd();
    }
    EndTerminalActivity(activity_id_);
    active_ = false;
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_ = false;
  bool active_ = false;
  int frame_ = 0;
  uint64_t activity_id_ = 0;
  std::chrono::steady_clock::time_point started_;
  std::string label_;
  bool rolling_ = false;
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
