// Copyright 2026 Timon Gentzsch

#include "include/agent/activity_supervisor.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/core/events.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/time.h"

namespace uagent {
namespace {

constexpr size_t kRetainedActivities = 16;
constexpr auto kTrailingOutputGrace = std::chrono::milliseconds(100);

// Release the descriptors when the activity goes terminal, not when the
// session object is finally dropped.
void CloseSessionIo(const std::shared_ptr<ActivitySession>& session) {
  std::lock_guard<std::mutex> lock(session->mutex);
  session->output_fd.Reset();
  session->input_fd.Reset();
  session->log_fd.Reset();
}

}  // namespace

std::string ActivityKindName(ActivityKind kind) {
  switch (kind) {
    case ActivityKind::kSubagent:
      return "subagent";
    case ActivityKind::kMemory:
      return "memory";
    case ActivityKind::kDetached:
      return "detached";
    case ActivityKind::kCommand:
      return "command";
  }
  return "";
}

bool ActivityTerminal(ActivityState state) {
  return state == ActivityState::kDrained ||
         state == ActivityState::kDelivered || state == ActivityState::kStopped;
}

bool TransitionActivityLocked(ActivitySession& session, ActivityState next) {
  const ActivityState current = session.state;
  if (current == next) return true;
  bool allowed = false;
  switch (current) {
    case ActivityState::kStarting:
      allowed = next == ActivityState::kRunning ||
                next == ActivityState::kExited ||
                next == ActivityState::kStopped;
      break;
    case ActivityState::kRunning:
      allowed =
          next == ActivityState::kExited || next == ActivityState::kStopped;
      break;
    case ActivityState::kExited:
      allowed =
          next == ActivityState::kDrained || next == ActivityState::kStopped;
      break;
    case ActivityState::kDrained:
      allowed = next == ActivityState::kDelivered;
      break;
    case ActivityState::kDelivered:
    case ActivityState::kStopped:
      break;
  }
  if (allowed) session.state = next;
  return allowed;
}

ActivityReservation::~ActivityReservation() { Reset(); }

// Every member moves: the supervisor alone told the destination which counter
// to give back and, since the reservation names the log, which id to commit
// under. ReserveActivity hands its result through an optional, so this runs on
// the way out of every reservation.
ActivityReservation::ActivityReservation(ActivityReservation&& other) noexcept
    : supervisor_(std::exchange(other.supervisor_, nullptr)),
      subagent_(other.subagent_),
      id_(other.id_) {}

ActivityReservation& ActivityReservation::operator=(
    ActivityReservation&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  supervisor_ = std::exchange(other.supervisor_, nullptr);
  subagent_ = other.subagent_;
  id_ = other.id_;
  return *this;
}

void ActivityReservation::Reset() {
  if (!supervisor_) return;
  supervisor_->ReleaseReservation(subagent_);
  supervisor_ = nullptr;
}

std::optional<int64_t> ActivityReservation::Register(Activity job) {
  if (!supervisor_) return std::nullopt;
  ActivitySupervisor* supervisor = supervisor_;
  supervisor_ = nullptr;
  return supervisor->CommitReservation(std::move(job), subagent_);
}

ActivitySupervisor::ActivitySupervisor() {
  InitializeSignalNotifications();
  InstallSigchldHandler();
}

ActivitySupervisor::~ActivitySupervisor() {
  ActivityShutdownAll(*this);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    NotifyLocked();
  }
  // The stop callback wakes the poll, so this is the whole handshake;
  // ~jthread would join but not unblock.
  io_thread_.request_stop();
  if (io_thread_.joinable()) io_thread_.join();
  if (child_wake_registered_) {
    (void)RegisterChildWakeFd(wake_write_.Get(), /*add=*/false);
  }
}

