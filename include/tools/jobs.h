// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_JOBS_H_
#define UAGENT_INCLUDE_TOOLS_JOBS_H_
// Job log tailing, detached-terminal records, and automatic task collection.
// Memory stays bounded because only the tail of a log is ever read.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

inline void BgTrackSignal(pid_t pid, bool add) {
  TrackPid(g_bg_pids, kBgMax, pid, add);
}

// "[exit code N]" / "[killed by signal N]" suffix; "" for a clean exit unless
// show_ok
inline std::string FmtExit(int status, bool show_ok) {
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

inline ToolResult ProcessResult(std::string output, int status) {
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return ToolSuccess(std::move(output));
  }
  return ToolFailure(ToolErrorCode::kProcessFailed, std::move(output));
}

inline std::string ReadLogTail(const std::string& path, int64_t cap) {
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

inline uint64_t LogFileBytes(const std::string& path) {
  std::error_code error;
  uintmax_t bytes = std::filesystem::file_size(path, error);
  if (error) return 0;
  return bytes > std::numeric_limits<uint64_t>::max()
             ? std::numeric_limits<uint64_t>::max()
             : static_cast<uint64_t>(bytes);
}

inline void RemoveLog(const std::string& path) {
  unlink(path.c_str());
  unlink((path + ".1").c_str());
}

inline ToolArtifact PromoteLogArtifact(const std::string& path,
                                       uint64_t bytes) {
  std::string pattern = UagentDir(kArtifactsDir) + "/output-XXXXXX";
  std::vector<char> target(pattern.begin(), pattern.end());
  target.push_back('\0');
  int fd = mkstemp(target.data());
  if (fd >= 0) {
    fchmod(fd, 0600);
    close(fd);
    if (rename(path.c_str(), target.data()) == 0) {
      chmod(target.data(), 0600);
      return {target.data(), bytes};
    }
    int rename_error = errno;
    unlink(target.data());
    if (g_debug.Enabled()) {
      g_debug.Write("artifact_promotion_failed",
                    {{"path", path}, {"error", strerror(rename_error)}});
    }
  } else if (g_debug.Enabled()) {
    g_debug.Write("artifact_promotion_failed",
                  {{"path", path}, {"error", strerror(errno)}});
  }
  // Failure must not destroy the only recoverable copy.
  return {path, bytes};
}

struct CollectedLog {
  std::string output;
  std::optional<ToolArtifact> artifact;
};

// Completed small logs are disposable. Oversized logs become bounded,
// private artifacts so the model can inspect only the relevant slice instead
// of paying to keep the entire stream in context. Non-detached process logs
// are single files; rotating detached logs have their own persistent lifecycle.
inline CollectedLog CollectCompletedLog(const std::string& path, int64_t cap) {
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
inline int ToolLogPump(const std::string& path, int64_t max_bytes) {
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

inline bool ProcessAlive(pid_t pid) {
  return pid > 0 && kill(pid, 0) == 0 && getpgid(pid) == pid;
}

inline std::string DetachedRecordPath(pid_t pid) {
  return UagentDir("terminals") + "/" + std::to_string(pid) + ".json";
}

inline std::vector<json> DetachedRecords() {
  namespace fs = std::filesystem;
  std::vector<json> records;
  auto cutoff = fs::file_time_type::clock::now() -
                std::chrono::hours(24 * TerminalRecordDays());
  std::error_code ec;
  for (fs::directory_iterator it(UagentDir("terminals"), ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".json") continue;
    std::ifstream input(it->path());
    json record = json::parse(input, nullptr, false);
    if (record.is_discarded()) {
      fs::remove(it->path(), ec);
      continue;
    }
    record["_alive"] = ProcessAlive(JsonValue(record, "pid", 0));
    std::error_code time_error;
    auto modified = fs::last_write_time(it->path(), time_error);
    if (!record["_alive"].get<bool>() && !time_error && modified < cutoff) {
      fs::remove(JsonValue(record, "log", ""), ec);
      fs::remove(JsonValue(record, "log", "") + ".1", ec);
      fs::remove(it->path(), ec);
      continue;
    }
    records.push_back(std::move(record));
  }
  std::sort(records.begin(), records.end(), [](const json& a, const json& b) {
    return JsonValue(a, "started_at", "") > JsonValue(b, "started_at", "");
  });
  return records;
}

inline ToolResult SaveDetachedRecord(pid_t pid, const std::string& log,
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

inline ToolResult ToolTerminalOutput(int64_t pid) {
  std::vector<json> records = DetachedRecords();
  if (pid <= 0) {
    if (records.empty()) return ToolSuccess("(no detached terminals)");
    std::string out;
    for (const json& record : records) {
      pid_t record_pid = JsonValue(record, "pid", 0);
      out += (JsonValue(record, "_alive", false) ? "[running] " : "[exited] ");
      out += "pid " + std::to_string(record_pid) + " · " +
             JsonValue(record, "cwd", "") + " · " +
             FirstLine(JsonValue(record, "command", "")) + " · " +
             JsonValue(record, "log", "") + "\n";
    }
    int64_t cap = ToolResultCap();
    if (cap > 0 && out.size() > static_cast<size_t>(cap)) {
      out = Utf8Trunc(std::move(out), static_cast<size_t>(cap));
      out += "\n[terminal list truncated]";
    }
    return ToolSuccess(std::move(out));
  }

  auto found =
      std::find_if(records.begin(), records.end(), [&](const json& record) {
        return JsonValue(record, "pid", int64_t{0}) == pid;
      });
  if (found == records.end()) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: pid " + std::to_string(pid) +
                           " is not a uagent detached terminal");
  }
  const json& record = *found;
  std::string status =
      JsonValue(record, "_alive", false) ? "running" : "exited";
  return ToolSuccess(
      "[" + status + " · pid " + std::to_string(pid) + " · " +
      JsonValue(record, "cwd", "") + " · log " + JsonValue(record, "log", "") +
      "]\n" + ReadLogTail(JsonValue(record, "log", ""), ToolResultCap()));
}

// Drain finished bg jobs: return notification strings for any completed pids.
// Called at step boundaries and in the REPL loop — no threads needed.
inline std::string BgResultHeader(const BgJob& job) {
  if (job.kind == "task") {
    return "[Background result: task id " + std::to_string(job.pid) + "]";
  }
  return "[" + std::string(job.detached ? "Detached" : "Background") +
         " result: pid " + std::to_string(job.pid) + " `" + FirstLine(job.cmd) +
         "`]";
}

inline std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  std::vector<std::string> notes;
  for (BgJob& job : jobs) {
    int status = 0;
    if (waitpid(job.pid, &status, WNOHANG) != job.pid) {
      supervisor.Restore(std::move(job));
      continue;
    }
    if (!job.detached) BgTrackSignal(job.pid, false);
    CollectedLog collected =
        job.detached
            ? CollectedLog{ReadLogTail(job.log, ToolResultCap()), std::nullopt}
            : CollectCompletedLog(job.log, ToolResultCap());
    std::string output = std::move(collected.output);
    if (collected.artifact) output += ArtifactHint(*collected.artifact);
    notes.push_back(BgResultHeader(job) + "\n" + output +
                    FmtExit(status, /*show_ok=*/true));
  }
  return notes;
}

inline void BgShutdownAll(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  std::erase_if(jobs, [](const BgJob& job) {
    if (!job.detached) return false;
    int status = 0;
    waitpid(job.pid, &status, WNOHANG);
    return true;
  });
  for (const BgJob& job : jobs) {
    if (kill(-job.pid, SIGTERM) != 0) kill(job.pid, SIGTERM);
  }
  for (int attempt = 0; attempt < 10 && !jobs.empty(); ++attempt) {
    for (auto it = jobs.begin(); it != jobs.end();) {
      int status = 0;
      if (waitpid(it->pid, &status, WNOHANG) == it->pid) {
        BgTrackSignal(it->pid, false);
        unlink(it->log.c_str());
        it = jobs.erase(it);
      } else {
        ++it;
      }
    }
    if (!jobs.empty()) usleep(50 * 1000);
  }
  for (const BgJob& job : jobs) {
    kill(-job.pid, SIGKILL);
    kill(job.pid, SIGKILL);
    int status = 0;
    waitpid(job.pid, &status, 0);
    BgTrackSignal(job.pid, false);
    unlink(job.log.c_str());
  }
}

inline ProcessSupervisor::~ProcessSupervisor() { BgShutdownAll(*this); }

inline size_t BgCancelTasks(ProcessSupervisor& supervisor) {
  std::vector<BgJob> jobs = supervisor.TakeAll();
  size_t cancelled = 0;
  for (BgJob& job : jobs) {
    if (job.detached || job.kind != "task") {
      supervisor.Restore(std::move(job));
      continue;
    }
    ++cancelled;
    int status = 0;
    pid_t result = waitpid(job.pid, &status, WNOHANG);
    if (result < 0 && kill(job.pid, 0) == 0) result = 0;
    if (result == 0) {
      if (kill(-job.pid, SIGTERM) != 0) kill(job.pid, SIGTERM);
      for (int attempt = 0; attempt < 10; ++attempt) {
        usleep(50 * 1000);
        result = waitpid(job.pid, &status, WNOHANG);
        if (result == job.pid) break;
      }
    }
    if (result == 0) {
      if (kill(-job.pid, SIGKILL) != 0) kill(job.pid, SIGKILL);
      waitpid(job.pid, &status, 0);
    }
    BgTrackSignal(job.pid, false);
    unlink(job.log.c_str());
    unlink((job.log + ".1").c_str());
  }
  return cancelled;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_JOBS_H_
