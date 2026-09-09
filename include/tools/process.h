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
#include "include/core/json.h"
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

ActivityKind ParseActivityKind(const std::string& kind);
std::string ActivityKindName(ActivityKind kind);
bool ActivityTerminal(ActivityState state);
// Call while holding ActivitySession::mutex. Rejects illegal regressions and
// makes the process lifecycle's state graph explicit in one place.
bool TransitionActivityLocked(struct ActivitySession& session,
                              ActivityState next);

struct ActivitySession {
  // Process identity, written once during registration and read afterwards.
  pid_t pid = -1;
  bool tty = false;

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
  HeadTailBuffer pending_output UAGENT_GUARDED_BY(mutex);
  HeadTailBuffer transcript UAGENT_GUARDED_BY(mutex);
  std::string until_window UAGENT_GUARDED_BY(mutex);
  std::chrono::steady_clock::time_point last_used UAGENT_GUARDED_BY(mutex) =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point exited_at UAGENT_GUARDED_BY(mutex){};
};

struct BgJob {
  BgJob(pid_t process_pid, std::string log_path, std::string command,
        bool is_detached = false, const std::string& job_kind = {},
        int64_t activity_id = 0,
        std::shared_ptr<ActivitySession> activity = nullptr,
        std::string label = {}, std::string receipt = {},
        std::string source = {}, std::vector<std::string> notes = {},
        json metadata = json::object());

  pid_t pid;
  std::string log, cmd;
  bool detached = false;
  ActivityKind kind = ActivityKind::kCommand;
  int64_t id = 0;
  std::shared_ptr<ActivitySession> session;
  std::string display_label;
  std::string receipt_path;
  std::string source_id;
  std::vector<std::string> completion_notes;
  json metadata;
  int64_t started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  // Set where the job is constructed rather than where it is registered: the
  // two differ by a spawn, and the status row is reporting how long the child
  // has been alive, not how long the supervisor has known about it.
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
};

inline int64_t ActivityId(const BgJob& job) {
  return job.id > 0 ? job.id : static_cast<int64_t>(job.pid);
}

// A running delegated child, reduced to what a one-row status display can
// carry. `tail` is the child's newest headless-progress line and empty when its
// newest output is anything else, so it never shows the answer envelope. It is
// terminal output from another process -- the renderer must sanitize it.
struct SubagentView {
  int64_t id = 0;
  std::string source_id;
  std::string label;
  std::string tail;
  std::chrono::steady_clock::duration elapsed{};
};

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
  // The activity id this reservation will commit under, known before the job
  // exists so its log can be named after it rather than after a reusable pid.
  int64_t Id() const { return id_; }

 private:
  friend class ProcessSupervisor;
  ActivityReservation(ProcessSupervisor* supervisor, bool subagent, int64_t id)
      : supervisor_(supervisor), subagent_(subagent), id_(id) {}
  void Reset();
  ProcessSupervisor* supervisor_ = nullptr;
  bool subagent_ = false;
  int64_t id_ = 0;
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
  // max_subagents caps delegated children only; 0 means no per-kind cap.
  std::optional<ActivityReservation> ReserveActivity(int64_t max_pending,
                                                     int64_t max_subagents = 0);
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
  size_t Count(ActivityKind kind) const;
  size_t JoinableCount() const;
  // Background delegated children, newest last. Takes no lock a tool holds, so
  // a status repaint never waits on one.
  std::vector<SubagentView> SubagentViews() const;
  json ActivityViews() const;
  json InspectActivity(int64_t id) const;
  void SetOwner(std::string owner);
  std::string Owner() const;
  bool IsLive(int64_t id) const;
  std::optional<BgJob> Find(int64_t id) const;
  // Retaining a terminal activity keeps inspection continuous while its result
  // is consumed exactly once. Removal and retention share the same lock.
  std::optional<BgJob> Take(int64_t id, bool retain = false);
  std::vector<BgJob> Snapshot() const;
  std::vector<BgJob> TakeAllForShutdown();

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
  std::optional<int64_t> CommitReservation(BgJob job, bool subagent);
  void ReleaseReservation(bool subagent);
  std::optional<BgJob> TakeForegroundLocked(pid_t pid) UAGENT_REQUIRES(mutex_);
  void AssignId(BgJob& job) UAGENT_REQUIRES(mutex_);
  size_t IndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  size_t RetainedIndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  void StartIoLocked() UAGENT_REQUIRES(mutex_);
  void IoLoop(const std::stop_token& stop);
  void NotifyLocked(bool wake_io = true) UAGENT_REQUIRES(mutex_);
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
  int64_t subagent_reservations_ UAGENT_GUARDED_BY(mutex_) = 0;
  uint64_t generation_ UAGENT_GUARDED_BY(mutex_) = 0;
  std::string owner_ UAGENT_GUARDED_BY(mutex_);
  int64_t next_id_ UAGENT_GUARDED_BY(mutex_) = int64_t{1} << 30;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PROCESS_H_
