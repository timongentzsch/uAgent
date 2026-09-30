// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_TERM_H_
#define UAGENT_INCLUDE_CORE_TERM_H_
// Terminal colors and the blocking-call spinner. Every accessor returns an
// empty string when stdout is not a TTY, so callers need no conditionals.

#include <atomic>
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

// Separate questions: NO_COLOR and TERM=dumb suppress colour without
// suppressing cursor control, spinners or image protocols, and CLICOLOR_FORCE
// asks for colour down a pipe.
extern bool g_tty;
extern bool g_color;
// Bold, dim and italic are not colour, so NO_COLOR keeps them on a terminal
// that can show them. Colour, where it is on, brings them along.
extern bool g_attributes;
// A terminal whose locale cannot decode UTF-8 renders the row scaffolding as
// mojibake, so the glyphs fall back to ASCII at the point they are written.
extern bool g_unicode;
// The plain profile, for screen readers and transcripts: append-only rows that
// open with a spoken label, ASCII glyphs, no cursor control and no animation.
extern bool g_plain;
// Off for plain and for reduced motion: the spinner holds a still label.
extern bool g_motion;
extern volatile sig_atomic_t g_signal_tty;
bool ResolveColorEnabled(bool tty);
bool ResolveAttributesEnabled(bool tty);
bool ResolveUnicodeEnabled();
// Narrows what InitializeProcess resolved, once the configuration is known.
void ApplyTerminalProfile(bool plain, bool reduced_motion);
// Conversation text is always UTF-8, so width measurement needs a multibyte
// LC_CTYPE even when the environment names none. False when none exists.
bool EnsureUtf8Ctype();
// True while the REPL owns a pinned composer, which paints its own status row
// and must not be raced by the spinner thread. State-free header: the flag
// itself lives in src/core/term.cc, like the activity registry below.
void SetPersistentComposer(bool active);
bool PersistentComposer();
inline constexpr char kTerminalRestore[] = "\033[0m\033[39m\033[49m";
// Separate from TERMINAL_RESTORE, which RST() emits mid-stream as a pure SGR
// reset.
inline constexpr char kTerminalModeReset[] = "\033[?2004l";
// Two rules for every SGR accessor below: one for colour, one for attributes.
inline const char* Sgr(const char* sequence) { return g_color ? sequence : ""; }
inline const char* Attribute(const char* sequence) {
  return g_color || g_attributes ? sequence : "";
}
inline const char* DIM() { return Attribute("\033[2m"); }
inline const char* RST() { return Attribute(kTerminalRestore); }
inline const char* MUTED() { return Sgr("\033[90m"); }
inline const char* YEL() { return Sgr("\033[33m"); }
inline const char* RED() { return Sgr("\033[31m"); }
inline const char* GREEN() { return Sgr("\033[32m"); }
inline const char* BOLD() { return Attribute("\033[1m"); }
// The band behind an echoed user turn, so a prompt is findable in scrollback.
inline const char* InputBg() { return Sgr("\033[7m"); }
// Cursor control, not colour, so it follows g_tty. With background-colour-erase
// it extends the current background to the right edge, which bands the echo.
inline const char* EraseToEol() { return g_tty ? "\033[K" : ""; }
inline const char* BoldOff() { return Attribute("\033[22m"); }
inline const char* ITAL() { return Attribute("\033[3m"); }
inline const char* ItalOff() { return Attribute("\033[23m"); }
inline const char* FgDfl() { return Sgr("\033[39m"); }  // default foreground
// Cursor control, not colour, so no gate: only a terminal's own paths write it.
inline const char* ClearScreen() { return "\033[H\033[2J"; }
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
// The transient activity registry shown while a call is in flight.
uint64_t BeginTerminalActivity(std::string label);
void UpdateTerminalActivity(uint64_t id, std::string label);
void EndTerminalActivity(uint64_t id);
std::string CurrentTerminalActivity();

// Names what a blocking call waits on, for as long as it blocks.
class TerminalActivityLabel {
 public:
  explicit TerminalActivityLabel(std::string label)
      : id_(BeginTerminalActivity(std::move(label))) {}
  ~TerminalActivityLabel() { EndTerminalActivity(id_); }
  TerminalActivityLabel(const TerminalActivityLabel&) = delete;
  TerminalActivityLabel& operator=(const TerminalActivityLabel&) = delete;

 private:
  uint64_t id_;
};

// What a model round calls itself while it waits. It names no work -- it is
// the placeholder every other label is more informative than -- so a status
// row with something concrete to say may spend those columns on that instead.
// One spelling, because that decision is a comparison against this string.
inline constexpr const char* kWaitingActivity = "Working";

// The spinner the working row and a blocking call both animate: braille, or
// |/-\\ where the locale cannot show it.
inline const char* SpinnerFrame(size_t tick) {
  static constexpr const char* kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                            "⠴", "⠦", "⠧", "⠇", "⠏"};
  static constexpr const char* kAscii[] = {"|", "/", "-", "\\"};
  return g_unicode ? kFrames[tick % 10] : kAscii[tick % 4];
}

// Animates while a call blocks with nothing to print. stop() is idempotent and
// wakes the thread immediately — it runs on the first-streamed-byte path.
class TerminalSpinner {
 public:
  explicit TerminalSpinner(bool enabled = true) { Start(enabled); }

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
        const std::string row = DisplayTrunc(
            AsciiGlyphs(g_motion ? std::string(SpinnerFrame(frame_)) + " " +
                                       label_ + " · " + FmtDuration(elapsed)
                                 : label_ + "…"),
            TerminalWidth(1));
        printf("\r%s%s%s%s", DIM(), row.c_str(), EraseToEol(), RST());
        fflush(stdout);
        ++frame_;
        // Without motion only a new label or the stop redraws.
        if (g_motion) {
          wake_.wait_for(lock, std::chrono::milliseconds(100),
                         [this] { return done_; });
        } else {
          wake_.wait(lock);
        }
      }
    });
  }

  ~TerminalSpinner() { Stop(); }
  TerminalSpinner(const TerminalSpinner&) = delete;
  TerminalSpinner& operator=(const TerminalSpinner&) = delete;

  // Rename the live row when its observed operation changes.
  void SetLabel(std::string label) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      label_ = label;
    }
    UpdateTerminalActivity(activity_id_, std::move(label));
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
  size_t frame_ = 0;
  uint64_t activity_id_ = 0;
  std::chrono::steady_clock::time_point started_ =
      std::chrono::steady_clock::now();
  std::string label_ = kWaitingActivity;
  std::thread thread_;
};

// Code and math in the default foreground: colour was dropped on purpose, and
// these mark where it would apply.
inline const char* CODE() { return Sgr("\033[39m"); }     // inline `code`
inline const char* CodeBlk() { return Sgr("\033[39m"); }  // fenced block body
inline const char* MATH() { return Sgr("\033[39m"); }     // LaTeX

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_TERM_H_
