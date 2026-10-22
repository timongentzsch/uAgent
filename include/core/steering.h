// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
// In the persistent composer, Enter queues guidance for the active turn and a
// bare Escape requests a foreground abort; explicit Insert now applies queued
// input by interrupting the current step.

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "include/core/json.h"
#include "include/core/thread_annotations.h"

namespace uagent {

// Enter queues active-turn guidance. Bare ESC alone requests foreground
// interruption; it does not cancel supervised background work.

class Steering {
 public:
  struct Message {
    std::string text, request_id;
    bool auto_start = true;
    // Resolved host-claimed attachments ({path,name,mime,bytes,image,id})
    // and their transcript display images. Empty for text-only guidance.
    json attachments = json::array();
    json images = json::array();
    // Queued for after the running turn ("Queue next"): no step applies it
    // and no wait yields to it; the turn's end starts it as a turn of its
    // own, in arrival order with everything else that may start one.
    bool after_turn = false;
    // Who wrote it when not the person (Agent::Arrive): {author, sender,
    // mail, re}. Null for the person's own message.
    json origin = nullptr;
  };
  bool Requested() const { return requested_; }

  void Request();

  bool Take();

  void Queue(std::string input, std::string request_id = "",
             bool auto_start = true, json attachments = json::array(),
             json images = json::array());
  void Queue(Message message);
  // Drop one queued entry by client request id. True when an entry was
  // still queued; consumed steering is never resurrected.
  bool Recall(const std::string& request_id);
  // Promote an existing entry and interrupt the foreground step atomically.
  // If delivery won the race, nothing is interrupted or enqueued again.
  bool InsertNow(const std::string& request_id);
  // Everything the running turn applies now: all but after-turn messages.
  std::vector<Message> TakeMessages();
  // The oldest message that may start a turn, leaving every other queued
  // message where it is; the new turn's first step applies the rest.
  std::optional<Message> TakeNextAutoStart();
  size_t QueuedCount() const;
  // Queued for the running turn itself, which waits and steps yield to.
  size_t SteerCount() const;

 private:
  std::atomic<bool> requested_{false};
  mutable std::mutex queue_mutex_;
  std::deque<Message> queued_ UAGENT_GUARDED_BY(queue_mutex_);
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
