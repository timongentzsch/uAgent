// Copyright 2026 Timon Gentzsch

#include "include/tools/process.h"

#include "include/tools/jobs.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace uagent {
namespace {

constexpr size_t kRetainedActivities = 16;
constexpr auto kTrailingOutputGrace = std::chrono::milliseconds(100);

void CloseFd(int& fd) {
  if (fd >= 0) close(fd);
  fd = -1;
}

void WakeFd(int fd) {
  if (fd < 0) return;
  const char byte = 1;
  ssize_t ignored = write(fd, &byte, 1);
  (void)ignored;
}

}  // namespace

ActivityKind ParseActivityKind(const std::string& kind, bool detached) {
  if (detached) return ActivityKind::kDetached;
  if (kind == "task") return ActivityKind::kTask;
  if (kind == "memory") return ActivityKind::kMemory;
  return ActivityKind::kCommand;
}

std::string ActivityKindName(ActivityKind kind) {
  switch (kind) {
    case ActivityKind::kTask:
      return "task";
    case ActivityKind::kMemory:
      return "memory";
    case ActivityKind::kDetached:
      return "detached";
    case ActivityKind::kCommand:
      return "";
  }
  return "";
}

bool ActivityTerminal(ActivityState state) {
  return state == ActivityState::kDrained ||
         state == ActivityState::kDelivered ||
         state == ActivityState::kStopped;
}

BgJob::BgJob(pid_t process_pid, std::string log_path, std::string command,
             bool is_detached, std::string job_kind,
             std::optional<int> status, int64_t activity_id,
             std::shared_ptr<ActivitySession> activity, std::string label,
             std::string receipt, std::string source)
    : pid(process_pid),
      log(std::move(log_path)),
      cmd(std::move(command)),
      detached(is_detached),
      kind(std::move(job_kind)),
      leader_status(status),
      id(activity_id),
      session(std::move(activity)),
      display_label(std::move(label)),
      receipt_path(std::move(receipt)),
      source_id(std::move(source)) {}

ActivityReservation::~ActivityReservation() { Reset(); }

ActivityReservation::ActivityReservation(ActivityReservation&& other) noexcept
    : supervisor_(std::exchange(other.supervisor_, nullptr)) {}

ActivityReservation& ActivityReservation::operator=(
    ActivityReservation&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  supervisor_ = std::exchange(other.supervisor_, nullptr);
  return *this;
}

void ActivityReservation::Reset() {
  if (!supervisor_) return;
  supervisor_->ReleaseReservation();
  supervisor_ = nullptr;
}

std::optional<int64_t> ActivityReservation::Register(BgJob job) {
  if (!supervisor_) return std::nullopt;
  ProcessSupervisor* supervisor = supervisor_;
  supervisor_ = nullptr;
  return supervisor->CommitReservation(std::move(job));
}

ProcessSupervisor::ProcessSupervisor() {
  int wake[2] = {-1, -1};
  if (pipe(wake) == 0) {
    wake_read_ = wake[0];
    wake_write_ = wake[1];
    fcntl(wake_read_, F_SETFL, fcntl(wake_read_, F_GETFL) | O_NONBLOCK);
    fcntl(wake_write_, F_SETFL, fcntl(wake_write_, F_GETFL) | O_NONBLOCK);
    fcntl(wake_read_, F_SETFD, FD_CLOEXEC);
    fcntl(wake_write_, F_SETFD, FD_CLOEXEC);
  }
}

ProcessSupervisor::~ProcessSupervisor() {
  BgShutdownAll(*this);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    NotifyLocked();
  }
  WakeFd(wake_write_);
  if (io_thread_.joinable()) io_thread_.join();
  CloseFd(wake_read_);
  CloseFd(wake_write_);
}

void ProcessSupervisor::StartIoLocked() {
  if (!io_thread_.joinable()) io_thread_ = std::thread([this] { IoLoop(); });
}

void ProcessSupervisor::RegisterIo(
    const std::shared_ptr<ActivitySession>& session, int output_fd, int input_fd,
    int log_fd, int64_t log_limit) {
  if (!session) return;
  if (output_fd >= 0) {
    fcntl(output_fd, F_SETFL, fcntl(output_fd, F_GETFL) | O_NONBLOCK);
    fcntl(output_fd, F_SETFD, FD_CLOEXEC);
  }
  if (input_fd >= 0) fcntl(input_fd, F_SETFD, FD_CLOEXEC);
  {
    std::lock_guard<std::mutex> state_lock(session->mutex);
    session->output_fd = output_fd;
    session->input_fd = input_fd;
    session->log_fd = log_fd;
    session->log_limit = log_limit;
    session->state = ActivityState::kRunning;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    io_sessions_.push_back(session);
    StartIoLocked();
    NotifyLocked();
  }
  WakeFd(wake_write_);
}

