// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// Immediate steering. Escape interrupts a response and opens a prompt; the
// typed message joins the next turn, a second Escape resumes.

#include <termios.h>

#include <atomic>
#include <thread>

namespace uagent {

// Bare ESC cancels active work; the next prompt either submits a replacement
// message with Enter or cancels with ESC.

class Steering {
 public:
  ~Steering();

  bool Start();
  void Stop();

  bool Requested() const { return requested_; }

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
      : active_(enabled && SteeringState().Start()) {}
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
