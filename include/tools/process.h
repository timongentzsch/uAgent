// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_PROCESS_H_
#define UAGENT_INCLUDE_TOOLS_PROCESS_H_

#include <sys/types.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "include/core/fd.h"
#include "include/core/thread_annotations.h"
#include "include/tools/output_buffer.h"

namespace uagent {

// A subagent activity is a delegated child whose result joins the parent
// conversation when it finishes.
enum class ActivityKind : uint8_t { kCommand, kSubagent, kMemory, kDetached };
enum class ActivityState : uint8_t {
  kStarting,
  kRunning,
  kExited,
  kDrained,
  kDelivered,
  kStopped,
};

ActivityKind ParseActivityKind(const std::string& kind, bool detached = false);
std::string ActivityKindName(ActivityKind kind);
bool ActivityTerminal(ActivityState state);

struct ActivitySession {
  // Identity, written once during registration and read freely afterwards.
  int64_t id = 0;
  pid_t pid = -1;
  ActivityKind kind = ActivityKind::kCommand;
  bool tty = false;
  std::string log;
  std::string cmd;

  // Closed by ProcessSupervisor's I/O thread as soon as the activity reaches
  // a terminal state. Tools duplicate input_fd before writing, because this
  // owner may close underneath them.
  Fd output_fd;
  Fd input_fd;
  Fd log_fd;
  int64_t log_limit = 0;
  int64_t logged_bytes = 0;

  // `mutex` covers the mutable state below. `interaction` is coarser, taken
  // first when both are needed, and serialises whole tool interactions
  // (write stdin, then collect the reply).
  mutable std::mutex interaction;
  mutable std::mutex mutex;
  ActivityState state UAGENT_GUARDED_BY(mutex) = ActivityState::kStarting;
  std::optional<int> wait_status UAGENT_GUARDED_BY(mutex);
  bool background_requested UAGENT_GUARDED_BY(mutex) = false;
  bool stop_requested UAGENT_GUARDED_BY(mutex) = false;
  bool output_eof UAGENT_GUARDED_BY(mutex) = false;
  bool delivered UAGENT_GUARDED_BY(mutex) = false;
  HeadTailBuffer pending_output UAGENT_GUARDED_BY(mutex);
  HeadTailBuffer transcript UAGENT_GUARDED_BY(mutex);
  std::string until_window UAGENT_GUARDED_BY(mutex);
  std::chrono::steady_clock::time_point last_used UAGENT_GUARDED_BY(mutex) =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point exited_at UAGENT_GUARDED_BY(mutex){};
};

struct BgJob {
  BgJob(pid_t process_pid, std::string log_path, std::string command,
        bool is_detached = false, std::string job_kind = {},
        int64_t activity_id = 0,
        std::shared_ptr<ActivitySession> activity = nullptr,
        std::string label = {}, std::string receipt = {},
        std::string source = {});

  pid_t pid;
  std::string log, cmd;
  bool detached = false;
  std::string kind;
  int64_t id = 0;
  std::shared_ptr<ActivitySession> session;
  std::string display_label;
  std::string receipt_path;
  std::string source_id;
};

inline int64_t ActivityId(const BgJob& job) {
  return job.id > 0 ? job.id : static_cast<int64_t>(job.pid);
}

class ProcessSupervisor;

class ActivityReservation {
 public:
  ActivityReservation() = default;
  ~ActivityReservation();
  ActivityReservation(ActivityReservation&& other) noexcept;
  ActivityReservation& operator=(ActivityReservation&& other) noexcept;
  ActivityReservation(const ActivityReservation&) = delete;
  ActivityReservation& operator=(const ActivityReservation&) = delete;

  std::optional<int64_t> Register(BgJob job);

 private:
  friend class ProcessSupervisor;
  explicit ActivityReservation(ProcessSupervisor* supervisor)
      : supervisor_(supervisor) {}
  void Reset();
  ProcessSupervisor* supervisor_ = nullptr;
};

class ProcessSupervisor {
 public:
  ProcessSupervisor();
  ~ProcessSupervisor();
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  // Takes ownership of output_fd, input_fd, and log_fd.
  void RegisterIo(const std::shared_ptr<ActivitySession>& session,
                  int output_fd, int input_fd, int log_fd, int64_t log_limit);
  std::optional<ActivityReservation> ReserveActivity(int64_t max_pending);
  std::optional<BgJob> RemoveForeground(pid_t pid);
  std::optional<BgJob> MoveForegroundToBackground(pid_t pid);
  size_t ForegroundCount() const;
  bool WaitForForeground(size_t count,
                         std::chrono::steady_clock::time_point deadline) const;
  bool RequestForegroundBackground();

  bool TryAdd(BgJob job, int64_t max_pending);
  size_t PendingCount() const;
  size_t DetachedCount() const;
  size_t Count() const;
  size_t JoinableCount() const;
  bool IsLive(int64_t id) const;
  std::optional<BgJob> Find(int64_t id) const;
  std::optional<BgJob> Take(int64_t id);
  std::vector<BgJob> Snapshot() const;
  std::vector<BgJob> TakeAllForShutdown();
  void Retain(BgJob job);

  uint64_t Generation() const;
  void Wake();
  // Mirror supervisor state changes to one application-owned nonblocking pipe.
  // The caller must clear the descriptor before closing it.
  void SetNotifyFd(int fd);
  void WaitForChange(uint64_t generation) const;
  bool WaitForChange(uint64_t generation,
                     std::chrono::steady_clock::time_point deadline) const;

 private:
  friend class ActivityReservation;
  std::optional<int64_t> CommitReservation(BgJob job);
  void ReleaseReservation();
  std::optional<BgJob> TakeForegroundLocked(pid_t pid) UAGENT_REQUIRES(mutex_);
  void AssignId(BgJob& job) UAGENT_REQUIRES(mutex_);
  size_t IndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  size_t RetainedIndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  void StartIoLocked() UAGENT_REQUIRES(mutex_);
  void IoLoop(const std::stop_token& stop);
  void NotifyLocked() UAGENT_REQUIRES(mutex_);
  void PruneRetainedLocked() UAGENT_REQUIRES(mutex_);

  mutable std::mutex mutex_;
  mutable std::condition_variable event_;
  std::vector<BgJob> jobs_ UAGENT_GUARDED_BY(mutex_);
  std::vector<BgJob> foreground_ UAGENT_GUARDED_BY(mutex_);
  std::vector<BgJob> retained_ UAGENT_GUARDED_BY(mutex_);
  std::vector<std::shared_ptr<ActivitySession>> io_sessions_
      UAGENT_GUARDED_BY(mutex_);
  // A stop_callback writes the wake pipe, so request_stop() both flags the
  // loop and unblocks its poll().
  std::jthread io_thread_;
  Fd wake_read_;
  Fd wake_write_;
  // Borrowed. Written under the lock; the wake path tolerates a stale read.
  int notify_fd_ = -1;
  bool child_wake_registered_ = false;
  bool stopping_ UAGENT_GUARDED_BY(mutex_) = false;
  int64_t reservations_ UAGENT_GUARDED_BY(mutex_) = 0;
  uint64_t generation_ UAGENT_GUARDED_BY(mutex_) = 0;
  int64_t next_id_ UAGENT_GUARDED_BY(mutex_) = int64_t{1} << 30;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PROCESS_H_