void ProcessSupervisor::AssignId(BgJob& job) {
  if (job.id <= 0) {
    job.id = job.detached ? static_cast<int64_t>(job.pid) : next_id_++;
  }
  if (job.session) {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->id = job.id;
    job.session->pid = job.pid;
    job.session->kind = ParseActivityKind(job.kind, job.detached);
    job.session->log = job.log;
    job.session->cmd = job.cmd;
    job.session->last_used = std::chrono::steady_clock::now();
  }
}

std::optional<ActivityReservation> ProcessSupervisor::ReserveActivity(
    int64_t max_pending) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t live = foreground_.size() + static_cast<size_t>(std::count_if(
                                           jobs_.begin(), jobs_.end(),
                                           [](const BgJob& current) {
                                             return !current.detached;
                                           }));
  if (static_cast<int64_t>(live) + reservations_ >= max_pending) {
    return std::nullopt;
  }
  ++reservations_;
  NotifyLocked();
  return ActivityReservation(this);
}

std::optional<int64_t> ProcessSupervisor::CommitReservation(BgJob job) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservations_ <= 0) return std::nullopt;
  --reservations_;
  if (stopping_) {
    NotifyLocked();
    return std::nullopt;
  }
  AssignId(job);
  foreground_.push_back(std::move(job));
  NotifyLocked();
  return ActivityId(foreground_.back());
}

void ProcessSupervisor::ReleaseReservation() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservations_ > 0) --reservations_;
  NotifyLocked();
}

std::optional<BgJob> ProcessSupervisor::RemoveForeground(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = std::find_if(foreground_.begin(), foreground_.end(),
                            [pid](const BgJob& job) { return job.pid == pid; });
  if (found == foreground_.end()) return std::nullopt;
  BgJob job = std::move(*found);
  foreground_.erase(found);
  NotifyLocked();
  return job;
}

std::optional<BgJob> ProcessSupervisor::MoveForegroundToBackground(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = std::find_if(foreground_.begin(), foreground_.end(),
                            [pid](const BgJob& job) { return job.pid == pid; });
  if (found == foreground_.end()) return std::nullopt;
  jobs_.push_back(std::move(*found));
  foreground_.erase(found);
  NotifyLocked();
  return jobs_.back();
}

size_t ProcessSupervisor::ForegroundCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return foreground_.size();
}

bool ProcessSupervisor::WaitForForeground(
    size_t count, std::chrono::steady_clock::time_point deadline) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return event_.wait_until(lock, deadline,
                           [&] { return foreground_.size() >= count; });
}

bool ProcessSupervisor::RequestForegroundBackground() {
  std::vector<std::shared_ptr<ActivitySession>> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (foreground_.empty()) return false;
    for (const BgJob& job : foreground_) {
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

bool ProcessSupervisor::TryAdd(BgJob job, int64_t max_pending) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!job.detached) {
    size_t count = foreground_.size() + static_cast<size_t>(std::count_if(
                                             jobs_.begin(), jobs_.end(),
                                             [](const BgJob& current) {
                                               return !current.detached;
                                             }));
    if (static_cast<int64_t>(count) + reservations_ >= max_pending) return false;
  }
  AssignId(job);
  jobs_.push_back(std::move(job));
  NotifyLocked();
  return true;
}

size_t ProcessSupervisor::IndexOfLocked(int64_t id) const {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (ActivityId(jobs_[i]) == id) return i;
  }
  return jobs_.size();
}

size_t ProcessSupervisor::RetainedIndexOfLocked(int64_t id) const {
  for (size_t i = 0; i < retained_.size(); ++i) {
    if (ActivityId(retained_[i]) == id) return i;
  }
  return retained_.size();
}

size_t ProcessSupervisor::PendingCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(jobs_.begin(), jobs_.end(),
                                           [](const BgJob& job) {
                                             return !job.detached;
                                           }));
}

size_t ProcessSupervisor::DetachedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(jobs_.begin(), jobs_.end(),
                                           [](const BgJob& job) {
                                             return job.detached;
                                           }));
}

size_t ProcessSupervisor::Count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_.size();
}

size_t ProcessSupervisor::JoinableCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(jobs_.begin(), jobs_.end(),
                                           [](const BgJob& job) {
                                             return !job.detached &&
                                                    job.session &&
                                                    job.session->kind ==
                                                        ActivityKind::kTask;
                                           }));
}

bool ProcessSupervisor::IsLive(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IndexOfLocked(id) != jobs_.size();
}

