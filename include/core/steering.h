// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// In the persistent composer, Enter queues guidance for the active turn and a
// bare Escape is the only input that requests a foreground abort.

#include <termios.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "include/core/term.h"

namespace uagent {

// Enter queues active-turn guidance. Bare ESC alone requests foreground
// interruption; it does not cancel supervised background work.

class Steering {
 public:
  ~Steering();

  bool Start();
  void Stop();

  bool Requested() const { return requested_; }

  void Request();

  bool Take();

  void Queue(std::string input);
  std::vector<std::string> TakeQueued();
  size_t QueuedCount() const;

 private:
  void Restore();
  void Listen();

  std::atomic<bool> running_{false}, requested_{false};
  mutable std::mutex queue_mutex_;
  std::deque<std::string> queued_;
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
