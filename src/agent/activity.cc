// Copyright 2026 Timon Gentzsch

#include "include/agent/activity.h"

#include <fcntl.h>
#include <poll.h>
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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/file_services.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/sandbox_watch.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"

namespace uagent {

namespace {

// A wait bounded by the caller's request and by the turn deadline. The note
// says which one ended it: "timed out" alone reads as though the requested
// wait elapsed and invites giving up on an activity that is running normally.
struct WaitWindow {
  // Declared first: the constructor measures the deadline from it.
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  int64_t requested_ms;
  std::chrono::steady_clock::time_point deadline;
  bool capped;

  WaitWindow(int64_t wait_ms, std::chrono::steady_clock::time_point turn_end)
      : requested_ms(wait_ms),
        deadline(
            std::min(turn_end, started + std::chrono::milliseconds(wait_ms))),
        capped(turn_end < started + std::chrono::milliseconds(wait_ms)) {}

  // "<lead> 3s", or "<lead> 3s of 10s requested, capped by the turn deadline".
  std::string Note(std::string_view lead) const {
    const double waited = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - started)
                              .count();
    std::string note = std::string(lead) + " " + FmtDuration(waited);
    if (capped) {
      note += " of " + FmtDuration(static_cast<double>(requested_ms) / 1000.0) +
              " requested, capped by the turn deadline";
    }
    return note;
  }
};

// How long a child agent told to stop is given before it is killed. It has
// already stopped what it started; the time is for closing its MCP servers,
// saving its session and reporting what it spent.

inline constexpr std::string_view kNoNewActivityOutput = "(no new output)";

}  // namespace

void ActivityTrackSignal(pid_t pid, bool add) {
  TrackPid(g_activity_pids, kActivityMax, pid, add);
}

void KillProcess(pid_t pid) {
  if (!StopProcess(pid, true, {})) {
    DebugLog("child_reap_deferred", {{"pid", pid}});
  }
}

// A caller-requested budget never exceeds the global tool-result cap; 0 means
// "whatever the global cap allows".
int64_t ActivityOutputCap(int64_t requested) {
  return requested > 0 ? std::min(requested, ToolResultCap()) : ToolResultCap();
}

std::string LimitOutput(std::string text, int64_t cap) {
  return cap <= 0 ? std::move(text)
                  : HeadTail(std::move(text), static_cast<size_t>(cap));
}

ToolResult LimitOutput(ToolResult result, int64_t cap) {
  result.output = LimitOutput(std::move(result.output), cap);
  return result;
}

std::string ReadLogTail(const std::string& path, int64_t cap) {
  auto tail = [](const std::string& file, int64_t bytes) {
    int64_t start = 0;
    std::string text = bytes == 0 ? "" : ReadFileTail(file, bytes, &start);
    return std::pair{std::move(text), start};
  };
  auto [current, current_start] = tail(path, cap);
  int64_t remaining =
      cap > 0 ? std::max(int64_t{0}, cap - static_cast<int64_t>(current.size()))
              : -1;
  auto [previous, previous_start] = tail(path + ".1", remaining);
  if (current.empty() && previous.empty() && !PathExists(path) &&
      !PathExists(path + ".1")) {
    return "(no output captured: " + path + " missing)";
  }
  std::string s = previous + current;
  if (previous_start > 0 || current_start > 0 ||
      (!previous.empty() && PathExists(path + ".1"))) {
    s = "[rotating log tail]\n" + s;
  }
  return s.empty() ? "(no output)" : s;
}

namespace {
uint64_t LogFileBytes(const std::string& path) {
  std::error_code error;
  uintmax_t bytes = std::filesystem::file_size(path, error);
  if (error) return 0;
  return bytes > std::numeric_limits<uint64_t>::max()
             ? std::numeric_limits<uint64_t>::max()
             : static_cast<uint64_t>(bytes);
}
}  // namespace

void RemoveLog(const std::string& path) {
  unlink(path.c_str());
  unlink((path + ".1").c_str());
}

