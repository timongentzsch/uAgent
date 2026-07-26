// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_JOBS_H_
#define UAGENT_INCLUDE_TOOLS_JOBS_H_
// Job log tailing, detached-terminal records, and the wait tools. Memory
// stays bounded because only the tail of a log is ever read.

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
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
  auto cutoff =
      fs::file_time_type::clock::now() -
      std::chrono::hours(
          24 * std::max(int64_t{0}, EnvLong("UAGENT_TERMINAL_DAYS", 7)));
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

inline std::string SaveDetachedRecord(pid_t pid, const std::string& log,
                                      const std::string& cmd) {
  std::error_code ec;
  std::string cwd = std::filesystem::current_path(ec).string();
  json record = {{"pid", pid},
                 {"log", log},
                 {"command", cmd},
                 {"cwd", ec ? "" : cwd},
                 {"started_at", UtcStamp()}};
  return ToolWritePrivateFile(DetachedRecordPath(pid),
                              JsonDump(record, 2) + "\n");
}

inline std::string ToolTerminalOutput(int64_t pid) {
  std::vector<json> records = DetachedRecords();
  if (pid <= 0) {
    if (records.empty()) return "(no detached terminals)";
    std::string out;
    for (const json& record : records) {
      pid_t record_pid = JsonValue(record, "pid", 0);
      out += (JsonValue(record, "_alive", false) ? "[running] " : "[exited] ");
      out += "pid " + std::to_string(record_pid) + " · " +
             JsonValue(record, "cwd", "") + " · " +
             OneLine(JsonValue(record, "command", ""), 120) + " · " +
             JsonValue(record, "log", "") + "\n";
    }
    int64_t cap = ToolResultCap();
    if (cap > 0 && out.size() > static_cast<size_t>(cap)) {
      out = Utf8Trunc(std::move(out), static_cast<size_t>(cap));
      out += "\n[terminal list truncated]";
    }
    return out;
  }

  auto found =
      std::find_if(records.begin(), records.end(), [&](const json& record) {
        return JsonValue(record, "pid", int64_t{0}) == pid;
      });
  if (found == records.end()) {
    return "error: pid " + std::to_string(pid) +
           " is not a uagent detached terminal";
  }
  const json& record = *found;
  std::string status =
      JsonValue(record, "_alive", false) ? "running" : "exited";
  return "[" + status + " · pid " + std::to_string(pid) + " · " +
         JsonValue(record, "cwd", "") + " · log " +
         JsonValue(record, "log", "") + "]\n" +
         ReadLogTail(JsonValue(record, "log", ""), ToolResultCap());
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
            OneLine(it->cmd, 80) + "`]\n" + out + FmtExit(st, /*show_ok=*/true);
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

inline std::string ToolWaitBackground(ProcessSupervisor& supervisor,
                                      int64_t pid, int64_t timeout_s,
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
    return "error: pid " + std::to_string(pid) +
           " is not a live uagent background job";
  }
  // A detached job outlives the turn, so there is nothing to join — answer
  // with its output so far rather than making the model retry another tool.
  if (registered.detached) {
    return "[detached job " + std::to_string(pid) +
           " is persistent; output so far]\n" +
           ReadLogTail(registered.log, ToolResultCap());
  }
  std::string log = registered.log;
  int status = 0;
  bool reaped = false;
  std::string note;
  int64_t bounded_timeout =
      context.RemainingSeconds(timeout_s > 0 ? timeout_s : (int64_t{1} << 30));
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(bounded_timeout);
  RunCancellable([&] {  // Ctrl+C cancels the wait, never the process
    while (true) {
      pid_t r = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
      if (r == static_cast<pid_t>(pid)) {
        reaped = true;
        break;
      }
      if (r < 0 && kill(static_cast<pid_t>(pid), 0) != 0) {
        break;  // reaped earlier
      }
      if (AbortRequested()) {
        note = "[wait cancelled — process still running]\n";
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        note = "[wait timed out after " + std::to_string(timeout_s) +
               "s — process still running]\n";
        break;
      }
      usleep(250 * 1000);
    }
  });
  if (note.empty()) {  // finished: forget the job
    BgTrackSignal(static_cast<pid_t>(pid), false);
    supervisor.WithJobs([&](std::vector<BgJob>& jobs) {
      std::erase_if(jobs, [&](const BgJob& job) {
        return job.pid == static_cast<pid_t>(pid);
      });
    });
  }
  std::string out = note + ReadLogTail(log, ToolResultCap());
  if (reaped) {
    out += FmtExit(status, /*show_ok=*/true);  // confirm completion even on 0
  }
  return out;
}

inline std::string ToolWaitSideTask(SideTaskSupervisor& supervisor, int64_t id,
                                    int64_t timeout_s,
                                    const ToolContext& context = {}) {
  if (id <= 0 || !supervisor.Contains(id)) {
    return "error: id " + std::to_string(id) +
           " is not a live uagent background job";
  }
  int64_t bounded_timeout =
      context.RemainingSeconds(timeout_s > 0 ? timeout_s : (int64_t{1} << 30));
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(bounded_timeout);
  while (!AbortRequested() && std::chrono::steady_clock::now() < deadline) {
    if (auto result = supervisor.Wait(id, std::chrono::milliseconds(0))) {
      return "[Background result: " + result->kind + " `" +
             OneLine(result->label, 80) + "`]\n" + result->output;
    }
    supervisor.WaitForOne(std::chrono::milliseconds(100));
  }
  return std::string(AbortRequested() ? "[wait cancelled" : "[wait timed out") +
         " — background job still running]";
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_JOBS_H_
