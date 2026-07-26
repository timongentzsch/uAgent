// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// Immediate steering. Escape interrupts a response and opens a prompt; the
// typed message joins the next turn, a second Escape resumes.

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "include/core/signals.h"
#include "include/core/term.h"

namespace uagent {

// Bare ESC cancels active work; the next prompt either submits a replacement
// message with Enter or cancels with ESC.

class Steering {
 public:
  ~Steering() { Stop(); }

  bool Start() {
    if (!g_tty || !isatty(STDIN_FILENO) || !SteeringEnabled() ||
        running_.exchange(true)) {
      return false;
    }
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
      running_ = false;
      return false;
    }
    termios raw = saved_;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    g_steering_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      g_steering_active = 0;
      running_ = false;
      return false;
    }
    terminal_raw_ = true;
    thread_ = std::thread([this] { Listen(); });
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    Restore();
  }

  bool Requested() const { return requested_; }

  bool Take() {
    bool value = requested_.exchange(false);
    ClearAbort();
    DebugLog("steering_take");
    return value;
  }

 private:
  void Restore() {
    if (terminal_raw_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
      terminal_raw_ = false;
    }
    g_steering_active = 0;
    if (g_tty) {
      fputs(RST(), stdout);
      fflush(stdout);
    }
  }

  void Listen() {
    while (running_) {
      struct pollfd input = {STDIN_FILENO, POLLIN, 0};
      if (poll(&input, 1, 100) <= 0 || !(input.revents & POLLIN)) continue;
      unsigned char bytes[64];
      ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
      for (ssize_t i = 0; i < count && running_; ++i) {
        if (bytes[i] != 0x1b) continue;
        if (i + 1 < count) break;  // arrow/Alt escape sequence
        struct pollfd next = {STDIN_FILENO, POLLIN, 0};
        if (poll(&next, 1, 30) > 0) {
          unsigned char discard[16];
          ssize_t discarded = read(STDIN_FILENO, discard, sizeof discard);
          (void)discarded;
          break;
        }
        requested_ = true;
        RequestAbort();
        DebugLog("steering_interrupt");
        return;
      }
    }
  }

  std::atomic<bool> running_{false}, requested_{false};
  bool terminal_raw_ = false;
  termios saved_{};
  std::thread thread_;
};

inline Steering g_steering;

class SteeringGuard {
 public:
  explicit SteeringGuard(bool enabled = true)
      : active_(enabled && g_steering.Start()) {}
  ~SteeringGuard() { Stop(); }
  void Stop() {
    if (!active_) return;
    g_steering.Stop();
    active_ = false;
  }

 private:
  bool active_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STEERING_H_
