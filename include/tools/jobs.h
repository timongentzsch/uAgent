// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_JOBS_H_
#define UAGENT_INCLUDE_TOOLS_JOBS_H_
// Job log tailing, detached-terminal records, and the wait tools. Memory
// stays bounded because only the tail of a log is ever read.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

inline uintmax_t LogBytes(const std::string& path) {
  std::error_code ec;
  uintmax_t bytes = std::filesystem::file_size(path, ec);
  return ec ? 0 : bytes;
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
inline std::vector<std::string> BgTakeCompleted(ProcessSupervisor& supervisor) {
  return supervisor.WithJobs([](std::vector<BgJob>& jobs) {
    std::vector<std::string> notes;
    for (auto it = jobs.begin(); it != jobs.end();) {
      int st;
      if (waitpid(it->pid, &st, WNOHANG) == it->pid) {
        if (!it->detached) BgTrackSignal(it->pid, false);
        std::string out = ReadLogTail(it->log, ToolResultCap());
        if (!it->detached) unlink(it->log.c_str());
        std::string note =
            "[" + std::string(it->detached ? "Detached" : "Background") +
            " result: pid " + std::to_string(it->pid) + " `" +
            FirstLine(it->cmd) + "`]\n" + out + FmtExit(st, /*show_ok=*/true);
        notes.push_back(std::move(note));
        it = jobs.erase(it);
      } else {
        ++it;
      }
    }
    return notes;
  });
}

inline void BgShutdownAll(ProcessSupervisor& supervisor) {
  supervisor.WithJobs([](std::vector<BgJob>& jobs) {
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
    jobs.clear();
  });
}

inline ProcessSupervisor::~ProcessSupervisor() { BgShutdownAll(*this); }

inline bool IsTrackedTask(ProcessSupervisor& supervisor, int64_t id) {
  return supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
    return std::any_of(jobs.begin(), jobs.end(), [&](const BgJob& job) {
      return job.pid == static_cast<pid_t>(id) && job.kind == "task";
    });
  });
}

// Reap one completed task without retaining another copy of its bounded log.
inline std::optional<ToolResult> TakeCompletedTask(
    ProcessSupervisor& supervisor, int64_t id) {
  pid_t process = static_cast<pid_t>(id);
  std::optional<ToolResult> output;
  supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
    auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BgJob& job) {
      return job.pid == process && job.kind == "task";
    });
    if (it == jobs.end()) return;
    int status = 0;
    pid_t result = waitpid(process, &status, WNOHANG);
    bool finished = result == process || (result < 0 && kill(process, 0) != 0);
    if (!finished) return;
    std::string out = ReadLogTail(it->log, ToolResultCap());
    if (result == process) {
      out += FmtExit(status, /*show_ok=*/true);
      output = ProcessResult(std::move(out), status);
    } else {
      out = "[process exited — status unavailable]\n" + out;
      output = ToolFailure(ToolErrorCode::kProcessFailed, std::move(out));
    }
    unlink(it->log.c_str());
    unlink((it->log + ".1").c_str());
    jobs.erase(it);
  });
  if (output) BgTrackSignal(process, false);
  return output;
}

inline ToolResult ToolWaitBackground(ProcessSupervisor& supervisor, int64_t pid,
                                     const ToolContext& context);

inline ToolResult ToolGetTaskOutput(ProcessSupervisor& supervisor, int64_t id) {
  if (id <= 0 || !IsTrackedTask(supervisor, id)) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: task id " + std::to_string(id) +
                           " is not a live uagent background task");
  }
  ToolContext immediate;
  immediate.deadline = std::chrono::steady_clock::now();
  return ToolWaitBackground(supervisor, id, immediate);
}

inline ToolResult ToolWaitTasks(ProcessSupervisor& supervisor, const json& ids,
                                bool wait_all,
                                const ToolContext& context = {}) {
  if (!ids.is_array() || ids.empty() || ids.size() > 20) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: ids must contain 1-20 task ids");
  }
  std::vector<int64_t> pending;
  for (const json& value : ids) {
    if (!value.is_number_integer()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: every task id must be integer");
    }
    int64_t id = value.get<int64_t>();
    if (id <= 0 ||
        std::find(pending.begin(), pending.end(), id) != pending.end()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: task ids must be positive and unique");
    }
    if (!IsTrackedTask(supervisor, id)) {
      return ToolFailure(ToolErrorCode::kNotFound,
                         "error: task id " + std::to_string(id) +
                             " is not a live uagent background task");
    }
    pending.push_back(id);
  }

  std::string out;
  bool process_failed = false;
  while (!AbortRequested() &&
         std::chrono::steady_clock::now() < context.deadline) {
    for (auto it = pending.begin(); it != pending.end();) {
      std::optional<ToolResult> result = TakeCompletedTask(supervisor, *it);
      if (!result) {
        ++it;
        continue;
      }
      process_failed = process_failed || !result->Ok();
      out += "[task " + std::to_string(*it) + " completed]\n" + result->output +
             "\n";
      it = pending.erase(it);
      if (!wait_all) {
        return process_failed
                   ? ToolFailure(ToolErrorCode::kProcessFailed, std::move(out))
                   : ToolSuccess(std::move(out));
      }
    }
    if (pending.empty()) {
      return process_failed
                 ? ToolFailure(ToolErrorCode::kProcessFailed, std::move(out))
                 : ToolSuccess(std::move(out));
    }
    usleep(100 * 1000);
  }

  out += std::string(AbortRequested() ? "[wait cancelled" : "[wait timed out") +
         " — running task ids:";
  for (int64_t id : pending) out += " " + std::to_string(id);
  out += "]";
  return AbortRequested() ? ToolCancelled(std::move(out))
                          : ToolTimedOut(std::move(out));
}

