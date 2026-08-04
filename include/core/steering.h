// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// Foreground interruption. In the persistent composer, Enter queues and a
// bare Escape is the only input that requests an abort.

#include <termios.h>

#include <atomic>
#include <thread>

#include "include/core/term.h"

namespace uagent {

// Bare ESC cancels active work; the next prompt either submits a replacement
// message with Enter or cancels with ESC.

class Steering {
 public:
  ~Steering();

  bool Start();
  void Stop();

  bool Requested() const { return requested_; }

  void Request();

  bool Take();

 private:
  void Restore();
  void Listen();

  std::atomic<bool> running_{false}, requested_{false};
  bool terminal_raw_ = false;
  termios saved_{};
  std::thread thread_;
};

Steering& SteeringState();

class SteeringGuard {
 public:
  explicit SteeringGuard(bool enabled = true)
      : active_(enabled && !g_persistent_composer && SteeringState().Start()) {}
  ~SteeringGuard() { Stop(); }
  void Stop() {
    if (!active_) return;
    SteeringState().Stop();
    active_ = false;
  }

 private:
  bool active_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STEERING_H_
