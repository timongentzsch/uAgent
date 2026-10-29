// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_ACTIVITY_SUPERVISOR_H_
#define UAGENT_INCLUDE_AGENT_ACTIVITY_SUPERVISOR_H_
// Supervises a conversation's activities: commands, memory work and
// detached processes, from start to the result that rejoins the turn.

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
#include "include/core/output_buffer.h"
#include "include/core/sandbox_watch.h"
#include "include/core/thread_annotations.h"
#include "include/tools/tool.h"

namespace uagent {

enum class ActivityKind : uint8_t { kCommand, kMemory, kDetached };
enum class ActivityState : uint8_t {
  kStarting,
  kRunning,
  kExited,
  kDrained,
  kDelivered,
  kStopped,
};

std::string ActivityKindName(ActivityKind kind);
bool ActivityTerminal(ActivityState state);
struct ActivitySession {
  // Process identity, written once during registration and read afterwards.
  pid_t pid = -1;
  bool tty = false;

  // Closed by ActivitySupervisor's I/O thread as soon as the activity reaches
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

// Call while holding ActivitySession::mutex. Rejects illegal regressions and
// makes the process lifecycle's state graph explicit in one place.
bool TransitionActivityLocked(ActivitySession& session, ActivityState next)
    UAGENT_REQUIRES(session.mutex);

struct Activity {
  pid_t pid = -1;
  std::string log{}, cmd{};
  ActivityKind kind = ActivityKind::kCommand;
  int64_t id = 0;
  std::shared_ptr<ActivitySession> session{};
  std::string display_label{};
  std::string receipt_path{};
  std::string source_id{};
  // Put right when the activity ends, where the sandbox could not deny its
  // writes.
  std::shared_ptr<const SandboxWatch> watch{};
  int64_t started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  // Set where the activity is constructed rather than where it is registered:
  // the two differ by a spawn, and the status row is reporting how long the
  // child has been alive, not how long the supervisor has known about it.
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();

  bool Detached() const { return kind == ActivityKind::kDetached; }
};

inline int64_t ActivityId(const Activity& job) {
  return job.id > 0 ? job.id : static_cast<int64_t>(job.pid);
}

class ActivitySupervisor;

class ActivityReservation {
 public:
  ActivityReservation() = default;
  ~ActivityReservation();
  ActivityReservation(ActivityReservation&& other) noexcept;
  ActivityReservation& operator=(ActivityReservation&& other) noexcept;
  ActivityReservation(const ActivityReservation&) = delete;
  ActivityReservation& operator=(const ActivityReservation&) = delete;

  std::optional<int64_t> Register(Activity job);
  // The activity id this reservation will commit under, known before the
  // activity exists so its log can be named after it rather than after a
  // reusable pid.
  int64_t Id() const { return id_; }

 private:
  friend class ActivitySupervisor;
  ActivityReservation(ActivitySupervisor* supervisor, int64_t id)
      : supervisor_(supervisor), id_(id) {}
  void Reset();
  ActivitySupervisor* supervisor_ = nullptr;
  int64_t id_ = 0;
};

class ActivitySupervisor {
 public:
  ActivitySupervisor();
  ~ActivitySupervisor();
  ActivitySupervisor(const ActivitySupervisor&) = delete;
  ActivitySupervisor& operator=(const ActivitySupervisor&) = delete;

  // Takes ownership of output_fd, input_fd, and log_fd.
  void RegisterIo(const std::shared_ptr<ActivitySession>& session,
                  int output_fd, int input_fd, int log_fd, int64_t log_limit);
  std::optional<ActivityReservation> ReserveActivity(int64_t max_pending);
  std::optional<Activity> RemoveForeground(pid_t pid);
  std::optional<Activity> MoveForegroundToBackground(pid_t pid);
  bool WaitForForeground(size_t count,
                         std::chrono::steady_clock::time_point deadline) const;
  bool RequestForegroundBackground();

  bool TryAdd(Activity job, int64_t max_pending);
  size_t PendingCount() const;
  size_t Count() const;
  json ActivityViews() const;
  json InspectActivity(int64_t id) const;
  void SetOwner(std::string owner);
  std::string Owner() const;
  bool IsLive(int64_t id) const;
  std::optional<Activity> Find(int64_t id) const;
  // Retaining a terminal activity keeps inspection continuous while its result
  // is consumed exactly once. Removal and retention share the same lock.
  std::optional<Activity> Take(int64_t id, bool retain = false);
  std::vector<Activity> Snapshot() const;
  // What runs now: the background activities and those a tool call waits on.
  std::vector<Activity> Running() const;
  std::vector<Activity> TakeAllForShutdown();
  // Commands a stop ended, which report no completion: their rows take
  // their end from here (Agent::DrainActivities).
  void Stopped(EndedCommand ended);
  std::vector<EndedCommand> TakeStopped();

  uint64_t Generation() const;
  void Wake();
  // Mirror supervisor state changes to one application-owned nonblocking pipe.
  // The caller must clear the descriptor before closing it.
  void SetNotifyFd(int fd);
  // Whether a finished activity wakes the session that started it: a
  // conversation runtime takes up a result as a turn; a headless run's answer
  // is final.
  bool WakesSession() const;
  void WaitForChange(uint64_t generation) const;
  bool WaitForChange(uint64_t generation,
                     std::chrono::steady_clock::time_point deadline) const;

 private:
  friend class ActivityReservation;
  std::optional<int64_t> CommitReservation(Activity job);
  void ReleaseReservation();
  std::optional<Activity> TakeForegroundLocked(pid_t pid)
      UAGENT_REQUIRES(mutex_);
  void AssignId(Activity& job) UAGENT_REQUIRES(mutex_);
  size_t IndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  size_t RetainedIndexOfLocked(int64_t id) const UAGENT_REQUIRES(mutex_);
  void StartIoLocked() UAGENT_REQUIRES(mutex_);
  void IoLoop(const std::stop_token& stop);
  void NotifyLocked(bool wake_io = true) UAGENT_REQUIRES(mutex_);
  void PruneRetainedLocked() UAGENT_REQUIRES(mutex_);

  mutable std::mutex mutex_;
  mutable std::condition_variable event_;
  std::vector<Activity> activities_ UAGENT_GUARDED_BY(mutex_);
  std::vector<Activity> foreground_ UAGENT_GUARDED_BY(mutex_);
  std::vector<Activity> retained_ UAGENT_GUARDED_BY(mutex_);
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
  std::vector<EndedCommand> stopped_ UAGENT_GUARDED_BY(mutex_);
  std::string owner_ UAGENT_GUARDED_BY(mutex_);
  int64_t next_id_ UAGENT_GUARDED_BY(mutex_) = int64_t{1} << 30;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_ACTIVITY_SUPERVISOR_H_
