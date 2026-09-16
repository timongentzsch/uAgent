// Copyright 2026 Timon Gentzsch

#include "include/core/steering.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/platform.h"
#include "include/core/signals.h"

namespace uagent {
namespace {

int g_steering_wake[2] = {-1, -1};
std::once_flag g_steering_wake_once;

void InitializeSteeringWake() { (void)OpenNonblockingPipe(g_steering_wake); }

void NotifySteeringWake() {
  std::call_once(g_steering_wake_once, InitializeSteeringWake);
  WakeDescriptor(g_steering_wake[1]);
  // Condition-variable process waits only wake on the child-dispatch pipe,
  // so queued guidance must ring it too; aborts are not implied.
  WakeProcessWaits();
}

void DrainSteeringWake() { DrainDescriptor(g_steering_wake[0]); }

}  // namespace

bool Steering::Take() {
  bool value = requested_.exchange(false);
  ClearAbort();
  DebugLog("steering_take");
  return value;
}

void Steering::Queue(std::string input, std::string request_id,
                     bool auto_start, json attachments, json images) {
  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queued_.push_back({std::move(input), std::move(request_id), auto_start,
                       std::move(attachments), std::move(images)});
    queued = queued_.size();
  }
  NotifySteeringWake();
  DebugLog("steering_queued", {{"queued", queued}});
}

std::vector<Steering::Message> Steering::TakeMessages() {
  std::vector<Message> result;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    result.reserve(queued_.size());
    while (!queued_.empty()) {
      result.push_back(std::move(queued_.front()));
      queued_.pop_front();
    }
    DrainSteeringWake();
  }
  if (!result.empty()) {
    DebugLog("steering_delivered", {{"messages", result.size()}});
  }
  return result;
}

std::vector<Steering::Message> Steering::TakeAutoStartMessages() {
  std::vector<Message> result;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto it = queued_.begin(); it != queued_.end();) {
      if (it->auto_start) {
        result.push_back(std::move(*it));
        it = queued_.erase(it);
      } else {
        ++it;
      }
    }
    DrainSteeringWake();
  }
  if (!result.empty()) {
    DebugLog("steering_delivered", {{"messages", result.size()}});
  }
  return result;
}

size_t Steering::QueuedCount() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queued_.size();
}

bool Steering::Recall(const std::string& request_id) {
  if (request_id.empty()) return false;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  for (auto it = queued_.begin(); it != queued_.end(); ++it) {
    if (it->request_id == request_id) {
      queued_.erase(it);
      DebugLog("steering_recalled", {{"queued", queued_.size()}});
      return true;
    }
  }
  return false;
}

void Steering::Request() {
  requested_ = true;
  RequestAbort();
  DebugLog("steering_interrupt");
}

Steering& SteeringState() {
  static Steering steering;
  return steering;
}

bool SteeringYieldRequested() { return SteeringState().QueuedCount() > 0; }

int SteeringWakeFd() {
  std::call_once(g_steering_wake_once, InitializeSteeringWake);
  return g_steering_wake[0];
}

}  // namespace uagent