namespace {
ToolArtifact PromoteLogArtifact(const std::string& path, uint64_t bytes) {
  ScopedTempFile target(UagentDir(kArtifactsDir) + "/output-XXXXXX");
  if (target) {
    fchmod(target.Get(), kPrivateFileMode);
    target.Close();
    if (rename(path.c_str(), target.Path().c_str()) == 0) {
      chmod(target.Path().c_str(), kPrivateFileMode);
      return {target.Release(), bytes};
    }
    int rename_error = errno;
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
}  // namespace

// Completed small logs are disposable. Oversized logs become bounded,
// private artifacts so the model can inspect only the relevant slice instead
// of paying to keep the entire stream in context. Non-detached process logs
// are single files; rotating detached logs have their own persistent lifecycle.
//
// A failed process's log is never disposable, whatever its size. "Small"
// stands in for "already fully reported", which holds only when the caller
// keeps the whole thing: a failure is summarised on its way back, so deleting
// the log destroys the detail the caller needs precisely when it needs it.
std::optional<ToolArtifact> KeepCompletedLog(const std::string& path,
                                             int64_t cap, bool failed) {
  uint64_t bytes = LogFileBytes(path);
  if (bytes > 0 &&
      (failed || (cap > 0 && bytes > static_cast<uint64_t>(cap)))) {
    return PromoteLogArtifact(path, bytes);
  }
  RemoveLog(path);
  return std::nullopt;
}

CollectedLog CollectCompletedLog(const std::string& path, int64_t cap,
                                 bool failed) {
  std::string tail = ReadLogTail(path, cap);
  return {std::move(tail), KeepCompletedLog(path, cap, failed)};
}

// Two half-size segments keep server logs bounded without sending
// SIGXFSZ/SIGPIPE to the server itself.
int PumpLog(int input, const std::string& path, int64_t max_bytes,
            const std::stop_token& stop) {
  int64_t segment = std::max(int64_t{512}, max_bytes / 2);
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kPrivateFileMode));
  if (!fd) return 1;
  std::array<char, KiB(64)> buffer{};
  int64_t written = 0;
  for (;;) {
    // A descendant that outlives its server can hold the pipe open for good;
    // a stoppable pump gives up once nothing more is waiting in it.
    if (stop.stop_possible()) {
      pollfd waiting = {input, POLLIN, 0};
      const int ready = poll(&waiting, 1, kLogPumpPollMs);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) break;
      if (ready == 0) {
        if (stop.stop_requested()) break;
        continue;
      }
    }
    ssize_t count = read(input, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      if (written >= segment) {
        fd.Reset();
        unlink((path + ".1").c_str());
        rename(path.c_str(), (path + ".1").c_str());
        fd.Reset(
            open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kPrivateFileMode));
        if (!fd) return 1;
        written = 0;
      }
      size_t chunk = std::min(static_cast<size_t>(segment - written),
                              static_cast<size_t>(count) - offset);
      ssize_t n = write(fd.Get(), buffer.data() + offset, chunk);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return 1;
      offset += static_cast<size_t>(n);
      written += n;
    }
  }
  return 0;
}

// Hidden subprocess mode used by detached shells.
int ToolLogPump(const std::string& path, int64_t max_bytes) {
  return PumpLog(STDIN_FILENO, path, max_bytes);
}

bool ProcessGroupAlive(pid_t leader) {
  if (leader <= 0) return false;
  // Signalling a group of zombies succeeds on Linux and is refused with EPERM
  // on macOS: either way the answer is in who is left in it.
  if (kill(-leader, 0) == 0 || errno == EPERM) {
    return ProcessGroupHasLiveMember(leader);
  }
  return false;
}

std::string DetachedRecordPath(pid_t pid) {
  return UagentDir(kTerminalsDir) + "/" + std::to_string(pid) + ".json";
}

namespace {

std::filesystem::file_time_type DetachedRecordCutoff() {
  return std::filesystem::file_time_type::clock::now() -
         std::chrono::hours(24 * LogKeepDays());
}

// Reads one terminal record, annotating liveness. A PID is not an identity:
// after reuse it may name an unrelated process group. Only a matching kernel
// start identity makes a persisted record live. Corrupt records and expired
// dead ones are removed with their logs and reported as absent.
std::optional<json> LoadDetachedRecord(const std::filesystem::path& path,
                                       std::filesystem::file_time_type cutoff) {
  namespace fs = std::filesystem;
  // A record holds one command, which a tool argument bounds well below this.
  // Only a record that reads but does not parse is corrupt; one that cannot be
  // read now (a race, no descriptors) may still name a live command.
  std::string bytes, error;
  if (!ReadRegularFile(path.string(), MiB(1), bytes, error)) {
    return std::nullopt;
  }
  json record = json::parse(bytes, nullptr, false);
  std::error_code ec;
  if (record.is_discarded()) {
    fs::remove(path, ec);
    return std::nullopt;
  }
  pid_t pid = JsonValue(record, "pid", pid_t{0});
  std::string recorded_identity = JsonValue(record, "process_identity", "");
  std::string live_identity = ProcessIdentity(pid);
  bool alive = ProcessGroupAlive(pid) && !recorded_identity.empty() &&
               live_identity == recorded_identity;
  record["_alive"] = alive;
  auto modified = fs::last_write_time(path, ec);
  if (!alive && !ec && modified < cutoff) {
    RemoveLog(JsonValue(record, "log", ""));
    std::error_code remove_error;
    fs::remove(path, remove_error);
    return std::nullopt;
  }
  return record;
}

}  // namespace