inline ToolResult ToolKillTask(ProcessSupervisor& supervisor, int64_t id) {
  BgJob job;
  bool found = supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
    auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BgJob& current) {
      return current.pid == static_cast<pid_t>(id) && current.kind == "task";
    });
    if (it == jobs.end()) return false;
    job = std::move(*it);
    jobs.erase(it);
    return true;
  });
  if (id <= 0 || !found) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: task id " + std::to_string(id) +
                           " is not a live uagent background task");
  }

  int status = 0;
  pid_t result = waitpid(job.pid, &status, WNOHANG);
  bool already_completed = result == job.pid;
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
  std::string output = ReadLogTail(job.log, ToolResultCap());
  unlink(job.log.c_str());
  unlink((job.log + ".1").c_str());
  if (already_completed) {
    output = "[task " + std::to_string(id) + " already completed]\n" + output +
             FmtExit(status, /*show_ok=*/true);
    return ProcessResult(std::move(output), status);
  }
  return ToolCancelled("[task " + std::to_string(id) + " cancelled]\n" +
                       output);
}

inline ToolResult ToolWaitBackground(ProcessSupervisor& supervisor, int64_t pid,
                                     const ToolContext& context = {}) {
  BgJob registered;
  bool found = supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
    auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BgJob& job) {
      return job.pid == static_cast<pid_t>(pid);
    });
    if (it == jobs.end()) return false;
    registered = *it;
    return true;
  });
  if (pid <= 0 || !found) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: pid " + std::to_string(pid) +
                           " is not a live uagent background job");
  }
  if (registered.detached) {
    return ToolSuccess("[detached job " + std::to_string(pid) +
                       " is persistent; output so far]\n" +
                       ReadLogTail(registered.log, ToolResultCap()));
  }
  const pid_t process = static_cast<pid_t>(pid);
  uintmax_t bytes = LogBytes(registered.log);
  const uintmax_t baseline = registered.observed_log_bytes.value_or(bytes);
  int status = 0;
  pid_t result = waitpid(process, &status, WNOHANG);
  bool changed = !registered.observed_log_bytes || bytes != baseline;
  bool cancelled = false;
  if (result == 0 && !changed) {
    cancelled = RunCancellable([&] {
      while (std::chrono::steady_clock::now() < context.deadline) {
        if (AbortRequested()) return;
        usleep(100 * 1000);
        result = waitpid(process, &status, WNOHANG);
        bytes = LogBytes(registered.log);
        if (result != 0 || bytes != baseline) return;
      }
    });
  }

  bool finished = result == process || (result < 0 && kill(process, 0) != 0);
  std::string out = ReadLogTail(registered.log, ToolResultCap());
  if (result == process) {
    out += FmtExit(status, /*show_ok=*/true);
  } else if (finished) {
    out = "[process exited — status unavailable]\n" + out;
  } else {
    const char* state =
        !registered.observed_log_bytes
            ? "current output"
            : (cancelled ? "wait cancelled"
                         : (std::chrono::steady_clock::now() >= context.deadline
                                ? "turn deadline reached"
                                : "new output"));
    out = "[process still running — " + std::string(state) + "]\n" + out;
  }

  supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
    auto it = std::find_if(jobs.begin(), jobs.end(), [&](const BgJob& job) {
      return job.pid == process;
    });
    if (it == jobs.end()) return;
    if (finished) {
      jobs.erase(it);
    } else {
      it->observed_log_bytes = bytes;
    }
  });
  if (finished) {
    BgTrackSignal(process, false);
  }
  if (result == process) return ProcessResult(std::move(out), status);
  if (finished) {
    return ToolFailure(ToolErrorCode::kProcessFailed, std::move(out));
  }
  if (cancelled) return ToolCancelled(std::move(out));
  if (registered.observed_log_bytes &&
      std::chrono::steady_clock::now() >= context.deadline) {
    return ToolTimedOut(std::move(out));
  }
  return ToolSuccess(std::move(out));
}

inline ToolResult ToolWaitSideTask(SideTaskSupervisor& supervisor, int64_t id,
                                   const ToolContext& context = {}) {
  if (id <= 0 || !supervisor.Contains(id)) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: id " + std::to_string(id) +
                           " is not a live uagent background job");
  }
  while (!AbortRequested() &&
         std::chrono::steady_clock::now() < context.deadline) {
    if (auto result = supervisor.Wait(id, std::chrono::milliseconds(0))) {
      std::string output = "[Background result: " + result->kind + " `" +
                           FirstLine(result->label) + "`]\n" + result->output;
      if (result->status == CompletionStatus::kCancelled) {
        return ToolCancelled(std::move(output));
      }
      if (result->status == CompletionStatus::kTimedOut) {
        return ToolTimedOut(std::move(output));
      }
      return result->status == CompletionStatus::kSuccess
                 ? ToolSuccess(std::move(output))
                 : ToolFailure(result->error, std::move(output));
    }
    supervisor.WaitForOne(std::chrono::milliseconds(100));
  }
  std::string output =
      std::string(AbortRequested() ? "[wait cancelled"
                                   : "[turn deadline reached") +
      " — background job still running]";
  return AbortRequested() ? ToolCancelled(std::move(output))
                          : ToolTimedOut(std::move(output));
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_JOBS_H_
