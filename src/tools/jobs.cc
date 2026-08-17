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
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"

namespace uagent {
namespace {

bool SignalProcessGroup(pid_t leader, int signal_number) {
  if (kill(-leader, signal_number) == 0 || errno == EPERM) return true;
  return errno == ESRCH;
}

pid_t PollProcess(pid_t pid, int* status) {
  return WaitPid(pid, status, WNOHANG);
}

void ReapLeader(pid_t leader) {
  int status = 0;
  PollProcess(leader, &status);
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

}  // namespace

void BgTrackSignal(pid_t pid, bool add) {
  TrackPid(g_bg_pids, kBgMax, pid, add);
}

void KillProcess(pid_t pid, int* status) {
  SignalProcessGroup(pid, SIGKILL);
  WaitPid(pid, status);
}

// "[exit code N]" / "[killed by signal N]" suffix; "" for a clean exit unless
// show_ok
std::string FmtExit(int status, bool show_ok) {
  if (WIFEXITED(status)) {
    return (WEXITSTATUS(status) != 0 || show_ok)
               ? "\n[exit code " + std::to_string(WEXITSTATUS(status)) + "]"
               : "";
  }
  if (WIFSIGNALED(status)) {
    return "\n[killed by signal " + std::to_string(WTERMSIG(status)) + "]";
  }
  return "";
}

ToolResult ProcessResult(std::string output, int status) {
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return ToolSuccess(std::move(output));
  }
  return ToolFailure(ToolErrorCode::kProcessFailed, std::move(output));
}

std::string ReadLogTail(const std::string& path, int64_t cap) {
  auto tail = [](const std::string& file, int64_t bytes) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f || bytes == 0) return std::pair<std::string, int64_t>{"", 0};
    int64_t size = static_cast<int64_t>(f.tellg());
    int64_t start = bytes > 0 && size > bytes ? size - bytes : 0;
    f.seekg(start);
    return std::pair<std::string, int64_t>{
        std::string(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>()),
        start};
  };
  auto [current, current_start] = tail(path, cap);
  int64_t remaining =
      cap > 0 ? std::max(int64_t{0}, cap - static_cast<int64_t>(current.size()))
              : -1;
  auto [previous, previous_start] = tail(path + ".1", remaining);
  if (current.empty() && previous.empty() && !std::filesystem::exists(path) &&
      !std::filesystem::exists(path + ".1")) {
    return "(no output captured: " + path + " missing)";
  }
  std::string s = previous + current;
  if (previous_start > 0 || current_start > 0 ||
      (!previous.empty() && std::filesystem::exists(path + ".1"))) {
    s = "[rotating log tail]\n" + s;
  }
  return s.empty() ? "(no output)" : s;
}

uint64_t LogFileBytes(const std::string& path) {
  std::error_code error;
  uintmax_t bytes = std::filesystem::file_size(path, error);
  if (error) return 0;
  return bytes > std::numeric_limits<uint64_t>::max()
             ? std::numeric_limits<uint64_t>::max()
             : static_cast<uint64_t>(bytes);
}

void RemoveLog(const std::string& path) {
  unlink(path.c_str());
  unlink((path + ".1").c_str());
}

ToolArtifact PromoteLogArtifact(const std::string& path, uint64_t bytes) {
  std::string target;
  int fd = CreateTempFile(UagentDir(kArtifactsDir) + "/output-XXXXXX", target);
  if (fd >= 0) {
    fchmod(fd, 0600);
    close(fd);
    if (rename(path.c_str(), target.c_str()) == 0) {
      chmod(target.c_str(), 0600);
      return {target, bytes};
    }
    int rename_error = errno;
    unlink(target.c_str());
    if (Debug().Enabled()) {
      Debug().Write("artifact_promotion_failed",
                    {{"path", path}, {"error", strerror(rename_error)}});
    }
  } else if (Debug().Enabled()) {
    Debug().Write("artifact_promotion_failed",
                  {{"path", path}, {"error", strerror(errno)}});
  }
  // Failure must not destroy the only recoverable copy.
  return {path, bytes};
}

