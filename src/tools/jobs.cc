// Copyright 2026 Timon Gentzsch

#include "include/tools/jobs.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/tools/child_agent.h"
#include "include/tools/files.h"

namespace uagent {

bool SignalProcessGroup(pid_t leader, int signal_number) {
  if (kill(-leader, signal_number) == 0 || errno == EPERM) return true;
  return errno == ESRCH;
}

namespace {

// The leader's own exit status is not the group's, and nothing reads it: this
// only stops it lingering as a zombie while the group finishes.
void ReapLeader(pid_t leader) {
  int status = 0;
  WaitPid(leader, &status, WNOHANG);
}

bool WaitForProcessGroupExit(ProcessSupervisor& supervisor, pid_t leader,
                             std::chrono::milliseconds timeout,
                             bool reap_leader = false) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    uint64_t generation = supervisor.Generation();
    if (reap_leader) ReapLeader(leader);
    if (!ProcessGroupAlive(leader)) return true;
    if (!supervisor.WaitForChange(generation, deadline)) break;
  }
  if (reap_leader) ReapLeader(leader);
  return !ProcessGroupAlive(leader);
}

// TERM, then KILL if the group outlives the grace period. False only when the
// group is still alive after the escalation.
bool TerminateGroup(ProcessSupervisor& supervisor, pid_t leader,
                    std::chrono::milliseconds grace, bool reap_leader) {
  if (!SignalProcessGroup(leader, SIGTERM)) return false;
  if (WaitForProcessGroupExit(supervisor, leader, grace, reap_leader)) {
    return true;
  }
  if (!SignalProcessGroup(leader, SIGKILL)) return false;
  return WaitForProcessGroupExit(supervisor, leader, grace, reap_leader);
}

}  // namespace

std::string SupervisedJobLabel(const BgJob& job) {
  ActivityKind kind = job.kind;
  if (kind == ActivityKind::kSubagent) return "subagent";
  if (kind == ActivityKind::kMemory) return "memory";
  return job.detached ? "detached" : "background";
}

namespace {

// The banner every activity read carries: who it is, whether it is still worth
// writing to, and where the full log lives.
std::string ActivityHeader(const BgJob& job, std::string_view state = {}) {
  return "[" + SupervisedJobLabel(job) + " · activity " +
         std::to_string(ActivityId(job)) + std::string(state) + " · log " +
         job.log + "]\n";
}

}  // namespace

static ToolResult FormatActivityList(const std::vector<BgJob>& supervised,
                                     const std::vector<json>& records,
                                     std::string_view empty) {
  if (supervised.empty() && records.empty()) {
    return ToolSuccess(std::string(empty));
  }
  std::string output;
  for (const BgJob& job : supervised) {
    std::string summary =
        job.display_label.empty() ? FirstLine(job.cmd) : job.display_label;
    output += "[" + SupervisedJobLabel(job) + "] activity " +
              std::to_string(ActivityId(job)) + " · " + summary + " · " +
              job.log + "\n";
  }
  for (const json& record : records) {
    pid_t record_pid = JsonValue(record, "pid", 0);
    if (std::any_of(supervised.begin(), supervised.end(),
                    [record_pid](const BgJob& job) {
                      return job.detached && job.pid == record_pid;
                    })) {
      continue;
    }
    output += JsonValue(record, "_alive", false) ? "[running] " : "[exited] ";
    output += "activity " + std::to_string(record_pid) + " · " +
              JsonValue(record, "cwd", "") + " · " +
              FirstLine(JsonValue(record, "command", "")) + " · " +
              JsonValue(record, "log", "") + "\n";
  }
  int64_t cap = ToolResultCap();
  if (cap > 0 && output.size() > static_cast<size_t>(cap)) {
    output = Utf8Trunc(std::move(output), static_cast<size_t>(cap));
    output += "\n[activity list truncated]\n";
  }
  return ToolSuccess(std::move(output));
}

ToolResult ToolActivityList(const ProcessSupervisor& supervisor) {
  return FormatActivityList(supervisor.Snapshot(), {},
                            "(no active background work)");
}

std::string DrainActivityOutput(const BgJob& job, int64_t cap) {
  if (!job.session) return ReadLogTail(job.log, cap);
  std::string raw;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    raw = job.session->pending_output.Drain();
    job.session->last_used = std::chrono::steady_clock::now();
  }
  if (raw.empty()) return std::string(kNoNewActivityOutput);
  return LimitOutput(std::move(raw), cap);
}