void ActivitySupervisor::StartIoLocked() {
  if (io_thread_.joinable()) return;
  int wake[2] = {-1, -1};
  if (OpenNonblockingPipe(wake)) {
    wake_read_.Reset(wake[0]);
    wake_write_.Reset(wake[1]);
  }
  child_wake_registered_ = RegisterChildWakeFd(wake_write_.Get(), /*add=*/true);
  io_thread_ = std::jthread([this](const std::stop_token& stop) {
    std::stop_callback wake_on_stop(
        stop, [this] { WakeDescriptor(wake_write_.Get()); });
    IoLoop(stop);
  });
}

void ActivitySupervisor::RegisterIo(
    const std::shared_ptr<ActivitySession>& session, int output_fd,
    int input_fd, int log_fd, int64_t log_limit) {
  if (!session) return;
  if (output_fd >= 0) {
    fcntl(output_fd, F_SETFL, fcntl(output_fd, F_GETFL) | O_NONBLOCK);
    fcntl(output_fd, F_SETFD, FD_CLOEXEC);
  }
  if (input_fd >= 0) fcntl(input_fd, F_SETFD, FD_CLOEXEC);
  {
    std::lock_guard<std::mutex> state_lock(session->mutex);
    session->output_fd.Reset(output_fd);
    session->input_fd.Reset(input_fd);
    session->log_fd.Reset(log_fd);
    session->log_limit = log_limit;
    (void)TransitionActivityLocked(*session, ActivityState::kRunning);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_sessions_.push_back(session);
    StartIoLocked();
    NotifyLocked();
    WakeDescriptor(wake_write_.Get());
  }
}

void ActivitySupervisor::AssignId(Activity& job) {
  if (job.id <= 0) {
    job.id = job.Detached() ? static_cast<int64_t>(job.pid) : next_id_++;
  }
  if (job.session) {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->pid = job.pid;
    job.session->last_used = std::chrono::steady_clock::now();
  }
}

std::optional<ActivityReservation> ActivitySupervisor::ReserveActivity(
    int64_t max_pending, int64_t max_subagents) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t live =
      foreground_.size() +
      static_cast<size_t>(std::count_if(
          activities_.begin(), activities_.end(),
          [](const Activity& current) { return !current.Detached(); }));
  if (static_cast<int64_t>(live) + reservations_ >= max_pending) {
    return std::nullopt;
  }
  // A child competes against other children, never against the parent's own
  // commands, which would refuse delegation the moment the parent was busy.
  bool subagent = max_subagents > 0;
  if (subagent) {
    auto child = [](const Activity& current) {
      return !current.Detached() && current.kind == ActivityKind::kSubagent;
    };
    int64_t children = static_cast<int64_t>(std::count_if(
                           foreground_.begin(), foreground_.end(), child)) +
                       static_cast<int64_t>(std::count_if(
                           activities_.begin(), activities_.end(), child));
    if (children + subagent_reservations_ >= max_subagents) return std::nullopt;
    ++subagent_reservations_;
  }
  ++reservations_;
  NotifyLocked();
  // Spent whether or not the reservation commits: ids are opaque, so a gap
  // costs nothing and pre-allocation is what lets the caller name its log.
  return ActivityReservation(this, subagent, next_id_++);
}

std::optional<int64_t> ActivitySupervisor::CommitReservation(Activity job,
                                                             bool subagent) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservations_ <= 0) return std::nullopt;
  --reservations_;
  if (subagent && subagent_reservations_ > 0) --subagent_reservations_;
  if (stopping_) {
    NotifyLocked();
    return std::nullopt;
  }
  AssignId(job);
  foreground_.push_back(std::move(job));
  NotifyLocked();
  return ActivityId(foreground_.back());
}

void ActivitySupervisor::ReleaseReservation(bool subagent) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservations_ > 0) --reservations_;
  if (subagent && subagent_reservations_ > 0) --subagent_reservations_;
  NotifyLocked();
}

std::optional<Activity> ActivitySupervisor::RemoveForeground(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<Activity> job = TakeForegroundLocked(pid);
  if (job) NotifyLocked();
  return job;
}

