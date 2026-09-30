// Copyright 2026 Timon Gentzsch

#include "include/core/steering.h"

#include <algorithm>
#include <iterator>
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

void Steering::Queue(std::string input, std::string request_id, bool auto_start,
                     json attachments, json images) {
  Queue({std::move(input), std::move(request_id), auto_start,
         std::move(attachments), std::move(images)});
}

void Steering::Queue(Message message) {
  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queued_.push_back(std::move(message));
    queued = queued_.size();
  }
  NotifySteeringWake();
  DebugLog("steering_queued", {{"queued", queued}});
}

std::vector<Steering::Message> Steering::TakeMessages() {
  std::vector<Message> result;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    auto later = std::stable_partition(
        queued_.begin(), queued_.end(),
        [](const Message& message) { return !message.after_turn; });
    result.assign(std::make_move_iterator(queued_.begin()),
                  std::make_move_iterator(later));
    queued_.erase(queued_.begin(), later);
    DrainSteeringWake();
  }
  if (!result.empty()) {
    DebugLog("steering_delivered", {{"messages", result.size()}});
  }
  return result;
}

std::optional<Steering::Message> Steering::TakeNextAutoStart() {
  std::optional<Message> result;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    auto next = std::find_if(queued_.begin(), queued_.end(),
                             [](const Message& m) { return m.auto_start; });
    if (next == queued_.end()) return std::nullopt;
    result = std::move(*next);
    queued_.erase(next);
    if (queued_.empty()) DrainSteeringWake();
  }
  DebugLog("steering_delivered", {{"messages", 1}});
  return result;
}

size_t Steering::QueuedCount() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queued_.size();
}

size_t Steering::SteerCount() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return static_cast<size_t>(std::ranges::count_if(
      queued_, [](const Message& message) { return !message.after_turn; }));
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

bool SteeringYieldRequested() { return SteeringState().SteerCount() > 0; }

int SteeringWakeFd() {
  std::call_once(g_steering_wake_once, InitializeSteeringWake);
  return g_steering_wake[0];
}

}  // namespace uagent