namespace {

// fresh reports whether the returned text is captured output rather than a
// status sentinel, so callers never have to compare against those strings.
std::string CollectSessionOutput(const ProcessSupervisor& supervisor,
                                 const BgJob& job, int64_t wait_ms,
                                 std::string_view until,
                                 const ToolContext& context, int64_t cap,
                                 bool settle = false, bool* fresh = nullptr) {
  auto done = [fresh](std::string text, bool captured) {
    if (fresh) *fresh = captured;
    return text;
  };
  if (!job.session) {
    std::string tail = ReadLogTail(job.log, cap);
    bool captured = !tail.empty();
    return done(std::move(tail), captured);
  }
  auto poll_started = std::chrono::steady_clock::now();
  auto poll_requested = poll_started + std::chrono::milliseconds(wait_ms);
  auto deadline = std::min(context.deadline, poll_requested);
  bool poll_capped = context.deadline < poll_requested;
  HeadTailBuffer collected(cap > 0 ? static_cast<size_t>(cap)
                                   : size_t{1024} * 1024);
  std::optional<std::chrono::steady_clock::time_point> quiet_deadline;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->until_window.clear();
  }
  for (;;) {
    uint64_t generation = supervisor.Generation();
    std::string chunk = DrainActivityOutput(job, cap);
    if (chunk != kNoNewActivityOutput) {
      collected.Push(chunk);
      if (settle) {
        quiet_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      }
    }
    // Materialising the buffer is not free, so every exit test below shares one
    // snapshot per iteration.
    std::string snapshot = collected.Snapshot();
    bool terminal = false;
    bool matched = false;
    {
      std::lock_guard<std::mutex> lock(job.session->mutex);
      terminal = ActivityTerminal(job.session->state);
      matched = !until.empty() &&
                (snapshot.find(until) != std::string::npos ||
                 job.session->until_window.find(until) != std::string::npos);
    }
    // keep=false is the abort case: an interrupted wait reports only that,
    // leaving the captured bytes for the next read.
    auto finish = [&](std::string_view note, bool keep = true) {
      bool captured = keep && !snapshot.empty();
      std::string output = keep ? std::move(snapshot) : std::string();
      if (note.empty()) {
        return done(
            captured ? std::move(output) : std::string(kNoNewActivityOutput),
            captured);
      }
      if (captured) output += "\n";
      output.append(note);
      return done(std::move(output), captured);
    };
    if (matched || terminal ||
        (until.empty() && !settle && !snapshot.empty()) ||
        (quiet_deadline &&
         std::chrono::steady_clock::now() >= *quiet_deadline) ||
        wait_ms <= 0) {
      return finish({});
    }
    if (AbortRequested()) {
      return finish("[wait interrupted; process still running]",
                    /*keep=*/false);
    }
    if (SteeringYieldRequested()) {
      return finish(
          "[wait yielded for queued steering; process still running]");
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      double waited = std::chrono::duration<double>(now - poll_started).count();
      std::string note = "[waited " + FmtDuration(waited);
      if (poll_capped) {
        note += " of " + FmtDuration(static_cast<double>(wait_ms) / 1000.0) +
                " requested, capped by the turn deadline";
      }
      return finish(note + "; process still running]");
    }
    auto wait_deadline =
        quiet_deadline ? std::min(deadline, *quiet_deadline) : deadline;
    supervisor.WaitForChange(generation, wait_deadline);
  }
}

}  // namespace

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor,
                              int64_t pid) {
  if (pid <= 0) {
    // Only the listing needs every job copied out of the supervisor.
    return FormatActivityList(supervisor.Snapshot(), DetachedRecords(),
                              "(no supervised activities)");
  }

  if (std::optional<BgJob> job = supervisor.Find(pid)) {
    std::unique_lock<std::mutex> interaction;
    if (job->session) {
      interaction = std::unique_lock<std::mutex>(job->session->interaction);
    }
    // Whether it is still running is the first thing the reader needs, so it
    // rides in the header: an exit code means finished, its absence means the
    // id is still worth writing to.
    std::string state;
    bool terminal = false;
    if (job->session) {
      std::lock_guard<std::mutex> lock(job->session->mutex);
      terminal = job->session->wait_status.has_value() ||
                 ActivityTerminal(job->session->state);
      if (job->session->wait_status) {
        state = " · exit " +
                std::to_string(WIFEXITED(*job->session->wait_status)
                                   ? WEXITSTATUS(*job->session->wait_status)
                                   : -1);
      }
    }
    bool fresh = false;
    std::string output = CollectSessionOutput(supervisor, *job, 0, {}, {},
                                              ToolResultCap(), false, &fresh);
    ToolResult result = ToolSuccess(ActivityHeader(*job, state) + output);
    result.no_change = !fresh;
    result.activity_terminal = terminal;
    return result;
  }

  std::optional<json> detached = FindDetachedRecord(pid);
  if (!detached) return ActivityNotFound(pid);
  const json& record = *detached;
  bool alive = JsonValue(record, "_alive", false);
  std::string status = alive ? "running" : "exited";
  ToolResult result = ToolSuccess(
      "[" + status + " · activity " + std::to_string(pid) + " · " +
      JsonValue(record, "cwd", "") + " · log " + JsonValue(record, "log", "") +
      "]\n" + ReadLogTail(JsonValue(record, "log", ""), ToolResultCap()));
  result.activity_terminal = !alive;
  return result;
}

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor, int64_t id,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context,
                              int64_t max_output_chars) {
  int64_t cap = ActivityOutputCap(max_output_chars);
  if (id <= 0) return LimitOutput(ToolActivityOutput(supervisor, id), cap);
  // " · " is the prefix ActivityLabel keeps when it has to clip.
  TerminalActivityLabel waiting("wait · activity " + std::to_string(id));
  std::optional<BgJob> job = supervisor.Find(id);
  if (!job || !job->session) {
    // Persistent detached activities intentionally remain rotating-log based.
    // The job or record is resolved once here: re-deriving it per iteration
    // would re-parse every terminal record for the length of the wait.
    std::string watch_path;
    std::string record_cwd;
    if (job) {
      watch_path = job->log;
    } else {
      std::optional<json> record = FindDetachedRecord(id);
      if (!record) return ActivityNotFound(id);
      watch_path = JsonValue(*record, "log", "");
      record_cwd = JsonValue(*record, "cwd", "");
    }
    // Only liveness can change while waiting, so the banner is formatted once
    // the wait is over rather than on every poll.
    auto persisted_alive = [&] {
      std::optional<json> current = FindDetachedRecord(id);
      return current && JsonValue(*current, "_alive", false);
    };
    auto reply = [&](std::string body, bool no_change) {
      bool alive = job ? ProcessGroupAlive(job->pid) : persisted_alive();
      std::string header =
          job ? ActivityHeader(*job)
              : "[" + std::string(alive ? "running" : "exited") +
                    " · activity " + std::to_string(id) + " · " + record_cwd +
                    " · log " + watch_path + "]\n";
      ToolResult result = ToolSuccess(header + std::move(body));
      result.no_change = no_change;
      result.activity_terminal = !alive;
      return LimitOutput(std::move(result), cap);
    };
    // Read at the global cap, not the caller's: reply() applies the head/tail
    // limiter, so a lowered max_output_chars still keeps the window's head.
    const int64_t read_cap = ToolResultCap();
    std::string current = ReadLogTail(watch_path, read_cap);
    if (wait_ms <= 0) return reply(std::move(current), false);
    auto watch_started = std::chrono::steady_clock::now();
    auto watch_requested = watch_started + std::chrono::milliseconds(wait_ms);
    auto deadline = std::min(context.deadline, watch_requested);
    bool watch_capped = context.deadline < watch_requested;
    std::string accumulated = current;
    for (;;) {
      bool steering_yield = SteeringYieldRequested();
      bool found =
          !until.empty() && accumulated.find(until) != std::string::npos;
      auto now = std::chrono::steady_clock::now();
      bool expired = now >= deadline;
      if (found || expired || AbortRequested() || steering_yield) {
        bool no_change = accumulated == current && !steering_yield;
        if (steering_yield) {
          if (!accumulated.empty()) accumulated += "\n";
          accumulated +=
              "[wait yielded for queued steering; process still running]";
        } else if (expired && !found) {
          // Returning the log alone reads as "here is the state you asked
          // for", when in fact the marker never appeared and the wait may have
          // been cut short by the turn deadline rather than by wait_ms.
          if (!accumulated.empty()) accumulated += "\n";
          double waited =
              std::chrono::duration<double>(now - watch_started).count();
          accumulated += until.empty()
                             ? "[waited " + FmtDuration(waited)
                             : "[marker \"" + std::string(until) +
                                   "\" not seen in " + FmtDuration(waited);
          if (watch_capped) {
            accumulated += " of " +
                           FmtDuration(static_cast<double>(wait_ms) / 1000.0) +
                           " requested, capped by the turn deadline";
          }
          accumulated += "; process still running; call again to keep waiting]";
          no_change = false;
        }
        return reply(std::move(accumulated), no_change);
      }
      FileStamp observed = SnapshotFile(watch_path);
      std::string next = ReadLogTail(watch_path, read_cap);
      if (next != current) {
        accumulated = next;
        if (until.empty()) return reply(std::move(accumulated), false);
        current = std::move(next);
        continue;
      }
      FileWaitResult changed =
          WaitForFileChange(watch_path, observed, deadline);
      if (changed == FileWaitResult::kInterrupted ||
          changed == FileWaitResult::kSteering ||
          changed == FileWaitResult::kTimedOut) {
        continue;
      }
    }
  }
  std::lock_guard<std::mutex> interaction(job->session->interaction);
  bool fresh = false;
  std::string output = CollectSessionOutput(supervisor, *job, wait_ms, until,
                                            context, cap, false, &fresh);
  bool no_change = !fresh;
  if (output == kNoNewActivityOutput) {
    std::string replay;
    {
      std::lock_guard<std::mutex> lock(job->session->mutex);
      if (job->session->state == ActivityState::kDelivered) {
        replay = job->session->transcript.Snapshot();
      }
    }
    if (!replay.empty()) {
      output = "[complete transcript replay]\n" +
               LimitOutput(std::move(replay), cap);
      no_change = false;
    }
  }
  ToolResult result = ToolSuccess(ActivityHeader(*job) + output);
  result.no_change = no_change;
  {
    std::lock_guard<std::mutex> lock(job->session->mutex);
    result.activity_terminal = job->session->wait_status.has_value() ||
                               ActivityTerminal(job->session->state);
  }
  return result;
}