std::optional<Activity> ActivitySupervisor::MoveForegroundToBackground(
    pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<Activity> job = TakeForegroundLocked(pid);
  if (!job) return std::nullopt;
  activities_.push_back(std::move(*job));
  NotifyLocked();
  return activities_.back();
}

std::optional<Activity> ActivitySupervisor::TakeForegroundLocked(pid_t pid) {
  auto found =
      std::find_if(foreground_.begin(), foreground_.end(),
                   [pid](const Activity& job) { return job.pid == pid; });
  if (found == foreground_.end()) return std::nullopt;
  Activity job = std::move(*found);
  foreground_.erase(found);
  return job;
}

bool ActivitySupervisor::WaitForForeground(
    size_t count, std::chrono::steady_clock::time_point deadline) const {
  UniqueLock lock(mutex_);
  return event_.wait_until(lock, deadline, [&]() UAGENT_REQUIRES(mutex_) {
    return foreground_.size() >= count;
  });
}

bool ActivitySupervisor::RequestForegroundBackground() {
  std::vector<std::shared_ptr<ActivitySession>> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (foreground_.empty()) return false;
    for (const Activity& job : foreground_) {
      if (job.session) sessions.push_back(job.session);
    }
    NotifyLocked();
  }
  for (const auto& session : sessions) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->background_requested = true;
  }
  Wake();
  return true;
}

bool ActivitySupervisor::TryAdd(Activity job, int64_t max_pending) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto pending = [](const Activity& current) { return !current.Detached(); };
  if (job.Detached()) {
    // A detached terminal outlives the turn that started it, so this is the
    // only bound on how many one session can leave running.
    int64_t detached = static_cast<int64_t>(std::count_if(
        activities_.begin(), activities_.end(),
        [](const Activity& current) { return current.Detached(); }));
    if (detached >= max_pending) return false;
  } else {
    size_t count = foreground_.size() +
                   static_cast<size_t>(std::count_if(
                       activities_.begin(), activities_.end(), pending));
    if (static_cast<int64_t>(count) + reservations_ >= max_pending) {
      return false;
    }
  }
  AssignId(job);
  activities_.push_back(std::move(job));
  StartIoLocked();
  NotifyLocked();
  return true;
}

size_t ActivitySupervisor::IndexOfLocked(int64_t id) const {
  for (size_t i = 0; i < activities_.size(); ++i) {
    if (ActivityId(activities_[i]) == id) return i;
  }
  return activities_.size();
}

size_t ActivitySupervisor::RetainedIndexOfLocked(int64_t id) const {
  for (size_t i = 0; i < retained_.size(); ++i) {
    if (ActivityId(retained_[i]) == id) return i;
  }
  return retained_.size();
}

size_t ActivitySupervisor::PendingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(
      std::count_if(activities_.begin(), activities_.end(),
                    [](const Activity& job) { return !job.Detached(); }));
}

size_t ActivitySupervisor::Count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activities_.size();
}

std::vector<SubagentView> ActivitySupervisor::SubagentViews() const {
  std::vector<SubagentView> views;
  const int64_t now = NowMillis();
  for (const json& row : ActivityViews()) {
    const std::string status = JsonValue(row, "status", "");
    if (JsonValue(row, "kind", "") != "subagent" ||
        (status != "running" && status != "starting" &&
         status != "finishing")) {
      continue;
    }
    views.push_back(
        {JsonValue(row, "id", int64_t{0}), JsonValue(row, "agent_id", ""),
         JsonValue(row, "model", JsonValue(row, "label", "")),
         JsonValue(row, "progress", ""),
         std::chrono::milliseconds(
             std::max(int64_t{0}, now - JsonValue(row, "started_ms", now)))});
  }
  return views;
}

void ActivitySupervisor::SetOwner(std::string owner) {
  std::lock_guard lock(mutex_);
  owner_ = std::move(owner);
}

std::string ActivitySupervisor::Owner() const {
  std::lock_guard lock(mutex_);
  return owner_;
}

