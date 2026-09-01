// Copyright 2026 Timon Gentzsch

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
#include <utility>
#include <vector>

#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"

namespace uagent {

void BgTrackSignal(pid_t pid, bool add) {
  TrackPid(g_bg_pids, kBgMax, pid, add);
}

void KillProcess(pid_t pid) {
  SignalProcessGroup(pid, SIGKILL);
  if (!ReapPidFor(pid, nullptr, std::chrono::milliseconds(500))) {
    DebugLog("child_reap_deferred", {{"pid", pid}});
  }
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

// A caller-requested budget never exceeds the global tool-result cap; 0 means
// "whatever the global cap allows".
int64_t ActivityOutputCap(int64_t requested) {
  return requested > 0 ? std::min(requested, ToolResultCap()) : ToolResultCap();
}

// Over-cap text keeps its head and its tail: the middle is what a reader can
// most afford to lose.
std::string LimitOutput(std::string text, int64_t cap) {
  if (cap <= 0 || text.size() <= static_cast<size_t>(cap)) return text;
  HeadTailBuffer limited(static_cast<size_t>(cap));
  limited.Push(text);
  return limited.Snapshot();
}

ToolResult LimitOutput(ToolResult result, int64_t cap) {
  result.output = LimitOutput(std::move(result.output), cap);
  return result;
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
  Fd fd(CreateTempFile(UagentDir(kArtifactsDir) + "/output-XXXXXX", target));
  if (fd) {
    fchmod(fd.Get(), kPrivateFileMode);
    fd.Reset();
    if (rename(path.c_str(), target.c_str()) == 0) {
      chmod(target.c_str(), kPrivateFileMode);
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
//
// A failed process's log is never disposable, whatever its size. "Small"
// stands in for "already fully reported", which holds only when the caller
// keeps the whole thing: a failure is summarised on its way back, so deleting
// the log destroys the detail the caller needs precisely when it needs it.
CollectedLog CollectCompletedLog(const std::string& path, int64_t cap,
                                 bool failed) {
  uint64_t bytes = LogFileBytes(path);
  CollectedLog collected{ReadLogTail(path, cap), std::nullopt};
  if (bytes > 0 &&
      (failed || (cap > 0 && bytes > static_cast<uint64_t>(cap)))) {
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
  Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, kPrivateFileMode));
  if (!fd) return 1;
  std::array<char, size_t{64} * 1024> buffer{};
  int64_t written = 0;
  for (;;) {
    ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
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

bool ProcessGroupAlive(pid_t leader) {
  if (leader <= 0) return false;
  if (kill(-leader, 0) == 0) return true;
  return errno == EPERM;
}

std::string DetachedRecordPath(pid_t pid) {
  return UagentDir(kTerminalsDir) + "/" + std::to_string(pid) + ".json";
}

namespace {

std::filesystem::file_time_type DetachedRecordCutoff() {
  return std::filesystem::file_time_type::clock::now() -
         std::chrono::hours(24 * TerminalRecordDays());
}

// Reads one terminal record, annotating liveness. A PID is not an identity:
// after reuse it may name an unrelated process group. Only a matching kernel
// start identity makes a persisted record live. Corrupt records and expired
// dead ones are removed with their logs and reported as absent.
std::optional<json> LoadDetachedRecord(const std::filesystem::path& path,
                                       std::filesystem::file_time_type cutoff) {
  namespace fs = std::filesystem;
  std::ifstream input(path);
  if (!input) return std::nullopt;
  json record = json::parse(input, nullptr, false);
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

// Records are named after their pid, so one lookup beats scanning, parsing and
// sorting every record — this runs inside wait loops.
std::optional<json> FindDetachedRecord(int64_t pid) {
  if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return std::nullopt;
  return LoadDetachedRecord(DetachedRecordPath(static_cast<pid_t>(pid)),
                            DetachedRecordCutoff());
}

ToolResult ActivityNotFound(int64_t pid) {
  return ToolFailure(ToolErrorCode::kNotFound,
                     "error: activity " + std::to_string(pid) +
                         " is not supervised by uagent");
}

ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
                              const std::string& cmd) {
  std::error_code ec;
  std::string cwd = std::filesystem::current_path(ec).string();
  std::string identity = ProcessIdentity(pid);
  if (identity.empty()) {
    return ToolFailure(ToolErrorCode::kProcessFailed,
                       "error: cannot establish detached process identity");
  }
  json record = {{"pid", pid},           {"process_identity", identity},
                 {"log", log},           {"command", cmd},
                 {"cwd", ec ? "" : cwd}, {"started_at", UtcStamp()}};
  std::string content = JsonDump(record, 2) + "\n";
  std::string path = DetachedRecordPath(pid);
  return ToolAtomicWrite(path, content, kPrivateFileMode,
                         /*preserve_mode=*/true);
}

}  // namespace uagent