std::vector<json> DetachedRecords() {
  namespace fs = std::filesystem;
  std::vector<json> records;
  auto cutoff = DetachedRecordCutoff();
  std::error_code ec;
  for (fs::directory_iterator it(UagentDir(kTerminalsDir), ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    if (std::optional<json> record = LoadDetachedRecord(it->path(), cutoff)) {
      records.push_back(std::move(*record));
    }
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
// Records are named after their pid, so one lookup beats scanning, parsing and
// sorting every record — this runs inside wait loops.
std::optional<json> FindDetachedRecord(int64_t pid) {
  if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return std::nullopt;
  return LoadDetachedRecord(DetachedRecordPath(static_cast<pid_t>(pid)),
                            DetachedRecordCutoff());
}
}  // namespace

namespace {
ToolResult ActivityNotFound(int64_t pid) {
  return ToolFailure(
      ToolErrorCode::kNotFound,
      "activity " + std::to_string(pid) + " is not supervised by uagent");
}
}  // namespace

ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& cmd) {
  std::error_code ec;
  std::string cwd = std::filesystem::current_path(ec).string();
  std::string identity = ProcessIdentity(pid);
  if (identity.empty()) {
    return ToolFailure(ToolErrorCode::kProcessFailed,
                       "cannot establish detached process identity");
  }
  json record = {{"pid", pid},           {"process_identity", identity},
                 {"log", log},           {"command", cmd},
                 {"cwd", ec ? "" : cwd}, {"started_at", UtcStamp()}};
  std::string content = JsonDump(record, 2) + "\n";
  std::string path = DetachedRecordPath(pid);
  return ToolAtomicWrite(path, content, kPrivateFileMode,
                         /*preserve_mode=*/true);
}

namespace {
bool SignalProcessGroup(pid_t leader, int signal_number) {
  if (kill(-leader, signal_number) == 0 || errno == EPERM) return true;
  return errno == ESRCH;
}
}  // namespace

namespace {

// The leader's own exit status is not the group's, and nothing reads it: this
// only stops it lingering as a zombie while the group finishes.
void ReapLeader(pid_t leader) {
  int status = 0;
  WaitPid(leader, &status, WNOHANG);
}

bool WaitForProcessGroupExit(ActivitySupervisor& supervisor, pid_t leader,
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

bool TerminateGroup(ActivitySupervisor& supervisor, pid_t leader,
                    std::chrono::milliseconds grace, bool reap_leader) {
  if (!SignalProcessGroup(leader, SIGTERM)) return false;
  if (WaitForProcessGroupExit(supervisor, leader, grace, reap_leader)) {
    return true;
  }
  if (!SignalProcessGroup(leader, SIGKILL)) return false;
  return WaitForProcessGroupExit(supervisor, leader, grace, reap_leader);
}

namespace {

// The hint every activity read carries: who it is, whether it is still worth
// writing to, and where the full log lives.
std::string ActivityHeader(const Activity& job, std::string_view state = {}) {
  return "[" + ActivityKindName(job.kind) + " · activity " +
         std::to_string(ActivityId(job)) + std::string(state) + " · log " +
         job.log + "]";
}

// An activity read: what it printed, and the hint that says whose it is.
ToolResult ActivityRead(std::string output, std::string_view header,
                        std::string_view note = {}) {
  ToolResult result = ToolSuccess(std::move(output));
  result.body_kind = "tail";
  AddHint(result, header);
  AddHint(result, note);
  return result;
}

}  // namespace

static ToolResult FormatActivityList(const std::vector<Activity>& supervised,
                                     const std::vector<json>& records,
                                     std::string_view empty) {
  if (supervised.empty() && records.empty()) {
    return ListResult(std::string(empty), 0, "activity");
  }
  std::string output;
  int64_t count = 0;
  for (const Activity& job : supervised) {
    std::string summary =
        job.display_label.empty() ? FirstLine(job.cmd) : job.display_label;
    output += "[" + ActivityKindName(job.kind) + "] activity " +
              std::to_string(ActivityId(job)) + " · " + summary + " · " +
              job.log + "\n";
    ++count;
  }
  for (const json& record : records) {
    pid_t record_pid = JsonValue(record, "pid", 0);
    if (std::any_of(supervised.begin(), supervised.end(),
                    [record_pid](const Activity& job) {
                      return job.Detached() && job.pid == record_pid;
                    })) {
      continue;
    }
    output += JsonValue(record, "_alive", false) ? "[running] " : "[exited] ";
    output += "activity " + std::to_string(record_pid) + " · " +
              JsonValue(record, "cwd", "") + " · " +
              FirstLine(JsonValue(record, "command", "")) + " · " +
              JsonValue(record, "log", "") + "\n";
    ++count;
  }
  int64_t cap = ToolResultCap();
  const bool cut = cap > 0 && output.size() > static_cast<size_t>(cap);
  if (cut) output = Utf8Trunc(std::move(output), static_cast<size_t>(cap));
  ToolResult result = ListResult(std::move(output), count, "activity");
  if (cut) AddHint(result, "[activity list truncated]");
  return result;
}

namespace {
std::string DrainActivityOutput(const Activity& job, int64_t cap) {
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
}  // namespace

namespace {

// fresh reports whether the returned text is captured output rather than a
// status sentinel, so callers never have to compare against those strings.
// `note`, when given, receives why a wait ended early, for the model.
std::string CollectSessionOutput(const ActivitySupervisor& supervisor,
                                 const Activity& job, int64_t wait_ms,
                                 std::string_view until,
                                 const ToolContext& context, int64_t cap,
                                 bool settle = false, bool* fresh = nullptr,
                                 std::string* note = nullptr) {
  auto done = [fresh](std::string text, bool captured) {
    if (fresh) *fresh = captured;
    return text;
  };
  if (!job.session) {
    std::string tail = ReadLogTail(job.log, cap);
    bool captured = !tail.empty();
    return done(std::move(tail), captured);
  }
  const WaitWindow window(wait_ms, context.deadline);
  HeadTailBuffer collected(cap > 0 ? static_cast<size_t>(cap) : MiB(1));
  std::optional<std::chrono::steady_clock::time_point> quiet_deadline;
  {
    std::lock_guard<std::mutex> lock(job.session->mutex);
    job.session->until_window.clear();
  }
  for (;;) {
    uint64_t generation = supervisor.Generation();
    // Observe stop conditions before draining. A marker or exit arriving
    // after a drain must not end this poll without returning its new bytes.
    bool terminal = false;
    bool matched = false;
    {
      std::lock_guard<std::mutex> lock(job.session->mutex);
      terminal = ActivityTerminal(job.session->state);
      matched = !until.empty() &&
                job.session->until_window.find(until) != std::string::npos;
    }
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
    matched = matched ||
              (!until.empty() && snapshot.find(until) != std::string::npos);
    // keep=false is the abort case: an interrupted wait reports only that,
    // leaving the captured bytes for the next read.
    auto finish = [&](std::string_view why, bool keep = true) {
      bool captured = keep && !snapshot.empty();
      if (note) *note = why;
      if (captured) return done(std::move(snapshot), true);
      return done(
          why.empty() ? std::string(kNoNewActivityOutput) : std::string(),
          false);
    };
    if (matched || terminal ||
        (until.empty() && !settle && !snapshot.empty()) ||
        (quiet_deadline &&
         std::chrono::steady_clock::now() >= *quiet_deadline) ||
        wait_ms <= 0) {
      return finish({});
    }
    if (AbortRequested()) {
      return finish("[wait cancelled; process still running]",
                    /*keep=*/false);
    }
    if (SteeringYieldRequested()) {
      return finish(
          "[wait yielded for queued steering; process still running]");
    }
    if (std::chrono::steady_clock::now() >= window.deadline) {
      return finish("[" + window.Note("waited") + "; process still running]");
    }
    auto wait_deadline = quiet_deadline
                             ? std::min(window.deadline, *quiet_deadline)
                             : window.deadline;
    supervisor.WaitForChange(generation, wait_deadline);
  }
}

}  // namespace

ToolResult ToolActivityOutput(const ActivitySupervisor& supervisor,
                              int64_t pid) {
  if (pid <= 0) {
    // Only the listing needs every activity copied out of the supervisor.
    return FormatActivityList(supervisor.Snapshot(), DetachedRecords(),
                              "(no supervised activities)");
  }

  if (std::optional<Activity> job = supervisor.Find(pid)) {
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
    ToolResult result =
        ActivityRead(std::move(output), ActivityHeader(*job, state));
    if (job->session) {
      std::lock_guard<std::mutex> lock(job->session->mutex);
      if (job->session->wait_status) {
        result.facts.exit = ExitFacts(*job->session->wait_status);
      }
    }
    result.facts.no_change = !fresh;
    result.activity_terminal = terminal;
    return result;
  }

  std::optional<json> detached = FindDetachedRecord(pid);
  if (!detached) return ActivityNotFound(pid);
  const json& record = *detached;
  bool alive = JsonValue(record, "_alive", false);
  std::string status = alive ? "running" : "exited";
  ToolResult result =
      ActivityRead(ReadLogTail(JsonValue(record, "log", ""), ToolResultCap()),
                   "[" + status + " · activity " + std::to_string(pid) + " · " +
                       JsonValue(record, "cwd", "") + " · log " +
                       JsonValue(record, "log", "") + "]");
  result.activity_terminal = !alive;
  return result;
}

ToolResult ToolActivityOutput(const ActivitySupervisor& supervisor, int64_t id,
                              int64_t wait_ms, std::string_view until,
                              const ToolContext& context,
                              int64_t max_output_chars) {
  int64_t cap = ActivityOutputCap(max_output_chars);
  if (id <= 0) return LimitOutput(ToolActivityOutput(supervisor, id), cap);
  // " · " is the prefix StatusLabel keeps when it has to clip.
  TerminalStatusLabel waiting("wait · activity " + std::to_string(id));
  std::optional<Activity> job = supervisor.Find(id);
  if (!job || !job->session) {
    // Persistent detached activities intentionally remain rotating-log based.
    // The activity or record is resolved once here: re-deriving it per
    // iteration would re-parse every terminal record for the length of the
    // wait.
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
    auto reply = [&](std::string body, bool no_change,
                     std::string_view note = {}) {
      bool alive = job ? ProcessGroupAlive(job->pid) : persisted_alive();
      std::string header =
          job ? ActivityHeader(*job)
              : "[" + std::string(alive ? "running" : "exited") +
                    " · activity " + std::to_string(id) + " · " + record_cwd +
                    " · log " + watch_path + "]";
      ToolResult result = ActivityRead(std::move(body), header, note);
      result.facts.no_change = no_change;
      result.activity_terminal = !alive;
      return LimitOutput(std::move(result), cap);
    };
    // Read at the global cap, not the caller's: reply() applies the head/tail
    // limiter, so a lowered max_output_chars still keeps the window's head.
    const int64_t read_cap = ToolResultCap();
    std::string current = ReadLogTail(watch_path, read_cap);
    if (wait_ms <= 0) return reply(std::move(current), false);
    const WaitWindow window(wait_ms, context.deadline);
    std::string accumulated = current;
    for (;;) {
      bool steering_yield = SteeringYieldRequested();
      bool found =
          !until.empty() && accumulated.find(until) != std::string::npos;
      bool expired = std::chrono::steady_clock::now() >= window.deadline;
      if (found || expired || AbortRequested() || steering_yield) {
        bool no_change = accumulated == current && !steering_yield;
        std::string note;
        if (steering_yield) {
          note = "[wait yielded for queued steering; process still running]";
        } else if (expired && !found) {
          // Returning the log alone reads as "here is the state you asked
          // for", when in fact the marker never appeared and the wait may have
          // been cut short by the turn deadline rather than by wait_ms.
          note = "[" +
                 window.Note(until.empty() ? "waited"
                                           : "marker \"" + std::string(until) +
                                                 "\" not seen in") +
                 "; process still running; call again to keep waiting]";
          no_change = false;
        }
        return reply(std::move(accumulated), no_change, note);
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
          WaitForFileChange(watch_path, observed, window.deadline);
      if (changed == FileWaitResult::kInterrupted ||
          changed == FileWaitResult::kSteering ||
          changed == FileWaitResult::kTimedOut) {
        continue;
      }
    }
  }
  std::lock_guard<std::mutex> interaction(job->session->interaction);
  bool fresh = false;
  std::string note;
  std::string output = CollectSessionOutput(supervisor, *job, wait_ms, until,
                                            context, cap, false, &fresh, &note);
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
      output = LimitOutput(std::move(replay), cap);
      note = "[complete transcript replay]";
      no_change = false;
    }
  }
  ToolResult result =
      ActivityRead(std::move(output), ActivityHeader(*job), note);
  result.facts.no_change = no_change;
  {
    std::lock_guard<std::mutex> lock(job->session->mutex);
    result.activity_terminal = job->session->wait_status.has_value() ||
                               ActivityTerminal(job->session->state);
  }
  return result;
}

ToolResult ToolActivityInput(const ActivitySupervisor& supervisor, int64_t id,
                             const std::string& chars, int64_t wait_ms,
                             const ToolContext& context, int64_t rows,
                             int64_t cols, int64_t max_output_chars) {
  std::optional<Activity> job = supervisor.Find(id);
  if (!job || !job->session) return ActivityNotFound(id);
  std::shared_ptr<ActivitySession> session = job->session;
  std::lock_guard<std::mutex> interaction(session->interaction);
  const bool resize = rows > 0 || cols > 0;
  if (resize && (rows <= 0 || cols <= 0 || rows > kPtyMaxDimension ||
                 cols > kPtyMaxDimension)) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "PTY resize requires rows and cols in 1.." +
                           std::to_string(kPtyMaxDimension));
  }

  const bool needs_input =
      resize || (!chars.empty() && chars != std::string_view("\x03"));
  if (needs_input && !session->tty) {
    std::string action =
        resize ? "has no PTY to resize" : "does not accept input";
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "activity " + std::to_string(id) + " " + action +
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
                       "activity " + std::to_string(id) +
                           " input is closed; inspect its output or start a "
                           "new tty=true activity");
  }
  if (needs_input && !input) {
    return ToolFailure(ToolErrorCode::kInternal,
                       "could not access activity " + std::to_string(id) +
                           " input: " + std::strerror(duplicate_error));
  }

  if (resize) {
    winsize size{};
    size.ws_row = static_cast<uint16_t>(rows);
    size.ws_col = static_cast<uint16_t>(cols);
    if (ioctl(input.Get(), TIOCSWINSZ, &size) != 0) {
      int resize_error = errno;
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "could not resize activity " + std::to_string(id) +
                             " PTY: " + std::strerror(resize_error));
    }
    (void)kill(-job->pid, SIGWINCH);
  }
  if (!chars.empty()) {
    if (chars == "\x03") {
      if (kill(-job->pid, SIGINT) != 0 && errno != ESRCH) {
        int interrupt_error = errno;
        return ToolFailure(ToolErrorCode::kProcessFailed,
                           "could not interrupt activity " +
                               std::to_string(id) + ": " +
                               std::strerror(interrupt_error));
      }
    } else if (!WriteAll(input.Get(), chars)) {
      int write_error = errno;
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "could not write activity " + std::to_string(id) +
                             " stdin: " + std::strerror(write_error));
    }
  }
  input.Reset();
  int64_t cap = ActivityOutputCap(max_output_chars);
  std::string note;
  std::string output =
      CollectSessionOutput(supervisor, *job, wait_ms, {}, context, cap,
                           !chars.empty(), nullptr, &note);
  return ActivityRead(std::move(output), ActivityHeader(*job), note);
}

