// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PROCESS_H_
#define UAGENT_INCLUDE_TOOLS_PROCESS_H_
// Supervised background work: shell job groups and the side-task pool the
// web search rides on. Both are polled at step boundaries rather than
// pushing from a thread.

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/tools/tool.h"

namespace uagent {

// The shell runner writes stdout+stderr to a bounded log. Longer commands keep
// running in a supervised process group and wait_background reports each log
// change to the model.
//
// Completed jobs are auto-detected at step boundaries: the agent loop checks
// the ProcessSupervisor before each model call and injects results — no
// background thread needed, no LLM output wasted.

struct BgJob {
  pid_t pid;
  std::string log, cmd;
  bool join_before_final = false;
  bool detached = false;
  std::optional<uintmax_t> observed_log_bytes;
  std::string kind;
};

class ProcessSupervisor {
 public:
  ProcessSupervisor() = default;
  ~ProcessSupervisor();
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  // `run` is parallel_safe, so several tool workers can touch the job table
  // at once. Every read and write goes through here, under one lock.
  template <class F>
  auto WithJobs(F&& fn) -> decltype(fn(std::declval<std::vector<BgJob>&>())) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fn(jobs_);
  }
  bool TryAdd(BgJob job, int64_t max_pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job.detached &&
        static_cast<int64_t>(
            std::count_if(jobs_.begin(), jobs_.end(), [](const BgJob& current) {
              return !current.detached;
            })) >= max_pending) {
      return false;
    }
    jobs_.push_back(std::move(job));
    return true;
  }
  size_t PendingCount() const {
    return CountIf([](const BgJob& j) { return !j.detached; });
  }
  size_t DetachedCount() const {
    return CountIf([](const BgJob& j) { return j.detached; });
  }
  size_t JoinableCount() const {
    return CountIf([](const BgJob& j) { return j.join_before_final; });
  }
  std::vector<pid_t> PendingPids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<pid_t> pids;
    for (const BgJob& job : jobs_) {
      if (!job.detached) pids.push_back(job.pid);
    }
    return pids;
  }

 private:
  template <class P>
  size_t CountIf(P predicate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), predicate));
  }
  mutable std::mutex mutex_;
  std::vector<BgJob> jobs_;
};

struct SideTaskResult {
  int64_t id = 0;
  std::string kind, label, output;
  CompletionStatus status = CompletionStatus::kSuccess;
  ToolErrorCode error = ToolErrorCode::kNone;
  double duration_ms = 0;
};

// In-process counterpart to ProcessSupervisor: side requests get a short
// foreground grace period, then report back between model steps. Every worker
// owns a cancellation flag and is joined before its session-owned supervisor.
class SideTaskSupervisor {
 public:
  using Work = std::function<ToolResult(const std::atomic<bool>&)>;

  ~SideTaskSupervisor() { CancelAll(); }
  SideTaskSupervisor() = default;
  SideTaskSupervisor(const SideTaskSupervisor&) = delete;
  SideTaskSupervisor& operator=(const SideTaskSupervisor&) = delete;

  int64_t Start(std::string kind, std::string label, Work work, int64_t limit,
                bool join_before_final = true) {
    auto job = std::make_shared<Job>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int64_t>(jobs_.size()) >= std::max(int64_t{1}, limit)) {
      return 0;
    }
    job->id = next_id_++;
    job->kind = std::move(kind);
    job->label = std::move(label);
    job->join_before_final = join_before_final;
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    auto started = std::chrono::steady_clock::now();
    int64_t id = job->id;
    std::string result_kind = job->kind, result_label = job->label;
    auto cancel = job->cancel;
    job->future = std::async(
        std::launch::async,
        [id, kind = std::move(result_kind), label = std::move(result_label),
         cancel, started, work = std::move(work)]() mutable {
          SideTaskResult result;
          result.id = id;
          result.kind = std::move(kind);
          result.label = std::move(label);
          ToolResult outcome = work(*cancel);
          result.output = std::move(outcome.output);
          result.status = outcome.status;
          result.error = outcome.error;
          result.duration_ms = ElapsedMs(started);
          return result;
        });
    jobs_.push_back(std::move(job));
    return id;
  }

  std::optional<SideTaskResult> Wait(int64_t id,
                                     std::chrono::milliseconds grace) {
    std::shared_ptr<Job> job = Find(id);
    if (!job || job->future.wait_for(grace) != std::future_status::ready) {
      return std::nullopt;
    }
    return Take(id);
  }

  std::vector<SideTaskResult> TakeCompleted() {
    std::vector<std::shared_ptr<Job>> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = jobs_.begin(); it != jobs_.end();) {
        if ((*it)->future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
          ready.push_back(*it);
          it = jobs_.erase(it);
        } else {
          ++it;
        }
      }
    }
    std::vector<SideTaskResult> results;
    for (auto& job : ready) results.push_back(job->future.get());
    return results;
  }

  bool WaitForOne(std::chrono::milliseconds timeout) const {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.empty()) return false;
        for (const auto& job : jobs_) {
          if (job->future.wait_for(std::chrono::milliseconds(0)) ==
              std::future_status::ready) {
            return true;
          }
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (true);
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
  }
  bool Empty() const { return Size() == 0; }
  bool Contains(int64_t id) const { return static_cast<bool>(Find(id)); }

  size_t Joinable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(
        std::count_if(jobs_.begin(), jobs_.end(),
                      [](const auto& job) { return job->join_before_final; }));
  }

  size_t CancelAll() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs.swap(jobs_);
    }
    for (auto& job : jobs) job->cancel->store(true);
    for (auto& job : jobs) {
      if (job->future.valid()) job->future.wait();
    }
    return jobs.size();
  }

 private:
  struct Job {
    int64_t id = 0;
    std::string kind, label;
    bool join_before_final = true;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::future<SideTaskResult> future;
  };

  std::shared_ptr<Job> Find(int64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& job : jobs_) {
      if (job->id == id) return job;
    }
    return {};
  }

  std::optional<SideTaskResult> Take(int64_t id) {
    std::shared_ptr<Job> job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = std::find_if(
          jobs_.begin(), jobs_.end(),
          [&](const auto& candidate) { return candidate->id == id; });
      if (it == jobs_.end() || (*it)->future.wait_for(std::chrono::milliseconds(
                                   0)) != std::future_status::ready) {
        return std::nullopt;
      }
      job = *it;
      jobs_.erase(it);
    }
    return job->future.get();
  }

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Job>> jobs_;
  int64_t next_id_ = 1;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PROCESS_H_