// Completed small logs are disposable. Oversized logs become bounded,
// private artifacts so the model can inspect only the relevant slice instead
// of paying to keep the entire stream in context. Non-detached process logs
// are single files; rotating detached logs have their own persistent lifecycle.
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap) {
  uint64_t bytes = LogFileBytes(path);
  CollectedLog collected{ReadLogTail(path, cap), std::nullopt};
  if (cap > 0 && bytes > static_cast<uint64_t>(cap)) {
    collected.artifact = PromoteLogArtifact(path, bytes);
  } else {
    RemoveLog(path);
  }
  return collected;
}

// Hidden subprocess mode used by detached shells. Two half-size segments keep
// server logs bounded without sending SIGXFSZ/SIGPIPE to the server itself.
int ToolLogPump(const std::string& path, int64_t max_bytes) {
  int64_t segment = std::max(int64_t{512}, max_bytes / 2);
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return 1;
  std::array<char, 64 * 1024> buffer{};
  int64_t written = 0;
  for (;;) {
    ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      if (written >= segment) {
        close(fd);
        unlink((path + ".1").c_str());
        rename(path.c_str(), (path + ".1").c_str());
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return 1;
        written = 0;
      }
      size_t chunk = std::min(static_cast<size_t>(segment - written),
                              static_cast<size_t>(count) - offset);
      ssize_t n = write(fd, buffer.data() + offset, chunk);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        close(fd);
        return 1;
      }
      offset += static_cast<size_t>(n);
      written += n;
    }
  }
  close(fd);
  return 0;
}

bool ProcessGroupAlive(pid_t leader) {
  if (leader <= 0) return false;
  if (kill(-leader, 0) == 0) return true;
  return errno == EPERM;
}

std::string DetachedRecordPath(pid_t pid) {
  return UagentDir(kTerminalsDir) + "/" + std::to_string(pid) + ".json";
}

std::vector<json> DetachedRecords() {
  namespace fs = std::filesystem;
  std::vector<json> records;
  auto cutoff = fs::file_time_type::clock::now() -
                std::chrono::hours(24 * TerminalRecordDays());
  std::error_code ec;
  for (fs::directory_iterator it(UagentDir(kTerminalsDir), ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    std::ifstream input(it->path());
    json record = json::parse(input, nullptr, false);
    if (record.is_discarded()) {
      fs::remove(it->path(), ec);
      continue;
    }
    bool alive = ProcessGroupAlive(JsonValue(record, "pid", 0));
    record["_alive"] = alive;
    std::error_code time_error;
    auto modified = fs::last_write_time(it->path(), time_error);
    if (!alive && !time_error && modified < cutoff) {
      RemoveLog(JsonValue(record, "log", ""));
      std::error_code remove_error;
      fs::remove(it->path(), remove_error);
      continue;
    }
    records.push_back(std::move(record));
  }
  std::sort(records.begin(), records.end(), [](const json& a, const json& b) {
    return JsonValue(a, "started_at", "") > JsonValue(b, "started_at", "");
  });
  return records;
}

std::optional<DetachedActivity> FindRunningDetachedActivity(
    const std::string& command) {
  std::error_code error;
  std::filesystem::path cwd = std::filesystem::current_path(error);
  if (error) return std::nullopt;
  cwd = CanonicalAccessPath(cwd.string());
  for (const json& record : DetachedRecords()) {
    std::string recorded_cwd = JsonValue(record, "cwd", "");
    if (!JsonValue(record, "_alive", false) || recorded_cwd.empty() ||
        JsonValue(record, "command", "") != command ||
        CanonicalAccessPath(recorded_cwd) != cwd) {
      continue;
    }
    return DetachedActivity{JsonValue(record, "pid", pid_t{-1}),
                            JsonValue(record, "log", "")};
  }
  return std::nullopt;
}

namespace {

std::optional<json> FindDetachedRecord(int64_t pid) {
  std::vector<json> records = DetachedRecords();
  auto found =
      std::find_if(records.begin(), records.end(), [pid](const json& record) {
        return JsonValue(record, "pid", int64_t{0}) == pid;
      });
  return found == records.end() ? std::nullopt
                                : std::optional<json>(std::move(*found));
}

ToolResult ActivityNotFound(int64_t pid) {
  return ToolFailure(ToolErrorCode::kNotFound,
                     "error: activity " + std::to_string(pid) +
                         " is not supervised by uagent");
}

}  // namespace

ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& cmd) {
  std::error_code ec;
  std::string cwd = std::filesystem::current_path(ec).string();
  json record = {{"pid", pid},
                 {"log", log},
                 {"command", cmd},
                 {"cwd", ec ? "" : cwd},
                 {"started_at", UtcStamp()}};
  std::string content = JsonDump(record, 2) + "\n";
  std::string error;
  std::string path = DetachedRecordPath(pid);
  if (!AtomicWriteFile(path, content, 0600, /*preserve_mode=*/true, error)) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + error);
  }
  return ToolSuccess("wrote " + std::to_string(content.size()) + " bytes to " +
                     path);
}