json ActivitySupervisor::ActivityViews() const {
  // One copy under one lock: a job retained meanwhile is not listed twice.
  std::vector<Activity> jobs;
  size_t background = 0;
  size_t foreground = 0;
  {
    std::lock_guard lock(mutex_);
    jobs = activities_;
    background = activities_.size();
    foreground = foreground_.size();
    jobs.insert(jobs.end(), foreground_.begin(), foreground_.end());
    jobs.insert(jobs.end(), retained_.begin(), retained_.end());
  }
  json rows = json::array();
  for (size_t index = 0; index < jobs.size(); ++index) {
    const Activity& job = jobs[index];
    if (job.kind == ActivityKind::kMemory) continue;
    json row = job.metadata;
    row["id"] = ActivityId(job);
    // The turn is waiting on it, as opposed to running behind it.
    row["foreground"] = index >= background && index < background + foreground;
    row["agent_id"] = job.source_id;
    row["detached"] = job.Detached();
    row["kind"] = ActivityKindName(job.kind);
    row["label"] =
        JsonValue(row, "label",
                  job.display_label.empty()
                      ? Utf8Trunc(FirstLine(job.cmd), kStatusLabelBytes)
                      : job.display_label);
    // The modal shows the full command scrollable on top. Rows only carry
    // the 160-char label, and inspect fails once a finished job leaves the
    // retained set, so the command travels here (bounded like transcript
    // blocks) instead of degrading to the abbreviated label.
    if (!job.cmd.empty()) {
      row["command"] = Utf8Trunc(job.cmd, kActivityCommandBytes);
    }
    row["started_ms"] = job.started_ms;
    row["status"] = "running";
    if (job.session) {
      std::lock_guard lock(job.session->mutex);
      std::string progress = job.session->transcript.TailLine();
      if (job.kind != ActivityKind::kSubagent ||
          progress.starts_with(kHeadlessProgressPrefix)) {
        if (job.kind == ActivityKind::kSubagent) {
          progress.erase(
              0, std::char_traits<char>::length(kHeadlessProgressPrefix));
        }
        row["progress"] = Utf8Trunc(progress, 240);
      }
      if (job.session->stop_requested) row["status"] = "stopping";
      if (job.session->state == ActivityState::kStopped) {
        row["status"] = "stopped";
      }
      if (job.session->wait_status && !ActivityTerminal(job.session->state)) {
        row["status"] = "finishing";
      }
      if (ActivityTerminal(job.session->state) && job.session->wait_status) {
        int status = *job.session->wait_status;
        row["status"] = job.session->stop_requested ? "stopped"
                        : ExitedOk(status)          ? "completed"
                                                    : "failed";
        row["exit_code"] = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      if (job.session->wait_status) {
        row["duration_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                job.session->exited_at - job.started)
                .count();
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

json ActivitySupervisor::InspectActivity(int64_t id) const {
  auto job = Find(id);
  if (!job) return {{"error", "activity unavailable in this session"}};
  json result = {
      {"id", id}, {"agent_id", job->source_id}, {"command", job->cmd}};
  // Inspection never takes the consuming tool's interaction lock or output.
  if (job->session) {
    std::lock_guard lock(job->session->mutex);
    result["output"] = job->session->transcript.Snapshot();
  } else {
    result["output"] = ReadLogTail(job->log, static_cast<int64_t>(KiB(64)));
  }
  return result;
}

size_t ActivitySupervisor::JoinableCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(
      activities_.begin(), activities_.end(), [](const Activity& job) {
        return !job.Detached() && job.session &&
               job.kind == ActivityKind::kSubagent;
      }));
}

bool ActivitySupervisor::IsLive(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IndexOfLocked(id) != activities_.size();
}

std::optional<Activity> ActivitySupervisor::Find(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = IndexOfLocked(id);
  if (index != activities_.size()) return activities_[index];
  for (const Activity& job : foreground_) {
    if (ActivityId(job) == id) return job;
  }
  index = RetainedIndexOfLocked(id);
  return index == retained_.size() ? std::nullopt
                                   : std::optional<Activity>(retained_[index]);
}

std::optional<Activity> ActivitySupervisor::Take(int64_t id, bool retain) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = IndexOfLocked(id);
  if (index == activities_.size()) return std::nullopt;
  const auto& session = activities_[index].session;
  retain = retain && session && !activities_[index].Detached();
  if (retain) {
    std::lock_guard state_lock(session->mutex);
    if (session->state != ActivityState::kStopped &&
        !TransitionActivityLocked(*session, ActivityState::kDelivered)) {
      return std::nullopt;
    }
    session->last_used = std::chrono::steady_clock::now();
  }
  Activity job = std::move(activities_[index]);
  activities_.erase(activities_.begin() + static_cast<std::ptrdiff_t>(index));
  if (retain) {
    retained_.push_back(job);
    PruneRetainedLocked();
  }
  NotifyLocked();
  return job;
}

std::vector<Activity> ActivitySupervisor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return activities_;
}

std::vector<Activity> ActivitySupervisor::Running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Activity> jobs = activities_;
  jobs.insert(jobs.end(), foreground_.begin(), foreground_.end());
  return jobs;
}

std::vector<Activity> ActivitySupervisor::TakeAllForShutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Activity> jobs;
  jobs.swap(activities_);
  for (Activity& job : foreground_) jobs.push_back(std::move(job));
  foreground_.clear();
  NotifyLocked();
  return jobs;
}