ToolResult ToolActivityStop(ActivitySupervisor& supervisor, int64_t requested) {
  if (requested <= 0) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "id must identify a supervised uagent activity");
  }
  std::optional<Activity> supervised = supervisor.Find(requested);
  if (!supervised && requested > std::numeric_limits<pid_t>::max()) {
    return ActivityNotFound(requested);
  }
  pid_t pid = supervised ? supervised->pid : static_cast<pid_t>(requested);
  std::string log;
  bool detached = false;
  bool persisted_alive = false;
  if (supervised) {
    log = supervised->log;
    detached = supervised->Detached();
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
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "could not stop process group " + std::to_string(pid));
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
  if (!detached) ActivityTrackSignal(pid, false);
  if (detached) unlink(DetachedRecordPath(pid).c_str());
  RemoveLog(log);
  supervisor.Wake();
  return ToolSuccess(
      std::string(was_alive ? "stopped process group "
                            : "process group already exited; cleaned pid ") +
      std::to_string(pid));
}

void ActivityShutdownAll(ActivitySupervisor& supervisor) {
  std::vector<Activity> jobs = supervisor.TakeAllForShutdown();
  std::erase_if(jobs, [](const Activity& job) { return job.Detached(); });
  for (const Activity& job : jobs) SignalProcessGroup(job.pid, SIGTERM);
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!jobs.empty()) {
    uint64_t generation = supervisor.Generation();
    std::erase_if(jobs, [](const Activity& job) {
      if (ProcessGroupAlive(job.pid)) return false;
      ActivityTrackSignal(job.pid, false);
      RemoveLog(job.log);
      return true;
    });
    if (jobs.empty() || !supervisor.WaitForChange(generation, deadline)) {
      break;
    }
  }
  for (const Activity& job : jobs) {
    SignalProcessGroup(job.pid, SIGKILL);
    ActivityTrackSignal(job.pid, false);
    RemoveLog(job.log);
  }
  supervisor.Wake();
}

