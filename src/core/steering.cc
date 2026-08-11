// Copyright 2026 Timon Gentzsch

#include "include/core/steering.h"

#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/signals.h"

namespace uagent {

bool Steering::Take() {
  bool value = requested_.exchange(false);
  ClearAbort();
  DebugLog("steering_take");
  return value;
}

void Steering::Queue(std::string input) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  queued_.push_back(std::move(input));
  DebugLog("steering_queued", {{"queued", queued_.size()}});
}

std::vector<std::string> Steering::TakeQueued() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  std::vector<std::string> result;
  result.reserve(queued_.size());
  while (!queued_.empty()) {
    result.push_back(std::move(queued_.front()));
    queued_.pop_front();
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

void Steering::Request() {
  requested_ = true;
  RequestAbort();
  DebugLog("steering_interrupt");
}

Steering& SteeringState() {
  static Steering steering;
  return steering;
}

}  // namespace uagent