void ActivitySupervisor::PruneRetainedLocked() {
  while (retained_.size() > kRetainedActivities) {
    auto oldest = retained_.end();
    for (auto it = retained_.begin(); it != retained_.end(); ++it) {
      if (!it->session || !it->session->interaction.try_lock()) continue;
      it->session->interaction.unlock();
      std::chrono::steady_clock::time_point used;
      {
        std::lock_guard<std::mutex> state_lock(it->session->mutex);
        used = it->session->last_used;
      }
      if (oldest == retained_.end()) {
        oldest = it;
      } else {
        std::lock_guard<std::mutex> state_lock(oldest->session->mutex);
        if (used < oldest->session->last_used) oldest = it;
      }
    }
    if (oldest == retained_.end()) break;
    retained_.erase(oldest);
  }
}

uint64_t ActivitySupervisor::Generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

void ActivitySupervisor::NotifyLocked(bool wake_io) {
  ++generation_;
  event_.notify_all();
  WakeDescriptor(notify_fd_);
  if (wake_io) WakeDescriptor(wake_write_.Get());
}

void ActivitySupervisor::SetNotifyFd(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  notify_fd_ = fd;
  WakeDescriptor(notify_fd_);
}

bool ActivitySupervisor::WakesSession() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return notify_fd_ >= 0;
}

void ActivitySupervisor::Wake() {
  int wake_fd = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    NotifyLocked();
    wake_fd = wake_write_.Get();
  }
  WakeDescriptor(wake_fd);
}

void ActivitySupervisor::WaitForChange(uint64_t generation) const {
  UniqueLock lock(mutex_);
  event_.wait(lock, [&]() UAGENT_REQUIRES(mutex_) {
    return generation_ != generation;
  });
}

bool ActivitySupervisor::WaitForChange(
    uint64_t generation, std::chrono::steady_clock::time_point deadline) const {
  UniqueLock lock(mutex_);
  return event_.wait_until(lock, deadline, [&]() UAGENT_REQUIRES(mutex_) {
    return generation_ != generation;
  });
}