namespace {

std::string ActivityCount(size_t count) {
  return std::to_string(count) + (count == 1 ? " activity" : " activities");
}

}  // namespace

// Drain finished activities exactly once after the process-I/O owner has
// recorded status and drained trailing output.
namespace {

// An activity is identified by its kind and id, and by what it was asked to
// run.
std::string ActivityName(ActivityKind kind, int64_t id,
                         const std::string& command) {
  return ActivityKindName(kind) + " " + std::to_string(id) + " `" +
         FirstLine(command) + "`";
}

}  // namespace

std::string ActivityResultHeader(const ActivityCompletion& completion) {
  return "[Activity result: " +
         ActivityName(completion.kind, completion.activity_id,
                      completion.command) +
         "]";
}

namespace {

int64_t AutomaticResultCap() {
  int64_t cap = ToolResultCap();
  return cap > 0 ? std::min<int64_t>(cap, kActivityResultChars)
                 : kActivityResultChars;
}

// A detached leader's exit status, from the poll that reaps it until its
// result is taken: no later poll finds a child to ask. Reaping and keeping
// are one step, so two waits at once agree on it. Process-wide, like the
// pids it is kept by.
struct LeaderExits {
  std::mutex mutex;
  std::map<pid_t, int> statuses UAGENT_GUARDED_BY(mutex);
};
LeaderExits& Leaders() {
  static LeaderExits exits;
  return exits;
}

// Whether the leader has ended, with its status when it has.
bool LeaderEnded(pid_t pid, int& status) {
  LeaderExits& exits = Leaders();
  std::lock_guard lock(exits.mutex);
  int reaped = 0;
  const pid_t waited = WaitPid(pid, &reaped, WNOHANG);
  // No child and no status kept: it was never this process's to wait for.
  const bool gone = waited < 0 && errno == ECHILD;
  if (waited == pid) exits.statuses[pid] = reaped;
  const auto found = exits.statuses.find(pid);
  if (found == exits.statuses.end()) return gone;
  status = found->second;
  return true;
}

std::vector<ActivityCompletion> TakeCompleted(ActivitySupervisor& supervisor,
                                              std::string_view kind,
                                              const std::vector<int64_t>* ids,
                                              int64_t output_cap) {
  std::vector<Activity> jobs = supervisor.Snapshot();
  std::vector<ActivityCompletion> completions;
  for (Activity& candidate : jobs) {
    // Filter before locking: both tests read fields that are immutable once the
    // id is assigned, and `interaction` is held for a whole tool interaction,
    // so taking it first blocks this wait on an unrelated activity.
    if (ids && std::find(ids->begin(), ids->end(), ActivityId(candidate)) ==
                   ids->end()) {
      continue;
    }
    if (!kind.empty() && ActivityKindName(candidate.kind) != kind) {
      continue;
    }
    std::unique_lock<std::mutex> interaction;
    if (candidate.session) {
      interaction =
          std::unique_lock<std::mutex>(candidate.session->interaction);
    }

    int status = 0;
    bool completed = false;
    if (candidate.Detached()) {
      // The leader may end long before what it started does.
      completed = LeaderEnded(candidate.pid, status) &&
                  !ProcessGroupAlive(candidate.pid);
      if (!completed) continue;
    } else if (candidate.session) {
      std::lock_guard<std::mutex> lock(candidate.session->mutex);
      completed = candidate.session->state == ActivityState::kDrained;
      status = candidate.session->wait_status.value_or(0);
      if (!completed) continue;
    }

    std::optional<Activity> taken =
        supervisor.Take(ActivityId(candidate), /*retain=*/true);
    if (!taken) continue;  // another waiter owns exactly-once delivery
    Activity job = std::move(*taken);
    if (job.Detached()) {
      std::lock_guard lock(Leaders().mutex);
      Leaders().statuses.erase(job.pid);
    }
    if (!job.Detached()) ActivityTrackSignal(job.pid, false);
    if (job.Detached()) unlink(DetachedRecordPath(job.pid).c_str());
    std::string incremental =
        job.session ? DrainActivityOutput(job, output_cap) : std::string();
    bool failed = !ExitedOk(status);
    CollectedLog collected =
        job.Detached()
            ? CollectedLog{ReadLogTail(job.log, output_cap), std::nullopt}
            : CollectCompletedLog(job.log, output_cap, failed);
    if (job.Detached()) RemoveLog(job.log);
    std::string output;
    if (job.session) {
      output = incremental.empty() || incremental == kNoNewActivityOutput
                   ? std::string(kNoNewActivityOutput)
                   : std::move(incremental);
    } else {
      output = std::move(collected.output);
    }
    // The pointer to the full log rides in the hint, which no cut of the
    // output can squeeze out.
    ToolResult notes;
    if (collected.artifact) AddHint(notes, ArtifactHint(*collected.artifact));
    AddHint(notes, SandboxRestoreNote(job.watch.get(), CanonicalCwd()));
    completions.push_back({ActivityId(job), job.kind, status, job.cmd,
                           std::move(output), std::move(notes.hint),
                           job.display_label, job.receipt_path, job.source_id,
                           job.started_ms});
  }
  return completions;
}

// What a wait took, as one result: each completion's output for a person,
// and for the model whose it was, its notes and how it exited.
ToolResult WaitResult(const std::vector<ActivityCompletion>& completions,
                      std::string_view note, int64_t cap) {
  if (completions.empty()) {
    // A wait that ended on its note found nothing; the note says why.
    ToolResult result =
        ToolSuccess(note.empty() ? "(activities already complete)" : "");
    result.facts.no_change = !note.empty();
    AddHint(result, note);
    return result;
  }
  // Several outputs are told apart by name.
  const bool named = completions.size() > 1;
  std::string output;
  ToolResult notes;
  for (const ActivityCompletion& completion : completions) {
    if (!output.empty()) output += "\n\n";
    if (named) {
      output += ActivityName(completion.kind, completion.activity_id,
                             completion.command) +
                ":\n";
    }
    output += completion.output;
    AddHint(notes, ActivityCompletionText(completion, ""));
  }
  ToolResult result = ToolSuccess(LimitOutput(std::move(output), cap));
  result.body_kind = "tail";
  result.hint = std::move(notes.hint);
  for (const ActivityCompletion& completion : completions) {
    result.ended.push_back(EndedOf(completion));
  }
  AddHint(result, note);
  if (named) {
    result.facts.items = static_cast<int64_t>(completions.size());
    result.facts.unit = "activity";
  } else {
    result.facts.exit = ExitFacts(completions.front().status);
  }
  return result;
}

}  // namespace

