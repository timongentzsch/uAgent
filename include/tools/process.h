// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PROCESS_H_
#define UAGENT_INCLUDE_TOOLS_PROCESS_H_
// Ownership and bounded registry for supervised shell process groups.

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uagent {

// The shell runner writes stdout+stderr to a bounded log. Longer commands keep
// running in a supervised process group until the harness joins them.
//
// Completed jobs are detected at model-step and interactive-loop boundaries.
// The latter can resume an idle agent with the completion event; no watcher
// thread or parallel process registry is needed.

struct BgJob {
  pid_t pid;
  std::string log, cmd;
  bool detached = false;
  std::string kind;
  std::optional<int> leader_status = std::nullopt;
};

class ProcessSupervisor {
 public:
  ProcessSupervisor() = default;
  ~ProcessSupervisor();
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  // The registry owns every tracked PID. Collection swaps the records out
  // before waitpid, signals, or filesystem I/O; live records are restored.
  // No external work runs while mutex_ is held.
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
  void Restore(BgJob job) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.push_back(std::move(job));
  }
  std::vector<BgJob> TakeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BgJob> jobs;
    jobs.swap(jobs_);
    return jobs;
  }
  size_t PendingCount() const {
    return CountIf([](const BgJob& j) { return !j.detached; });
  }
  size_t DetachedCount() const {
    return CountIf([](const BgJob& j) { return j.detached; });
  }
  size_t Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
  }
  size_t JoinableCount() const {
    return CountIf(
        [](const BgJob& job) { return !job.detached && job.kind == "task"; });
  }
  std::vector<pid_t> PendingPids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<pid_t> pids;
    for (const BgJob& job : jobs_) {
      if (!job.detached) pids.push_back(job.pid);
    }
    return pids;
  }
  std::optional<BgJob> Find(pid_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t index = IndexOf(pid);
    return index == jobs_.size() ? std::nullopt
                                 : std::optional<BgJob>(jobs_[index]);
  }
  std::optional<BgJob> Take(pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t index = IndexOf(pid);
    if (index == jobs_.size()) return std::nullopt;
    BgJob job = std::move(jobs_[index]);
    jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(index));
    return job;
  }
  std::vector<BgJob> Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
  }

 private:
  size_t IndexOf(pid_t pid) const {
    for (size_t i = 0; i < jobs_.size(); ++i) {
      if (jobs_[i].pid == pid) return i;
    }
    return jobs_.size();
  }

  template <class P>
  size_t CountIf(P predicate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), predicate));
  }
  mutable std::mutex mutex_;
  std::vector<BgJob> jobs_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PROCESS_H_