void ActivitySupervisor::IoLoop(const std::stop_token& stop) {
  json previous = json::array();
  while (!stop.stop_requested()) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions = io_sessions_;
    }
    std::vector<pollfd> poll_fds;
    poll_fds.reserve(sessions.size() + 1);
    poll_fds.push_back({wake_read_.Get(), POLLIN, 0});
    for (const auto& session : sessions) {
      int fd = -1;
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        fd = session->output_fd.Get();
      }
      poll_fds.push_back(
          {fd, static_cast<int16_t>(POLLIN | POLLHUP | POLLERR), 0});
    }
    int timeout_ms = child_wake_registered_ ? -1 : 1000;
    for (const auto& session : sessions) {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (!session->wait_status || session->output_eof ||
          ActivityTerminal(session->state)) {
        continue;
      }
      int candidate = PollTimeoutMs(session->exited_at + kTrailingOutputGrace);
      timeout_ms = timeout_ms < 0 ? candidate : std::min(timeout_ms, candidate);
    }
    int ready =
        poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout_ms);
    if (ready < 0 && errno != EINTR) continue;
    if (!poll_fds.empty() && (poll_fds[0].revents & POLLIN)) {
      DrainDescriptor(wake_read_.Get());
      std::lock_guard<std::mutex> lock(mutex_);
      NotifyLocked(false);
    }

    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sessions.size(); ++i) {
      const auto& session = sessions[i];
      bool notify = false;
      int16_t events = poll_fds[i + 1].revents;
      if (events & (POLLIN | POLLHUP | POLLERR)) {
        std::array<char, 8192> bytes{};
        for (;;) {
          ssize_t count = read(poll_fds[i + 1].fd, bytes.data(), bytes.size());
          if (count < 0 && errno == EINTR) continue;
          if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          if (count <= 0) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->output_eof = true;
            notify = true;
            break;
          }
          std::string_view chunk(bytes.data(), static_cast<size_t>(count));
          int log_fd = -1;
          size_t keep = 0;
          {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (session->state == ActivityState::kStarting) {
              (void)TransitionActivityLocked(*session, ActivityState::kRunning);
            }
            session->pending_output.Push(chunk);
            session->transcript.Push(chunk);
            // Trimmed at twice its size, so each byte moves at most once.
            session->until_window.append(chunk);
            if (session->until_window.size() > 2 * kUntilWindowBytes) {
              session->until_window.erase(
                  0, session->until_window.size() - kUntilWindowBytes);
            }
            int64_t remaining = std::max(
                int64_t{0}, session->log_limit - session->logged_bytes);
            keep = std::min(static_cast<size_t>(remaining), chunk.size());
            session->logged_bytes += static_cast<int64_t>(keep);
            log_fd = session->log_fd.Get();
            notify = true;
          }
          if (log_fd >= 0) (void)WriteAll(log_fd, chunk.substr(0, keep));
        }
      }

      int status = 0;
      pid_t waited = waitpid(session->pid, &status, WNOHANG);
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (waited == session->pid && !session->wait_status) {
          session->wait_status = status;
          (void)TransitionActivityLocked(*session, ActivityState::kExited);
          session->exited_at = now;
          notify = true;
        }
        if (session->wait_status &&
            (session->output_eof ||
             now - session->exited_at >= kTrailingOutputGrace)) {
          (void)TransitionActivityLocked(
              *session, session->stop_requested ? ActivityState::kStopped
                                                : ActivityState::kDrained);
          notify = true;
        }
      }
      if (notify) Wake();
    }

    std::vector<std::shared_ptr<ActivitySession>> removed;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto terminal = [](const std::shared_ptr<ActivitySession>& session) {
        std::lock_guard<std::mutex> state_lock(session->mutex);
        return ActivityTerminal(session->state);
      };
      for (const auto& session : io_sessions_) {
        if (terminal(session)) removed.push_back(session);
      }
      std::erase_if(io_sessions_, terminal);
    }
    for (const auto& session : removed) CloseSessionIo(session);
    json current = ActivityViews();
    if (current != previous) {
      previous = current;
      Emit(Event{EventId::kActivitiesChanged,
                 {{"activities", std::move(current)}}});
    }
  }

  std::vector<std::shared_ptr<ActivitySession>> remaining;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remaining.swap(io_sessions_);
  }
  for (const auto& session : remaining) CloseSessionIo(session);
}

}  // namespace uagent