ToolResult ToolActivityInput(const ProcessSupervisor& supervisor, int64_t id,
                             const std::string& chars, int64_t wait_ms,
                             const ToolContext& context, int64_t rows,
                             int64_t cols, int64_t max_output_chars) {
  std::optional<BgJob> job = supervisor.Find(id);
  if (!job || !job->session) return ActivityNotFound(id);
  std::shared_ptr<ActivitySession> session = job->session;
  std::lock_guard<std::mutex> interaction(session->interaction);
  const bool resize = rows > 0 || cols > 0;
  if (resize && (rows <= 0 || cols <= 0 || rows > 1000 || cols > 1000)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: PTY resize requires rows and cols in 1..1000");
  }

  const bool needs_input =
      resize || (!chars.empty() && chars != std::string_view("\x03"));
  if (needs_input && !session->tty) {
    std::string action =
        resize ? "has no PTY to resize" : "does not accept input";
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: activity " + std::to_string(id) + " " + action +
                           "; start it with tty=true");
  }

  // A private duplicate: the supervisor's I/O thread may close the session's
  // own descriptor at any point after this lock is released.
  Fd input;
  bool input_open = false;
  int duplicate_error = 0;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->last_used = std::chrono::steady_clock::now();
    if (needs_input) {
      input_open = session->input_fd.Valid();
      if (input_open) {
        input = session->input_fd.Duplicate();
        if (!input) duplicate_error = errno;
      }
    }
  }
  if (needs_input && !input_open) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: activity " + std::to_string(id) +
                           " input is closed; inspect its output or start a "
                           "new tty=true activity");
  }
  if (needs_input && !input) {
    return ToolFailure(ToolErrorCode::kInternal,
                       "error: could not access activity " +
                           std::to_string(id) +
                           " input: " + std::strerror(duplicate_error));
  }

  if (resize) {
    winsize size{};
    size.ws_row = static_cast<uint16_t>(rows);
    size.ws_col = static_cast<uint16_t>(cols);
    if (ioctl(input.Get(), TIOCSWINSZ, &size) != 0) {
      int resize_error = errno;
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "error: could not resize activity " +
                             std::to_string(id) +
                             " PTY: " + std::strerror(resize_error));
    }
    (void)kill(-job->pid, SIGWINCH);
  }
  if (!chars.empty()) {
    if (chars == "\x03") {
      if (kill(-job->pid, SIGINT) != 0 && errno != ESRCH) {
        int interrupt_error = errno;
        return ToolFailure(ToolErrorCode::kProcessFailed,
                           "error: could not interrupt activity " +
                               std::to_string(id) + ": " +
                               std::strerror(interrupt_error));
      }
    } else {
      size_t offset = 0;
      while (offset < chars.size()) {
        ssize_t count =
            write(input.Get(), chars.data() + offset, chars.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          int write_error = errno;
          return ToolFailure(ToolErrorCode::kProcessFailed,
                             "error: could not write activity " +
                                 std::to_string(id) +
                                 " stdin: " + std::strerror(write_error));
        }
        offset += static_cast<size_t>(count);
      }
    }
  }
  input.Reset();
  int64_t cap = ActivityOutputCap(max_output_chars);
  std::string output = CollectSessionOutput(supervisor, *job, wait_ms, {},
                                            context, cap, !chars.empty());
  return ToolSuccess(ActivityHeader(*job) + output);
}