std::optional<BgJob> ProcessSupervisor::Find(int64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = IndexOfLocked(id);
  if (index != jobs_.size()) return jobs_[index];
  index = RetainedIndexOfLocked(id);
  return index == retained_.size() ? std::nullopt
                                   : std::optional<BgJob>(retained_[index]);
}

std::optional<BgJob> ProcessSupervisor::Take(int64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t index = IndexOfLocked(id);
  if (index == jobs_.size()) return std::nullopt;
  BgJob job = std::move(jobs_[index]);
  jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(index));
  NotifyLocked();
  return job;
}

std::vector<BgJob> ProcessSupervisor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_;
}

std::vector<BgJob> ProcessSupervisor::TakeAllForShutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<BgJob> jobs;
  jobs.swap(jobs_);
  for (BgJob& job : foreground_) jobs.push_back(std::move(job));
  foreground_.clear();
  NotifyLocked();
  return jobs;
}

void ProcessSupervisor::PruneRetainedLocked() {
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

void ProcessSupervisor::Retain(BgJob job) {
  if (!job.session || job.detached) return;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->state = ActivityState::kDelivered;
    job.session->delivered = true;
    job.session->last_used = std::chrono::steady_clock::now();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  retained_.push_back(std::move(job));
  PruneRetainedLocked();
  NotifyLocked();
}

uint64_t ProcessSupervisor::Generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

void ProcessSupervisor::NotifyLocked() {
  ++generation_;
  event_.notify_all();
}

void ProcessSupervisor::Wake() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    NotifyLocked();
  }
  WakeFd(wake_write_);
}

bool ProcessSupervisor::WaitForChange(
    uint64_t generation,
    std::chrono::steady_clock::time_point deadline) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return event_.wait_until(lock, deadline,
                           [&] { return generation_ != generation; });
}

void ProcessSupervisor::IoLoop() {
  for (;;) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) break;
      sessions = io_sessions_;
    }
    std::vector<pollfd> poll_fds;
    poll_fds.reserve(sessions.size() + 1);
    poll_fds.push_back({wake_read_, POLLIN, 0});
    for (const auto& session : sessions) {
      int fd = -1;
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        fd = session->output_fd;
      }
      poll_fds.push_back({fd, static_cast<short>(POLLIN | POLLHUP | POLLERR),
                          0});
    }
    int ready = poll(poll_fds.data(), poll_fds.size(), 50);
    if (ready < 0 && errno != EINTR) continue;
    if (!poll_fds.empty() && (poll_fds[0].revents & POLLIN)) {
      std::array<char, 128> wake{};
      while (read(wake_read_, wake.data(), wake.size()) > 0) {
      }
    }

    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sessions.size(); ++i) {
      const auto& session = sessions[i];
      bool notify = false;
      short events = poll_fds[i + 1].revents;
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
            session->state = session->state == ActivityState::kStarting
                                 ? ActivityState::kRunning
                                 : session->state;
            session->pending_output.Push(chunk);
            session->transcript.Push(chunk);
            session->until_window.append(chunk);
            if (session->until_window.size() > 64 * 1024) {
              session->until_window.erase(0,
                                          session->until_window.size() - 64 * 1024);
            }
            int64_t remaining =
                std::max(int64_t{0}, session->log_limit - session->logged_bytes);
            keep = std::min(static_cast<size_t>(remaining), chunk.size());
            session->logged_bytes += static_cast<int64_t>(keep);
            log_fd = session->log_fd;
            notify = true;
          }
          size_t offset = 0;
          while (log_fd >= 0 && offset < keep) {
            ssize_t written = write(log_fd, bytes.data() + offset, keep - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            offset += static_cast<size_t>(written);
          }
        }
      }

      int status = 0;
      pid_t waited = waitpid(session->pid, &status, WNOHANG);
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (waited == session->pid && !session->wait_status) {
          session->wait_status = status;
          session->state = ActivityState::kExited;
          session->exited_at = now;
          notify = true;
        }
        if (session->wait_status &&
            (session->output_eof ||
             now - session->exited_at >= kTrailingOutputGrace)) {
          session->state = session->stop_requested ? ActivityState::kStopped
                                                   : ActivityState::kDrained;
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
    for (const auto& session : removed) {
      std::lock_guard<std::mutex> lock(session->mutex);
      CloseFd(session->output_fd);
      CloseFd(session->input_fd);
      CloseFd(session->log_fd);
    }
  }

  std::vector<std::shared_ptr<ActivitySession>> remaining;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    remaining.swap(io_sessions_);
  }
  for (const auto& session : remaining) {
    std::lock_guard<std::mutex> lock(session->mutex);
    CloseFd(session->output_fd);
    CloseFd(session->input_fd);
    CloseFd(session->log_fd);
  }
}

}  // namespace uagent
