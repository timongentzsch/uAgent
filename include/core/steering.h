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
  struct Message {
    std::string text, request_id;
    bool auto_start = true;
  };
  bool Requested() const { return requested_; }

  void Request();

  bool Take();

  void Queue(std::string input, std::string request_id = "",
             bool auto_start = true);
  // Drop one queued entry by client request id. True when an entry was
  // still queued; consumed steering is never resurrected.
  bool Recall(const std::string& request_id);
  std::vector<Message> TakeMessages();
  std::vector<Message> TakeAutoStartMessages();
  size_t QueuedCount() const;

 private:
  std::atomic<bool> requested_{false};
  mutable std::mutex queue_mutex_;
  std::deque<Message> queued_;
};

Steering& SteeringState();

// Passive waits use queued guidance as a soft-yield condition. The input owner
// must publish the queue entry before waking the process supervisor.
bool SteeringYieldRequested();
// Pollable companion for waits on state outside ProcessSupervisor, such as a
// detached log owned by another uagent process.
int SteeringWakeFd();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STEERING_H_