ToolResult ToolActivityStop(ProcessSupervisor& supervisor, int64_t requested) {
  if (requested <= 0) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: id must identify a supervised uagent activity");
  }
  std::optional<BgJob> supervised = supervisor.Find(requested);
  if (!supervised && requested > std::numeric_limits<pid_t>::max()) {
    return ActivityNotFound(requested);
  }
  pid_t pid = supervised ? supervised->pid : static_cast<pid_t>(requested);
  std::string log;
  bool detached = false;
  bool persisted_alive = false;
  if (supervised) {
    log = supervised->log;
    detached = supervised->detached;
  } else {
    std::optional<json> record = FindDetachedRecord(pid);
    if (!record) return ActivityNotFound(pid);
    log = JsonValue(*record, "log", "");
    detached = true;
    persisted_alive = JsonValue(*record, "_alive", false);
  }

  bool was_alive = persisted_alive;
  if (supervised) {
    bool terminal = false;
    if (supervised->session) {
      std::lock_guard<std::mutex> lock(supervised->session->mutex);
      terminal = supervised->session->wait_status.has_value() ||
                 ActivityTerminal(supervised->session->state);
    }
    was_alive = !terminal && ProcessGroupAlive(pid);
  }
  bool reap_leader = !supervised || !supervised->session;
  if (supervised && supervised->session) {
    std::lock_guard lock(supervised->session->mutex);
    if (ActivityTerminal(supervised->session->state)) {
      return ToolSuccess("activity already complete");
    }
    supervised->session->stop_requested = true;
  }
  supervisor.Wake();
  if (was_alive) {
    if (!TerminateGroup(supervisor, pid, std::chrono::seconds(1),
                        reap_leader)) {
      return ToolFailure(
          ToolErrorCode::kProcessFailed,
          "error: could not stop process group " + std::to_string(pid));
    }
  } else if (detached) {
    ReapLeader(pid);
  }
  if (supervised && supervised->session) {
    std::lock_guard<std::mutex> lock(supervised->session->mutex);
    (void)TransitionActivityLocked(*supervised->session,
                                   ActivityState::kStopped);
  }
  if (supervised) {
    (void)supervisor.Take(requested, /*retain=*/true);
  }
  if (!detached) BgTrackSignal(pid, false);
  if (detached) unlink(DetachedRecordPath(pid).c_str());
  RemoveLog(log);
  supervisor.Wake();
  return ToolSuccess(
      std::string(was_alive ? "stopped process group "
                            : "process group already exited; cleaned pid ") +
      std::to_string(pid));
}