std::string SupervisedJobLabel(const BgJob& job) {
  ActivityKind kind = job.session ? job.session->kind
                                  : ParseActivityKind(job.kind, job.detached);
  if (kind == ActivityKind::kTask) return "task";
  if (kind == ActivityKind::kMemory) return "memory";
  return job.detached ? "detached" : "background";
}

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

namespace {

std::string DrainIncremental(const BgJob& job, int64_t cap) {
  if (!job.session) return ReadLogTail(job.log, cap);
  std::string raw;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    raw = job.session->pending_output.Drain();
    job.session->last_used = std::chrono::steady_clock::now();
  }
  if (raw.empty()) return "(no new output)";
  if (cap <= 0 || raw.size() <= static_cast<size_t>(cap)) return raw;
  HeadTailBuffer limited(static_cast<size_t>(cap));
  limited.Push(raw);
  return limited.Snapshot();
}

std::string CollectSessionOutput(const ProcessSupervisor& supervisor,
                                 const BgJob& job, int64_t wait_ms,
                                 std::string_view until,
                                 const ToolContext& context, int64_t cap,
                                 bool settle = false) {
  if (!job.session) return ReadLogTail(job.log, cap);
  auto deadline =
      std::min(context.deadline, std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(wait_ms));
  HeadTailBuffer collected(cap > 0 ? static_cast<size_t>(cap) : 1024 * 1024);
  std::optional<std::chrono::steady_clock::time_point> quiet_deadline;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->until_window.clear();
  }
  for (;;) {
    uint64_t generation = supervisor.Generation();
    std::string chunk = DrainIncremental(job, cap);
    if (chunk != "(no new output)") {
      collected.Push(chunk);
      if (settle) {
        quiet_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      }
    }
    bool terminal = false;
    bool matched = false;
    {
      std::lock_guard<std::mutex> lock(job.session->mutex);
      terminal = ActivityTerminal(job.session->state);
      std::string snapshot = collected.Snapshot();
      matched = !until.empty() &&
                (snapshot.find(until) != std::string::npos ||
                 job.session->until_window.find(until) != std::string::npos);
    }
    if (matched || terminal ||
        (until.empty() && !settle && !collected.Snapshot().empty()) ||
        (quiet_deadline &&
         std::chrono::steady_clock::now() >= *quiet_deadline) ||
        wait_ms <= 0) {
      std::string output = collected.Snapshot();
      return output.empty() ? "(no new output)" : output;
    }
    if (AbortRequested()) return "[wait interrupted; process still running]";
    if (SteeringYieldRequested()) {
      std::string output = collected.Snapshot();
      if (!output.empty()) output += "\n";
      return output +
             "[wait yielded for queued steering; process still running]";
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::string output = collected.Snapshot();
      if (!output.empty()) output += "\n";
      return output + "[wait timed out; process still running]";
    }
    auto wait_deadline =
        quiet_deadline ? std::min(deadline, *quiet_deadline) : deadline;
    supervisor.WaitForChange(generation, wait_deadline);
  }
}

}  // namespace

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor,
                              int64_t pid) {
  std::vector<BgJob> supervised = supervisor.Snapshot();
  if (pid <= 0) {
    return FormatActivityList(supervised, DetachedRecords(),
                              "(no supervised activities)");
  }

  if (std::optional<BgJob> job = supervisor.Find(pid)) {
    std::unique_lock<std::mutex> interaction;
    if (job->session) {
      interaction = std::unique_lock<std::mutex>(job->session->interaction);
    }
    return ToolSuccess(
        "[" + SupervisedJobLabel(*job) + " · activity " +
        std::to_string(ActivityId(*job)) + " · log " + job->log + "]\n" +
        CollectSessionOutput(supervisor, *job, 0, {}, {}, ToolResultCap()));
  }

  std::optional<json> detached = FindDetachedRecord(pid);
  if (!detached) return ActivityNotFound(pid);
  const json& record = *detached;
  std::string status =
      JsonValue(record, "_alive", false) ? "running" : "exited";
  return ToolSuccess(
      "[" + status + " · activity " + std::to_string(pid) + " · " +
      JsonValue(record, "cwd", "") + " · log " + JsonValue(record, "log", "") +
      "]\n" + ReadLogTail(JsonValue(record, "log", ""), ToolResultCap()));
}

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor, int64_t id,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context,
                              int64_t max_output_chars) {
  int64_t cap = max_output_chars > 0
                    ? std::min(max_output_chars, ToolResultCap())
                    : ToolResultCap();
  auto limit_result = [cap](ToolResult result) {
    if (cap > 0 && result.output.size() > static_cast<size_t>(cap)) {
      HeadTailBuffer limited(static_cast<size_t>(cap));
      limited.Push(result.output);
      result.output = limited.Snapshot();
    }
    return result;
  };
  if (id <= 0) return limit_result(ToolActivityOutput(supervisor, id));
  std::optional<BgJob> job = supervisor.Find(id);
  if (!job || !job->session) {
    // Persistent detached activities intentionally remain rotating-log based.
    ToolResult current = ToolActivityOutput(supervisor, id);
    if (!current.Ok() || wait_ms <= 0) return limit_result(std::move(current));
    auto deadline =
        std::min(context.deadline, std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(wait_ms));
    std::string watch_path;
    if (job) {
      watch_path = job->log;
    } else if (std::optional<json> record = FindDetachedRecord(id)) {
      watch_path = JsonValue(*record, "log", "");
    }
    std::string accumulated = current.output;
    for (;;) {
      bool steering_yield = SteeringYieldRequested();
      if ((!until.empty() && accumulated.find(until) != std::string::npos) ||
          std::chrono::steady_clock::now() >= deadline || AbortRequested() ||
          steering_yield) {
        if (steering_yield) {
          if (!accumulated.empty()) accumulated += "\n";
          accumulated +=
              "[wait yielded for queued steering; process still running]";
        }
        return limit_result(ToolSuccess(std::move(accumulated)));
      }
      FileStamp observed = SnapshotFile(watch_path);
      ToolResult next = ToolActivityOutput(supervisor, id);
      if (!next.Ok()) return next;
      if (next.output != current.output) {
        accumulated = next.output;
        if (until.empty()) {
          return limit_result(ToolSuccess(std::move(accumulated)));
        }
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
  std::string output =
      CollectSessionOutput(supervisor, *job, wait_ms, until, context, cap);
  if (output == "(no new output)") {
    std::string replay;
    {
      std::lock_guard<std::mutex> lock(job->session->mutex);
      if (job->session->state == ActivityState::kDelivered) {
        replay = job->session->transcript.Snapshot();
      }
    }
    if (!replay.empty()) {
      HeadTailBuffer limited(cap > 0 ? static_cast<size_t>(cap)
                                     : replay.size());
      limited.Push(replay);
      output = "[complete transcript replay]\n" + limited.Snapshot();
    }
  }
  return ToolSuccess("[" + SupervisedJobLabel(*job) + " · activity " +
                     std::to_string(ActivityId(*job)) + " · log " + job->log +
                     "]\n" + output);
}

ToolResult ToolActivityInput(const ProcessSupervisor& supervisor, int64_t id,
                             const std::string& chars, int64_t wait_ms,
                             const ToolContext& context, int64_t rows,
                             int64_t cols, int64_t max_output_chars) {
  std::optional<BgJob> job = supervisor.Find(id);
  if (!job || !job->session) return ActivityNotFound(id);
  std::shared_ptr<ActivitySession> session = job->session;
  std::lock_guard<std::mutex> interaction(session->interaction);
  int input_fd = -1;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->input_fd >= 0) input_fd = dup(session->input_fd);
    session->last_used = std::chrono::steady_clock::now();
  }
  if (rows > 0 || cols > 0) {
    if (!session->tty || input_fd < 0 || rows <= 0 || cols <= 0 ||
        rows > 1000 || cols > 1000) {
      if (input_fd >= 0) close(input_fd);
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: PTY resize requires rows and cols in 1..1000");
    }
    winsize size{};
    size.ws_row = static_cast<uint16_t>(rows);
    size.ws_col = static_cast<uint16_t>(cols);
    if (ioctl(input_fd, TIOCSWINSZ, &size) != 0) {
      close(input_fd);
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "error: could not resize activity PTY");
    }
    (void)kill(-job->pid, SIGWINCH);
  }
  if (!chars.empty()) {
    if (chars == "\x03") {
      if (kill(-job->pid, SIGINT) != 0 && errno != ESRCH) {
        if (input_fd >= 0) close(input_fd);
        return ToolFailure(
            ToolErrorCode::kProcessFailed,
            "error: could not interrupt activity " + std::to_string(id));
      }
    } else if (!session->tty) {
      if (input_fd >= 0) close(input_fd);
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: activity stdin is closed (run with tty=true)");
    } else {
      size_t offset = 0;
      while (offset < chars.size()) {
        ssize_t count =
            write(input_fd, chars.data() + offset, chars.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
          close(input_fd);
          return ToolFailure(ToolErrorCode::kProcessFailed,
                             "error: could not write activity stdin");
        }
        offset += static_cast<size_t>(count);
      }
    }
  }
  if (input_fd >= 0) close(input_fd);
  int64_t cap = max_output_chars > 0
                    ? std::min(max_output_chars, ToolResultCap())
                    : ToolResultCap();
  std::string output = CollectSessionOutput(supervisor, *job, wait_ms, {},
                                            context, cap, !chars.empty());
  return ToolSuccess("[" + SupervisedJobLabel(*job) + " · activity " +
                     std::to_string(ActivityId(*job)) + " · log " + job->log +
                     "]\n" + output);
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
  if (supervised) {
    log = supervised->log;
    detached = supervised->detached;
  } else {
    std::optional<json> record = FindDetachedRecord(pid);
    if (!record) return ActivityNotFound(pid);
    log = JsonValue(*record, "log", "");
    detached = true;
  }

  bool was_alive = ProcessGroupAlive(pid);
  bool reap_leader = !supervised || !supervised->session;
  if (was_alive) {
    if (!SignalProcessGroup(pid, SIGTERM) ||
        (!WaitForProcessGroupExit(supervisor, pid, std::chrono::seconds(1),
                                  reap_leader) &&
         (!SignalProcessGroup(pid, SIGKILL) ||
          !WaitForProcessGroupExit(supervisor, pid, std::chrono::seconds(1),
                                   reap_leader)))) {
      return ToolFailure(
          ToolErrorCode::kProcessFailed,
          "error: could not stop process group " + std::to_string(pid));
    }
  } else if (detached) {
    ReapLeader(pid);
  }
  if (supervised && supervised->session) {
    std::lock_guard<std::mutex> lock(supervised->session->mutex);
    supervised->session->stop_requested = true;
  }
  if (supervised) (void)supervisor.Take(requested);
  if (!detached) BgTrackSignal(pid, false);
  unlink(DetachedRecordPath(pid).c_str());
  RemoveLog(log);
  supervisor.Wake();
  return ToolSuccess(
      std::string(was_alive ? "stopped process group "
                            : "process group already exited; cleaned pid ") +
      std::to_string(pid));
}

