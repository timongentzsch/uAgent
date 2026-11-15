// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_STEERING_H_
#define UAGENT_INCLUDE_CORE_STEERING_H_
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
    // Resolved attachments ({path,name,mime,bytes,image,id}) and their images.
    json attachments = json::array();
    json images = json::array();
    // Queued for after the running turn: no step applies it and no wait
    // yields to it; the turn's end starts it as a turn of its own.
    bool after_turn = false;
    // Who wrote it when not the person: {kind, mail, hops, agent}, `kind`
    // being "agent" (a report), "message" (other mail) or "activity".
    json origin = nullptr;
  };
  bool Requested() const { return requested_; }

  void Request();

  bool Take();

  void Queue(std::string input, std::string request_id = "",
             bool auto_start = true, json attachments = json::array(),
             json images = json::array());
  void Queue(Message message);
  // Drops one queued entry by client request id; true when it was still queued.
  bool Recall(const std::string& request_id);
  // Promote an existing entry and interrupt the foreground step atomically.
  // If delivery won the race, nothing is interrupted or enqueued again.
  bool InsertNow(const std::string& request_id);
  // Everything the running turn applies now: all but after-turn messages.
  std::vector<Message> TakeMessages();
  // The oldest message that may start a turn; the others stay queued.
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
// Pollable companion for waits on state outside ActivitySupervisor.
int SteeringWakeFd();

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_STEERING_H_