void BgShutdownAll(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAllForShutdown();
  std::erase_if(jobs, [](const BgJob& job) { return job.detached; });
  for (const BgJob& job : jobs) SignalProcessGroup(job.pid, SIGTERM);
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!jobs.empty()) {
    uint64_t generation = supervisor.Generation();
    std::erase_if(jobs, [](const BgJob& job) {
      if (ProcessGroupAlive(job.pid)) return false;
      BgTrackSignal(job.pid, false);
      RemoveLog(job.log);
      return true;
    });
    if (jobs.empty() || !supervisor.WaitForChange(generation, deadline)) {
      break;
    }
  }
  for (const BgJob& job : jobs) {
    SignalProcessGroup(job.pid, SIGKILL);
    BgTrackSignal(job.pid, false);
    RemoveLog(job.log);
  }
  supervisor.Wake();
}

size_t BgCancelSubagents(ProcessSupervisor& supervisor) {
  size_t cancelled = 0;
  for (const BgJob& candidate : supervisor.Snapshot()) {
    if (candidate.detached || !candidate.session ||
        candidate.kind != ActivityKind::kSubagent) {
      continue;
    }
    std::optional<BgJob> job = supervisor.Take(ActivityId(candidate));
    if (!job) continue;
    ++cancelled;
    (void)TerminateGroup(supervisor, job->pid, std::chrono::milliseconds(500),
                         /*reap_leader=*/false);
    BgTrackSignal(job->pid, false);
    RemoveLog(job->log);
  }
  supervisor.Wake();
  return cancelled;
}

}  // namespace uagent
