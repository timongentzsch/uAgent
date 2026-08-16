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
}

void DrainSteeringWake() { DrainDescriptor(g_steering_wake[0]); }

}  // namespace

bool Steering::Take() {
  bool value = requested_.exchange(false);
  ClearAbort();
  DebugLog("steering_take");
  return value;
}

void Steering::Queue(std::string input) {
  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queued_.push_back(std::move(input));
    queued = queued_.size();
  }
  NotifySteeringWake();
  DebugLog("steering_queued", {{"queued", queued}});
}

std::vector<std::string> Steering::TakeQueued() {
  std::vector<std::string> result;
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

bool SteeringYieldRequested() { return SteeringState().QueuedCount() > 0; }

int SteeringWakeFd() {
  std::call_once(g_steering_wake_once, InitializeSteeringWake);
  return g_steering_wake[0];
}

}  // namespace uagent