std::vector<ActivityCompletion> ActivityTakeCompletedDetails(
    ActivitySupervisor& supervisor, std::string_view kind) {
  return TakeCompleted(supervisor, kind, nullptr, AutomaticResultCap());
}

EndedCommand EndedOf(const ActivityCompletion& completion) {
  return {completion.activity_id, completion.status,
          completion.output == kNoNewActivityOutput ? "" : completion.output,
          static_cast<double>(NowMillis() - completion.started_ms)};
}

std::string ActivityCompletionText(const ActivityCompletion& completion,
                                   std::string_view body) {
  std::string text = ActivityResultHeader(completion);
  const std::string exit = ExitHint(completion.status, /*show_ok=*/true);
  for (std::string_view part :
       {body, std::string_view(completion.hint), std::string_view(exit)}) {
    if (!part.empty()) (text += '\n') += part;
  }
  return text;
}

ToolResult ToolActivityWait(ActivitySupervisor& supervisor,
                            const std::vector<int64_t>& requested,
                            std::string_view mode, int64_t wait_ms,
                            const ToolContext& context,
                            int64_t max_output_chars) {
  // Waiting consumes the completion it observes. Memory extraction is drained
  // by the harness into the memory audit instead, so a wait that scooped one
  // up would silently lose that record. A detached activity is waited for
  // when it is named: it may be a server that never ends, so a wait for
  // everything leaves it out.
  auto waitable = [](const Activity& job) {
    return job.kind != ActivityKind::kMemory;
  };
  std::vector<int64_t> ids;
  if (requested.empty()) {
    for (const Activity& job : supervisor.Snapshot()) {
      if (waitable(job) && !job.Detached()) ids.push_back(ActivityId(job));
    }
  } else {
    for (int64_t requested_id : requested) {
      std::optional<Activity> job =
          requested_id > 0 ? supervisor.Find(requested_id) : std::nullopt;
      if (!job) {
        return ToolFailure(ToolErrorCode::kNotFound,
                           "activity " + std::to_string(requested_id) +
                               " is not running in this session");
      }
      if (!waitable(*job)) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "activity " + std::to_string(requested_id) +
                               " is harness maintenance and is not waitable");
      }
      if (std::find(ids.begin(), ids.end(), requested_id) == ids.end()) {
        ids.push_back(requested_id);
      }
    }
  }
  if (ids.empty()) return ToolSuccess("(no waitable activities running)");

  TerminalStatusLabel waiting(
      ids.size() == 1 ? "wait · activity " + std::to_string(ids.front())
                      : "wait · " + ActivityCount(ids.size()));

  int64_t cap = ActivityOutputCap(max_output_chars);

  // The caller's wait_ms is only half the story: a tool call may not outlive
  // context.deadline, which for this tool is the turn's rather than the
  // per-call budget. Both ends are reported below, because "timed out" alone
  // reads as though the requested wait elapsed and invites the caller to give
  // up on an activity that is running normally.
  const WaitWindow window(wait_ms, context.deadline);
  std::vector<ActivityCompletion> taken;
  for (;;) {
    uint64_t generation = supervisor.Generation();
    std::vector<ActivityCompletion> completed =
        TakeCompleted(supervisor, {}, &ids, cap);
    for (ActivityCompletion& completion : completed) {
      taken.push_back(std::move(completion));
    }
    size_t running = static_cast<size_t>(
        std::count_if(ids.begin(), ids.end(),
                      [&](int64_t id) { return supervisor.IsLive(id); }));
    if ((mode == "any" && !completed.empty()) || running == 0) {
      return WaitResult(taken, {}, cap);
    }
    if (AbortRequested()) {
      return ToolCancelled("wait cancelled; " + ActivityCount(running) +
                           " still running");
    }
    if (SteeringYieldRequested()) {
      return WaitResult(taken,
                        "[wait yielded for queued steering; " +
                            ActivityCount(running) + " still running]",
                        cap);
    }
    if (std::chrono::steady_clock::now() >= window.deadline) {
      return WaitResult(taken,
                        "[" + window.Note("waited") + "; " +
                            ActivityCount(running) +
                            " still running; call again to keep waiting]",
                        cap);
    }
    // Process state changes, Escape, and queued steering all pair with Wake(),
    // so this predicate wait needs no periodic abort polling.
    supervisor.WaitForChange(generation, window.deadline);
  }
}

}  // namespace uagent