// Drain finished activities exactly once after the process-I/O owner has
// recorded status and drained trailing output.
std::string BgResultHeader(const BgJob& job) {
  ActivityKind kind = job.session ? job.session->kind
                                  : ParseActivityKind(job.kind, job.detached);
  if (kind == ActivityKind::kTask) {
    return "[Background result: task id " + std::to_string(ActivityId(job)) +
           "]";
  }
  return "[" + std::string(job.detached ? "Detached" : "Background") +
         " result: activity id " + std::to_string(ActivityId(job)) + " `" +
         FirstLine(job.cmd) + "`]";
}

std::string BgResultHeader(const BackgroundCompletion& completion) {
  if (completion.kind == ActivityKind::kTask) {
    return "[Background result: task id " +
           std::to_string(completion.activity_id) + "]";
  }
  return "[" +
         std::string(completion.kind == ActivityKind::kDetached
                         ? "Detached"
                         : "Background") +
         " result: activity id " + std::to_string(completion.activity_id) +
         " `" + FirstLine(completion.command) + "`]";
}

namespace {

int64_t AutomaticResultCap() {
  int64_t cap = ToolResultCap();
  return cap > 0 ? std::min<int64_t>(cap, 6000) : int64_t{6000};
}

std::vector<std::string> TakeCompleted(
    ProcessSupervisor& supervisor, std::string_view kind,
    const std::vector<int64_t>* ids, std::vector<BackgroundCompletion>* details,
    int64_t output_cap) {
  std::vector<BgJob> jobs = supervisor.Snapshot();
  std::vector<std::string> notes;
  for (BgJob& candidate : jobs) {
    std::unique_lock<std::mutex> interaction;
    if (candidate.session) {
      interaction =
          std::unique_lock<std::mutex>(candidate.session->interaction);
    }
    if (ids && std::find(ids->begin(), ids->end(), ActivityId(candidate)) ==
                   ids->end()) {
      continue;
    }
    if (!kind.empty() && candidate.kind != kind) continue;

    int status = 0;
    bool completed = false;
    if (candidate.detached) {
      pid_t waited = PollProcess(candidate.pid, &status);
      completed = waited == candidate.pid && !ProcessGroupAlive(candidate.pid);
      if (!completed) continue;
    } else if (candidate.session) {
      std::lock_guard<std::mutex> lock(candidate.session->mutex);
      completed = candidate.session->state == ActivityState::kDrained;
      status = candidate.session->wait_status.value_or(0);
      if (!completed) continue;
    }

    std::optional<BgJob> taken = supervisor.Take(ActivityId(candidate));
    if (!taken) continue;  // another waiter owns exactly-once delivery
    BgJob job = std::move(*taken);
    if (!job.detached) BgTrackSignal(job.pid, false);
    if (job.detached) unlink(DetachedRecordPath(job.pid).c_str());
    std::string incremental =
        job.session ? DrainIncremental(job, output_cap) : std::string();
    CollectedLog collected =
        job.detached
            ? CollectedLog{ReadLogTail(job.log, output_cap), std::nullopt}
            : CollectCompletedLog(job.log, output_cap);
    if (job.detached) RemoveLog(job.log);
    std::string output;
    if (job.session) {
      output = incremental.empty() || incremental == "(no new output)"
                   ? "(no new output)"
                   : std::move(incremental);
    } else {
      output = std::move(collected.output);
    }
    if (collected.artifact) output += ArtifactHint(*collected.artifact);
    std::string formatted =
        BgResultHeader(job) + "\n" + output + FmtExit(status, /*show_ok=*/true);
    notes.push_back(formatted);
    if (details) {
      ActivityKind activity_kind =
          job.session ? job.session->kind
                      : ParseActivityKind(job.kind, job.detached);
      details->push_back({ActivityId(job), activity_kind, status, job.cmd,
                          std::move(output), job.display_label,
                          job.receipt_path, job.source_id});
    }
    if (!job.detached) supervisor.Retain(std::move(job));
  }
  return notes;
}

}  // namespace

