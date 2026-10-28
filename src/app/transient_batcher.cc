// Copyright 2026 Timon Gentzsch

#include "include/app/transient_batcher.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "include/core/json.h"
#include "include/transport/session.h"

namespace uagent::session {

TransientBatcher::TransientBatcher(std::function<void(const AppEvent&)> deliver)
    : deliver_(std::move(deliver)) {}

TransientBatcher::~TransientBatcher() { Stop(); }

void TransientBatcher::Start() {
  thread_ = std::thread([this] { FlushLoop(); });
}

void TransientBatcher::Push(const AppEvent& event) {
  std::lock_guard lock(mutex_);
  if (event.type == AppType(EventId::kTurnStarted)) {
    FlushLocked();
    sent_delta_keys_.clear();
    sent_usage_ = false;
  }
  if (event.type == AppType(EventId::kAnswerDelta) ||
      event.type == AppType(EventId::kReasoningDelta)) {
    QueueDelta(event);
    return;
  }
  if (event.type == AppType(EventId::kUsageUpdated)) {
    QueueUsage(event);
    return;
  }
  // A status line is not ordered against the text around it, so it does not
  // cut the pending batch short.
  if (event.type == AppType(EventId::kActivityStatus)) {
    deliver_(event);
    return;
  }
  FlushLocked();
  deliver_(event);
  if (event.type == AppType(EventId::kTurnCompleted) ||
      event.type == AppType(EventId::kTurnStopped)) {
    sent_delta_keys_.clear();
    sent_usage_ = false;
  }
}

void TransientBatcher::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    changed_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
}

void TransientBatcher::Flush() {
  std::lock_guard lock(mutex_);
  FlushLocked();
}

std::string TransientBatcher::DeltaKey(const AppEvent& event) {
  return event.type + "\n" + JsonValue(event.data, "response_id", "");
}

void TransientBatcher::QueueDelta(const AppEvent& event) {
  const auto now = std::chrono::steady_clock::now();
  const std::string key = DeltaKey(event);
  if (JsonValue(event.data, "reset", false) ||
      sent_delta_keys_.insert(key).second) {
    FlushLocked();
    deliver_(event);
    return;
  }
  if (pending_delta_ && DeltaKey(*pending_delta_) != key) FlushDelta();
  if (!pending_delta_) {
    pending_delta_ = event;
    delta_started_ = now;
  } else {
    json& text = pending_delta_->data["text"];
    if (!text.is_string()) text = "";
    if (const std::string* more = JsonStringRef(event.data, "text")) {
      text.get_ref<std::string&>() += *more;
    }
    if (JsonValue(event.data, "preview_truncated", false)) {
      pending_delta_->data["preview_truncated"] = true;
    }
    pending_delta_->sequence = event.sequence;
    pending_delta_->time = event.time;
  }
  const std::string* batched = JsonStringRef(pending_delta_->data, "text");
  if ((batched && batched->size() >= kStreamBatchBytes) ||
      now - delta_started_ >= kStreamBatchInterval) {
    FlushDelta();
  } else {
    changed_.notify_one();
  }
}

void TransientBatcher::QueueUsage(const AppEvent& event) {
  const auto now = std::chrono::steady_clock::now();
  if (!sent_usage_) {
    sent_usage_ = true;
    usage_sent_ = now;
    deliver_(event);
    return;
  }
  pending_usage_ = event;
  if (now - usage_sent_ >= kUsagePublishInterval) {
    FlushUsage();
  } else {
    changed_.notify_one();
  }
}

void TransientBatcher::FlushDelta() {
  if (!pending_delta_) return;
  AppEvent event = std::move(*pending_delta_);
  pending_delta_.reset();
  deliver_(event);
}

void TransientBatcher::FlushUsage() {
  if (!pending_usage_) return;
  AppEvent event = std::move(*pending_usage_);
  pending_usage_.reset();
  usage_sent_ = std::chrono::steady_clock::now();
  deliver_(event);
}

void TransientBatcher::FlushLocked() {
  FlushDelta();
  FlushUsage();
}

void TransientBatcher::FlushLoop() {
  UniqueLock lock(mutex_);
  for (;;) {
    changed_.wait(lock, [this]() UAGENT_REQUIRES(mutex_) {
      return stop_ || pending_delta_ || pending_usage_;
    });
    if (stop_) return;
    auto deadline = std::chrono::steady_clock::time_point::max();
    if (pending_delta_) {
      deadline = std::min(deadline, delta_started_ + kStreamBatchInterval);
    }
    if (pending_usage_) {
      deadline = std::min(deadline, usage_sent_ + kUsagePublishInterval);
    }
    changed_.wait_until(lock, deadline);
    if (stop_) return;
    const auto now = std::chrono::steady_clock::now();
    if (pending_delta_ && now - delta_started_ >= kStreamBatchInterval) {
      FlushDelta();
    }
    if (pending_usage_ && now - usage_sent_ >= kUsagePublishInterval) {
      FlushUsage();
    }
  }
}

}  // namespace uagent::session
