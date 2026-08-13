// Copyright 2026 Timon Gentzsch

#include "include/tools/jobs.h"

#include <fcntl.h>
#include <signal.h>
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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
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

bool WaitForProcessGroupExit(pid_t leader, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    ReapLeader(leader);
    if (!ProcessGroupAlive(leader)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ReapLeader(leader);
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
  if (job.kind == "task") return "task";
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
    output += "[" + SupervisedJobLabel(job) + "] activity " +
              std::to_string(job.pid) + " · " + FirstLine(job.cmd) + " · " +
              job.log + "\n";
  }
  for (const json& record : records) {
    pid_t record_pid = JsonValue(record, "pid", 0);
    if (std::any_of(
            supervised.begin(), supervised.end(),
            [record_pid](const BgJob& job) { return job.pid == record_pid; })) {
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

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor,
                              int64_t pid) {
  std::vector<BgJob> supervised = supervisor.Snapshot();
  if (pid <= 0) {
    return FormatActivityList(supervised, DetachedRecords(),
                              "(no supervised activities)");
  }

  if (std::optional<BgJob> job = supervisor.Find(static_cast<pid_t>(pid))) {
    return ToolSuccess("[" + SupervisedJobLabel(*job) + " · activity " +
                       std::to_string(pid) + " · log " + job->log + "]\n" +
                       ReadLogTail(job->log, ToolResultCap()));
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

ToolResult ToolActivityOutput(const ProcessSupervisor& supervisor, int64_t pid,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context) {
  ToolResult current = ToolActivityOutput(supervisor, pid);
  if (pid <= 0 || wait_ms <= 0 || !current.Ok()) return current;
  auto running = [](const std::string& output) {
    return output.starts_with("[background ·") ||
           output.starts_with("[detached ·") || output.starts_with("[task ·") ||
           output.starts_with("[running ·");
  };
  if (!running(current.output)) return current;
  bool detached = false;
  if (std::optional<BgJob> job = supervisor.Find(static_cast<pid_t>(pid))) {
    detached = job->detached;
  }
  if (!until.empty() && current.output.find(until) != std::string::npos) {
    return current;
  }
  // WNOWAIT leaves an exited child waitable, so the supervisor still reaps it
  // and delivers its result at the next step boundary.
  auto finished = [pid] {
    siginfo_t status{};
    return waitid(P_PID, static_cast<id_t>(pid), &status,
                  WEXITED | WNOHANG | WNOWAIT) == 0 &&
           status.si_pid == pid;
  };
  auto deadline =
      std::min(context.deadline, std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(wait_ms));
  const std::string initial = current.output;
  for (;;) {
    if (finished()) {
      current.output += detached
                            ? "\n[process wrapper exited; its process group "
                              "will be reconciled next]"
                            : "\n[process exited; result will be delivered "
                              "next]";
      return current;
    }
    if (AbortRequested()) {
      return ToolCancelled("wait interrupted; process still running");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      current.output += "\n[wait timed out; process still running]";
      return current;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    current = ToolActivityOutput(supervisor, pid);
    if (!current.Ok() || !running(current.output)) {
      return current;
    }
    if (!until.empty()) {
      if (current.output.find(until) != std::string::npos) return current;
    } else if (current.output != initial) {
      return current;
    }
  }
}

ToolResult ToolActivityStop(ProcessSupervisor& supervisor, int64_t requested) {
  if (requested <= 0 || requested > std::numeric_limits<pid_t>::max()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: id must identify a supervised uagent activity");
  }
  pid_t pid = static_cast<pid_t>(requested);
  std::optional<BgJob> supervised = supervisor.Take(pid);
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
  if (was_alive) {
    if (!SignalProcessGroup(pid, SIGTERM) ||
        (!WaitForProcessGroupExit(pid, 20) &&
         (!SignalProcessGroup(pid, SIGKILL) ||
          !WaitForProcessGroupExit(pid, 20)))) {
      if (supervised) supervisor.Restore(std::move(*supervised));
      return ToolFailure(
          ToolErrorCode::kProcessFailed,
          "error: could not stop process group " + std::to_string(pid));
    }
  } else {
    ReapLeader(pid);
  }
  if (!detached) BgTrackSignal(pid, false);
  unlink(DetachedRecordPath(pid).c_str());
  RemoveLog(log);
  return ToolSuccess(
      std::string(was_alive ? "stopped process group "
                            : "process group already exited; cleaned pid ") +
      std::to_string(pid));
}

// Drain finished bg jobs: return notification strings for any completed pids.
// Called at step boundaries and in the REPL loop — no threads needed.
std::string BgResultHeader(const BgJob& job) {
  if (job.kind == "task") {
    return "[Background result: task id " + std::to_string(job.pid) + "]";
  }
  return "[" + std::string(job.detached ? "Detached" : "Background") +
         " result: activity id " + std::to_string(job.pid) + " `" +
         FirstLine(job.cmd) + "`]";
}

namespace {

std::vector<std::string> TakeCompleted(
    ProcessSupervisor& supervisor, std::string_view kind,
    const std::vector<pid_t>* ids = nullptr) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  std::vector<std::string> notes;
  for (BgJob& job : jobs) {
    if (ids && std::find(ids->begin(), ids->end(), job.pid) == ids->end()) {
      supervisor.Restore(std::move(job));
      continue;
    }
    if (!kind.empty() && job.kind != kind) {
      supervisor.Restore(std::move(job));
      continue;
    }
    int status = job.leader_status.value_or(0);
    pid_t waited = job.leader_status ? job.pid : PollProcess(job.pid, &status);
    int wait_error = waited < 0 ? errno : 0;
    if (waited == 0) {
      supervisor.Restore(std::move(job));
      continue;
    }
    if (job.detached && ProcessGroupAlive(job.pid)) {
      if (waited == job.pid) {
        job.leader_status = status;
      }
      supervisor.Restore(std::move(job));
      continue;
    }
    if (!job.detached) BgTrackSignal(job.pid, false);
    if (job.detached) unlink(DetachedRecordPath(job.pid).c_str());
    CollectedLog collected =
        job.detached
            ? CollectedLog{ReadLogTail(job.log, ToolResultCap()), std::nullopt}
            : CollectCompletedLog(job.log, ToolResultCap());
    if (job.detached) RemoveLog(job.log);
    std::string output = std::move(collected.output);
    if (collected.artifact) output += ArtifactHint(*collected.artifact);
    std::string exit = waited == job.pid
                           ? FmtExit(status, /*show_ok=*/true)
                           : "\n[process status unavailable: " +
                                 std::string(strerror(wait_error)) + "]";
    notes.push_back(BgResultHeader(job) + "\n" + output + exit);
  }
  return notes;
}

}  // namespace

std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor,
                                         std::string_view kind) {
  return TakeCompleted(supervisor, kind);
}

ToolResult ToolActivityWait(ProcessSupervisor& supervisor,
                            const std::vector<int64_t>& requested,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context) {
  std::vector<pid_t> ids;
  if (requested.empty()) {
    for (const BgJob& job : supervisor.Snapshot()) {
      if (!job.detached) ids.push_back(job.pid);
    }
  } else {
    for (int64_t requested_id : requested) {
      if (requested_id <= 0 ||
          requested_id > std::numeric_limits<pid_t>::max() ||
          !supervisor.Find(static_cast<pid_t>(requested_id))) {
        return ToolFailure(ToolErrorCode::kNotFound,
                           "error: activity " + std::to_string(requested_id) +
                               " is not running in this session");
      }
      pid_t id = static_cast<pid_t>(requested_id);
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
  }
  if (ids.empty()) return ToolSuccess("(no waitable activities running)");

  auto deadline =
      std::min(context.deadline, std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(wait_ms));
  std::string output;
  for (;;) {
    std::vector<std::string> completed = TakeCompleted(supervisor, {}, &ids);
    for (std::string& note : completed) {
      if (!output.empty()) output += "\n\n";
      output += note;
    }
    size_t running = static_cast<size_t>(std::count_if(
        ids.begin(), ids.end(), [&](pid_t id) { return supervisor.Find(id); }));
    if ((mode == "any" && !completed.empty()) || running == 0) {
      return ToolSuccess(output.empty() ? "(activities already complete)"
                                        : std::move(output));
    }
    if (AbortRequested()) {
      return ToolCancelled("wait interrupted; " + std::to_string(running) +
                           " activity(s) still running");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      if (!output.empty()) output += "\n\n";
      output += "[wait timed out; " + std::to_string(running) +
                " activity(s) still running]";
      return ToolSuccess(std::move(output));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void BgShutdownAll(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  std::erase_if(jobs, [](const BgJob& job) {
    if (!job.detached) return false;
    int status = 0;
    PollProcess(job.pid, &status);
    return true;
  });
  for (const BgJob& job : jobs) {
    SignalProcessGroup(job.pid, SIGTERM);
  }
  for (int attempt = 0; attempt < 10 && !jobs.empty(); ++attempt) {
    for (auto it = jobs.begin(); it != jobs.end();) {
      int status = 0;
      if (PollProcess(it->pid, &status) == it->pid) {
        BgTrackSignal(it->pid, false);
        RemoveLog(it->log);
        it = jobs.erase(it);
      } else {
        ++it;
      }
    }
    if (!jobs.empty()) usleep(50 * 1000);
  }
  for (const BgJob& job : jobs) {
    int status = 0;
    KillProcess(job.pid, &status);
    BgTrackSignal(job.pid, false);
    RemoveLog(job.log);
  }
}

ProcessSupervisor::~ProcessSupervisor() { BgShutdownAll(*this); }

size_t BgCancelTasks(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  size_t cancelled = 0;
  for (BgJob& job : jobs) {
    if (job.detached || job.kind != "task") {
      supervisor.Restore(std::move(job));
      continue;
    }
    ++cancelled;
    int status = 0;
    pid_t result = PollProcess(job.pid, &status);
    if (result < 0 && kill(job.pid, 0) == 0) result = 0;
    if (result == 0) {
      SignalProcessGroup(job.pid, SIGTERM);
      for (int attempt = 0; attempt < 10; ++attempt) {
        usleep(50 * 1000);
        result = PollProcess(job.pid, &status);
        if (result == job.pid) break;
      }
    }
    if (result == 0) {
      KillProcess(job.pid, &status);
    }
    BgTrackSignal(job.pid, false);
    RemoveLog(job.log);
  }
  return cancelled;
}

}  // namespace uagent