std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor,
                                         std::string_view kind) {
  return TakeCompleted(supervisor, kind, nullptr, nullptr,
                       AutomaticResultCap());
}

std::vector<BackgroundCompletion> BgTakeCompletedDetails(
    ProcessSupervisor& supervisor, std::string_view kind) {
  std::vector<BackgroundCompletion> details;
  (void)TakeCompleted(supervisor, kind, nullptr, &details,
                      AutomaticResultCap());
  return details;
}

ToolResult ToolActivityWait(ProcessSupervisor& supervisor,
                            const std::vector<int64_t>& requested,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context,
                            int64_t max_output_chars) {
  std::vector<int64_t> ids;
  if (requested.empty()) {
    for (const BgJob& job : supervisor.Snapshot()) {
      if (!job.detached) ids.push_back(ActivityId(job));
    }
  } else {
    for (int64_t requested_id : requested) {
      if (requested_id <= 0 || !supervisor.Find(requested_id)) {
        return ToolFailure(ToolErrorCode::kNotFound,
                           "error: activity " + std::to_string(requested_id) +
                               " is not running in this session");
      }
      if (std::find(ids.begin(), ids.end(), requested_id) == ids.end()) {
        ids.push_back(requested_id);
      }
    }
  }
  if (ids.empty()) return ToolSuccess("(no waitable activities running)");

  int64_t cap = max_output_chars > 0
                    ? std::min(max_output_chars, ToolResultCap())
                    : ToolResultCap();
  auto limit_output = [cap](std::string result) {
    if (cap > 0 && result.size() > static_cast<size_t>(cap)) {
      HeadTailBuffer limited(static_cast<size_t>(cap));
      limited.Push(result);
      return limited.Snapshot();
    }
    return result;
  };

  auto deadline =
      std::min(context.deadline, std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(wait_ms));
  std::string output;
  for (;;) {
    uint64_t generation = supervisor.Generation();
    std::vector<std::string> completed =
        TakeCompleted(supervisor, {}, &ids, nullptr, cap);
    for (std::string& note : completed) {
      if (!output.empty()) output += "\n\n";
      output += note;
    }
    size_t running = static_cast<size_t>(
        std::count_if(ids.begin(), ids.end(),
                      [&](int64_t id) { return supervisor.IsLive(id); }));
    if ((mode == "any" && !completed.empty()) || running == 0) {
      std::string result =
          output.empty() ? "(activities already complete)" : std::move(output);
      return ToolSuccess(limit_output(std::move(result)));
    }
    if (AbortRequested()) {
      return ToolCancelled("wait interrupted; " + std::to_string(running) +
                           " activity(s) still running");
    }
    if (SteeringYieldRequested()) {
      if (!output.empty()) output += "\n\n";
      output += "[wait yielded for queued steering; " +
                std::to_string(running) + " activity(s) still running]";
      return ToolSuccess(limit_output(std::move(output)));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      if (!output.empty()) output += "\n\n";
      output += "[wait timed out; " + std::to_string(running) +
                " activity(s) still running]";
      return ToolSuccess(limit_output(std::move(output)));
    }
    // Process state changes, Escape, and queued steering all pair with Wake(),
    // so this predicate wait needs no periodic abort polling.
    supervisor.WaitForChange(generation, deadline);
  }
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

size_t BgCancelTasks(ProcessSupervisor& supervisor) {
  size_t cancelled = 0;
  for (const BgJob& candidate : supervisor.Snapshot()) {
    if (candidate.detached || !candidate.session ||
        candidate.session->kind != ActivityKind::kTask) {
      continue;
    }
    std::optional<BgJob> job = supervisor.Take(ActivityId(candidate));
    if (!job) continue;
    ++cancelled;
    SignalProcessGroup(job->pid, SIGTERM);
    if (!WaitForProcessGroupExit(supervisor, job->pid,
                                 std::chrono::milliseconds(500))) {
      SignalProcessGroup(job->pid, SIGKILL);
      (void)WaitForProcessGroupExit(supervisor, job->pid,
                                    std::chrono::milliseconds(500));
    }
    BgTrackSignal(job->pid, false);
    RemoveLog(job->log);
  }
  supervisor.Wake();
  return cancelled;
}

}  // namespace uagent
