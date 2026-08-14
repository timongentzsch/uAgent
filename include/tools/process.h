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
#include <string>
#include <thread>
#include <vector>

#include "include/tools/output_buffer.h"

namespace uagent {

enum class ActivityKind : uint8_t { kCommand, kTask, kMemory, kDetached };
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
  int64_t id = 0;
  pid_t pid = -1;
  ActivityKind kind = ActivityKind::kCommand;
  bool tty = false;
  std::string log;
  std::string cmd;

  // Process I/O descriptors are owned and closed only by ProcessSupervisor's
  // I/O thread. Tools duplicate input_fd before writing.
  int output_fd = -1;
  int input_fd = -1;
  int log_fd = -1;
  int64_t log_limit = 0;
  int64_t logged_bytes = 0;

  mutable std::mutex mutex;
  mutable std::mutex interaction;
  ActivityState state = ActivityState::kStarting;
  std::optional<int> wait_status;
  bool background_requested = false;
  bool stop_requested = false;
  bool output_eof = false;
  bool delivered = false;
  HeadTailBuffer pending_output;
  HeadTailBuffer transcript;
  std::string until_window;
  std::chrono::steady_clock::time_point last_used =
      std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point exited_at{};
};

struct BgJob {
  BgJob(pid_t process_pid, std::string log_path, std::string command,
        bool is_detached = false, std::string job_kind = {},
        std::optional<int> status = std::nullopt, int64_t activity_id = 0,
        std::shared_ptr<ActivitySession> activity = nullptr);

  pid_t pid;
  std::string log, cmd;
  bool detached = false;
  std::string kind;
  std::optional<int> leader_status;
  int64_t id = 0;
  std::shared_ptr<ActivitySession> session;
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
  bool WaitForChange(uint64_t generation,
                     std::chrono::steady_clock::time_point deadline) const;

 private:
  friend class ActivityReservation;
  std::optional<int64_t> CommitReservation(BgJob job);
  void ReleaseReservation();
  void AssignId(BgJob& job);
  size_t IndexOfLocked(int64_t id) const;
  size_t RetainedIndexOfLocked(int64_t id) const;
  void StartIoLocked();
  void IoLoop();
  void NotifyLocked();
  void PruneRetainedLocked();

  mutable std::mutex mutex_;
  mutable std::condition_variable event_;
  std::vector<BgJob> jobs_;
  std::vector<BgJob> foreground_;
  std::vector<BgJob> retained_;
  std::vector<std::shared_ptr<ActivitySession>> io_sessions_;
  std::thread io_thread_;
  int wake_read_ = -1;
  int wake_write_ = -1;
  bool stopping_ = false;
  int64_t reservations_ = 0;
  uint64_t generation_ = 0;
  int64_t next_id_ = int64_t{1} << 30;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_PROCESS_H_
