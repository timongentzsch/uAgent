// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// In the persistent composer, Enter queues guidance for the active turn and a
// bare Escape is the only input that requests a foreground abort.

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace uagent {

// Enter queues active-turn guidance. Bare ESC alone requests foreground
// interruption; it does not cancel supervised background work.

class Steering {
 public:
  bool Requested() const { return requested_; }

  void Request();

  bool Take();

  void Queue(std::string input);
  std::vector<std::string> TakeQueued();
  size_t QueuedCount() const;

 private:
  std::atomic<bool> requested_{false};
  mutable std::mutex queue_mutex_;
  std::deque<std::string> queued_;
};

Steering& SteeringState();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STEERING_H_
