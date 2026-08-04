// Copyright 2026 Timon Gentzsch

#include "include/core/steering.h"

#include <poll.h>
#include <unistd.h>

#include <cstdio>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/signals.h"
#include "include/core/term.h"

namespace uagent {

Steering::~Steering() { Stop(); }

bool Steering::Start() {
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

void Steering::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  Restore();
}

bool Steering::Take() {
  bool value = requested_.exchange(false);
  ClearAbort();
  DebugLog("steering_take");
  return value;
}

void Steering::Request() {
  requested_ = true;
  RequestAbort();
  DebugLog("steering_interrupt");
}

void Steering::Restore() {
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

void Steering::Listen() {
  while (running_) {
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 100) <= 0 || !(input.revents & POLLIN)) continue;
    unsigned char bytes[64];
    ssize_t count = read(STDIN_FILENO, bytes, sizeof bytes);
    for (ssize_t index = 0; index < count && running_; ++index) {
      if (bytes[index] != 0x1b) continue;
      if (index + 1 < count) break;  // arrow/Alt escape sequence
      struct pollfd next = {STDIN_FILENO, POLLIN, 0};
      if (poll(&next, 1, 30) > 0) {
        unsigned char discard[16];
        ssize_t discarded = read(STDIN_FILENO, discard, sizeof discard);
        (void)discarded;
        break;
      }
      Request();
      return;
    }
  }
}

Steering& SteeringState() {
  static Steering steering;
  return steering;
}

}  // namespace uagent
