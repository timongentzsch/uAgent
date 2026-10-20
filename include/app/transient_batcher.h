// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_APP_TRANSIENT_BATCHER_H_
#define UAGENT_INCLUDE_APP_TRANSIENT_BATCHER_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include "include/core/events.h"
#include "include/core/thread_annotations.h"

namespace uagent::session {

// A worker's events on their way to its clients. Streamed text is held
// back up to kStreamBatchInterval or kStreamBatchBytes and usage up to
// kUsagePublishInterval, so a fast model costs one frame per batch; the
// first of each in a turn, and every other event, goes at once, after
// whatever is held back.
class TransientBatcher {
 public:
  explicit TransientBatcher(std::function<void(const AppEvent&)> deliver);
  ~TransientBatcher();
  TransientBatcher(const TransientBatcher&) = delete;
  TransientBatcher& operator=(const TransientBatcher&) = delete;

  // Starts the thread that sends what is held back once its time is up.
  void Start();
  void Push(const AppEvent& event);
  // Stops that thread; what is still held back waits for Flush.
  void Stop();
  void Flush();

 private:
  static std::string DeltaKey(const AppEvent& event);
  void QueueDelta(const AppEvent& event) UAGENT_REQUIRES(mutex_);
  void QueueUsage(const AppEvent& event) UAGENT_REQUIRES(mutex_);
  void FlushDelta() UAGENT_REQUIRES(mutex_);
  void FlushUsage() UAGENT_REQUIRES(mutex_);
  void FlushLocked() UAGENT_REQUIRES(mutex_);
  void FlushLoop();

  std::function<void(const AppEvent&)> deliver_;
  std::optional<AppEvent> pending_delta_ UAGENT_GUARDED_BY(mutex_);
  std::optional<AppEvent> pending_usage_ UAGENT_GUARDED_BY(mutex_);
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread thread_;
  std::unordered_set<std::string> sent_delta_keys_ UAGENT_GUARDED_BY(mutex_);
  std::chrono::steady_clock::time_point delta_started_
      UAGENT_GUARDED_BY(mutex_){};
  std::chrono::steady_clock::time_point usage_sent_ UAGENT_GUARDED_BY(mutex_){};
  bool sent_usage_ UAGENT_GUARDED_BY(mutex_) = false;
  bool stop_ UAGENT_GUARDED_BY(mutex_) = false;
};

}  // namespace uagent::session

#endif  // UAGENT_INCLUDE_APP_TRANSIENT_BATCHER_H_
