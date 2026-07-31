// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SHELL_H_
#define UAGENT_INCLUDE_TOOLS_SHELL_H_
// The shell runner and everything layered on it: supervised commands,
// isolated uv Python, and regex search.

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/child_env.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/tool.h"

namespace uagent {

struct ShellCommandResult {
  ToolResult result;
  std::optional<int> wait_status = std::nullopt;
};

// Foreground work waits until the shared tool deadline. Only explicitly
// asynchronous callers allow the process to outlive this call.
inline ShellCommandResult RunShellCommand(
    ProcessSupervisor& supervisor, const std::string& cmd,
    const ToolContext& context, bool allow_background, bool detach,
    std::string shell, bool immediate_background, std::string job_kind,
    const EnvironmentOverrides& environment = {},
    ChildEnvironmentPolicy environment_policy =
        ChildEnvironmentPolicy::kSanitized) {
  if (shell.empty() || shell.find('\0') != std::string::npos) {
    return {ToolFailure(
        ToolErrorCode::kInvalidArguments,
        "error: shell must be a non-empty executable name or path")};
  }
  int64_t max_jobs = MaxBackgroundJobs();
  if (static_cast<int64_t>(supervisor.PendingCount()) >= max_jobs) {
    return {ToolFailure(ToolErrorCode::kLimitExceeded,
                        "error: background job limit reached (" +
                            std::to_string(max_jobs) + ")")};
  }
  int64_t window = (detach || immediate_background)
                       ? 0
                       : context.RemainingSeconds(int64_t{1} << 30);
  const char* log_kind = detach ? "terminals" : "bg";
  std::string pattern =
      UagentDir(log_kind) + "/pending-" + std::to_string(getpid()) + "-XXXXXX";
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  int lfd = mkstemp(temp.data());
  std::string log = temp.data();
  if (lfd < 0) {
    return {ToolFailure(ToolErrorCode::kInternal,
                        "error: cannot create log file " + log)};
  }
  fchmod(lfd, 0600);
  int64_t log_bytes = BashLogBytes();
  // Foreground commands may be stopped at the cap. Detached servers instead
  // stream through this binary's tiny rotating log pump and keep running.
  std::string bounded_cmd =
      detach ? "(" + cmd + ") 2>&1 | " + ShellQuote(g_argv0) + " --log-pump " +
                   ShellQuote(log) + " " + std::to_string(log_bytes)
             : "ulimit -f " + std::to_string((log_bytes + 1023) / 1024) + "; " +
                   cmd;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, lfd, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, lfd, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, lfd);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  // POSIX specifies `short` for posix_spawnattr_setflags; fixed-width types
  // are not guaranteed to have that ABI.
  using PosixSpawnFlags = short;  // NOLINT: required by the POSIX ABI.
  PosixSpawnFlags group_flag = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_SETSID
  if (detach) group_flag = POSIX_SPAWN_SETSID;
#endif
  if (group_flag == POSIX_SPAWN_SETPGROUP) {
    posix_spawnattr_setpgroup(&attributes, 0);
  }
  sigset_t defaults, mask;
  sigemptyset(&defaults);
  for (int signal_number : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
    sigaddset(&defaults, signal_number);
  }
  sigemptyset(&mask);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setsigmask(&attributes, &mask);
  posix_spawnattr_setflags(&attributes, static_cast<PosixSpawnFlags>(
                                            group_flag | POSIX_SPAWN_SETSIGDEF |
                                            POSIX_SPAWN_SETSIGMASK));
  pid_t pid = -1;
  ChildEnvironment child_environment(environment, environment_policy);
  auto spawn_shell = [&](const std::string& executable) {
    char* const argv[] = {const_cast<char*>(executable.c_str()),
                          const_cast<char*>("-c"), bounded_cmd.data(), nullptr};
    return posix_spawnp(&pid, executable.c_str(), &actions, &attributes, argv,
                        child_environment.Data());
  };
  int spawn_error = spawn_shell(shell);
  if (spawn_error != 0 && shell == "bash") {
    spawn_error = spawn_shell("/bin/bash");
  }
  if (spawn_error != 0 && shell == "bash") spawn_error = spawn_shell("/bin/sh");
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_error != 0) {
    close(lfd);
    unlink(log.c_str());
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
  close(lfd);
  if (!detach) {
    std::string named =
        UagentDir(log_kind) + "/" + std::to_string(pid) + ".log";
    if (rename(log.c_str(), named.c_str()) == 0) {
      log = named;  // child's fd stays valid
    }
  }
  if (detach) {
    ToolResult saved = SaveDetachedRecord(pid, log, cmd);
    if (!saved.Ok()) {
      if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
      unlink(log.c_str());
      unlink((log + ".1").c_str());
      return {std::move(saved)};
    }
  }

  // Ctrl+C during the window is a hard stop: kill it too
  TrackPid(g_child_pgids, kFgMax, pid, true);
  int status = 0;
  bool exited = false;
  bool cancelled = false;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(window);
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    if (AbortRequested()) {
      if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      exited = cancelled = true;
      break;
    }
    usleep(100 * 1000);
  }
  TrackPid(g_child_pgids, kFgMax, pid, false);

  if (exited) {
    if (detach) unlink(DetachedRecordPath(pid).c_str());
    if (cancelled) {
      RemoveLog(log);
      return {ToolCancelled("error: command cancelled by user"), status};
    }
    CollectedLog collected = CollectCompletedLog(log, ToolResultCap());
    std::string out = std::move(collected.output);
    out += FmtExit(status, /*show_ok=*/false);
    ToolResult result = ProcessResult(std::move(out), status);
    result.artifact = std::move(collected.artifact);
    return {std::move(result), status};
  }
  if (!allow_background) {
    if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    if (detach) unlink(DetachedRecordPath(pid).c_str());
    CollectedLog collected = CollectCompletedLog(log, ToolResultCap());
    std::string output = std::move(collected.output);
    if (!output.empty() && output.back() != '\n') output += '\n';
    output += "error: command exceeded its execution deadline";
    ToolResult result = ToolTimedOut(std::move(output));
    result.artifact = std::move(collected.artifact);
    return {std::move(result), status};
  }
  BgJob job{pid, log, cmd, detach, job_kind};
  if (!supervisor.TryAdd(std::move(job), max_jobs)) {
    if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    unlink(log.c_str());
    unlink((log + ".1").c_str());
    return {ToolFailure(ToolErrorCode::kLimitExceeded,
                        "error: background job limit reached (" +
                            std::to_string(max_jobs) + ")")};
  }
  if (detach) {
    return {ToolSuccess(
        "[detached] pid " + std::to_string(pid) + ", log: " + log +
        " — read with terminal_output(pid=" + std::to_string(pid) + ")")};
  }
  BgTrackSignal(pid, true);
  if (job_kind == "task") {
    return {ToolSuccess("[started] task id " + std::to_string(pid) +
                        "; result will be delivered automatically")};
  }
  return {ToolSuccess("[running] pid " + std::to_string(pid) +
                      "; result will be delivered automatically")};
}

inline ToolResult ToolRunBash(ProcessSupervisor& supervisor,
                              const std::string& cmd,
                              const ToolContext& context = {},
                              bool allow_background = true, bool detach = false,
                              std::string shell = "bash",
                              bool immediate_background = false,
                              std::string job_kind = "",
                              const EnvironmentOverrides& environment = {},
                              ChildEnvironmentPolicy environment_policy =
                                  ChildEnvironmentPolicy::kSanitized) {
  return RunShellCommand(supervisor, cmd, context, allow_background, detach,
                         std::move(shell), immediate_background,
                         std::move(job_kind), environment, environment_policy)
      .result;
}

inline ToolResult ToolRunApprovedShell(ProcessSupervisor& supervisor,
                                       const std::string& command,
                                       const ToolContext& context, bool detach,
                                       std::string shell) {
  return ToolRunBash(supervisor, command, context, /*allow_background=*/detach,
                     detach, std::move(shell),
                     /*immediate_background=*/false, "", {},
                     ChildEnvironmentPolicy::kApprovedShell);
}

inline ToolResult ToolRunPython(ProcessSupervisor& supervisor,
                                const std::string& code, const json& packages,
                                const ToolContext& context = {}) {
  if (code.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: Python code must not be empty");
  }
  if (code.size() > 128 * 1024 || code.find('\0') != std::string::npos) {
    return ToolFailure(
        ToolErrorCode::kLimitExceeded,
        "error: Python code exceeds the 128 KiB limit or contains NUL");
  }
  if (!packages.is_array()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: packages must be an array");
  }
  if (packages.size() > 12) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: packages is limited to 12 entries");
  }

  std::string with;
  for (const json& value : packages) {
    if (!value.is_string()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: package entries must be strings");
    }
    std::string package = value.get<std::string>();
    if (package.empty() || package.size() > 256 ||
        package.find_first_of("\r\n") != std::string::npos ||
        package.find('\0') != std::string::npos) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: invalid package entry");
    }
    with += " --with " + ShellQuote(package);
  }
  // uv whenever it is available: isolation plus declared packages. Without it,
  // stdlib-only code still runs on plain python3 rather than being refused.
  bool uv = ExecutableOnPath("uv");
  if (!uv && !with.empty()) {
    return ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: packages require uv on PATH. Install it from "
        "https://docs.astral.sh/uv/getting-started/installation/ "
        "(macOS: brew install uv), or use only the standard library");
  }
  // Isolated environments are materialised in uv's cache, so pointing the cache
  // at the agent directory is what keeps them with the project (or the user).
  // An explicit UV_CACHE_DIR in the environment still wins.
  std::string cache =
      EnvStr("UV_CACHE_DIR").empty()
          ? "UV_CACHE_DIR=" + ShellQuote(UagentScopedDir("uv")) + " "
          : "";
  std::string command =
      uv ? cache +
               "UV_NO_PROGRESS=1 MPLBACKEND=Agg uv run --quiet "
               "--isolated --no-project" +
               with + " -- python"
         : "MPLBACKEND=Agg python3";
  command += " -c " + ShellQuote(code);
  ShellCommandResult result =
      RunShellCommand(supervisor, command, context, /*allow_background=*/false,
                      /*detach=*/false, "bash",
                      /*immediate_background=*/false, "");
  if (result.result.error == ToolErrorCode::kProcessFailed) {
    std::string hint;
    if (result.result.output.find("No module named") != std::string::npos) {
      hint =
          " Declare every third-party dependency in run_python.packages; "
          "do not install it with pip or run.";
    }
    result.result.output =
        "error: Python execution failed." + hint + "\n" + result.result.output;
  }
  return std::move(result.result);
}

inline ToolResult ToolGrep(ProcessSupervisor& supervisor,
                           const std::string& pattern, const std::string& path,
                           const std::string& glob, int64_t context_lines = 0,
                           const ToolContext& context = {}) {
  if (pattern.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: search pattern must not be empty");
  }
  if (context_lines < 0 || context_lines > 10) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: grep context must be between 0 and 10 lines");
  }
  std::string target = path.empty() ? "." : path;
  std::error_code path_error;
  auto status = std::filesystem::status(target, path_error);
  if (path_error || (!std::filesystem::is_regular_file(status) &&
                     !std::filesystem::is_directory(status))) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        "error: search path is not a readable file or directory: " + target);
  }
  int64_t max_results = GrepResults();
  int64_t bytes = GrepBytes();
  if (ToolResultCap() > 0) bytes = std::min(bytes, ToolResultCap());
  bool ripgrep = ExecutableOnPath("rg");
  std::string command;
  if (ripgrep) {
    command = "rg --line-number --column --no-heading --color=never";
    if (context_lines > 0) {
      command += " --context " + std::to_string(context_lines);
    }
    if (!glob.empty()) command += " --glob " + ShellQuote(glob);
  } else {
    command = "grep -r -E -n -H -I --exclude-dir=.git";
    if (context_lines > 0) {
      command += " -C " + std::to_string(context_lines);
    }
    if (!glob.empty()) command += " --include=" + ShellQuote(glob);
  }
  command = "set -o pipefail; " + command + " -- " + ShellQuote(pattern) + " " +
            ShellQuote(target) + " 2>&1 | head -n " +
            std::to_string(max_results + 1) + " | head -c " +
            std::to_string(bytes);
  ShellCommandResult execution = RunShellCommand(
      supervisor, command, context, false, false, "bash", false, "");
  ToolResult outcome = std::move(execution.result);
  std::string output = std::move(outcome.output);
  int exit_code = execution.wait_status && WIFEXITED(*execution.wait_status)
                      ? WEXITSTATUS(*execution.wait_status)
                      : -1;
  if (exit_code == 1) return ToolSuccess("(no matches)");
  if (exit_code == 141) {
    std::string suffix = FmtExit(*execution.wait_status, false);
    if (output.size() >= suffix.size()) {
      output.resize(output.size() - suffix.size());
    }
  } else if (!outcome.Ok()) {
    if (outcome.error != ToolErrorCode::kProcessFailed) {
      outcome.output = std::move(output);
      return outcome;
    }
    return ToolFailure(ToolErrorCode::kProcessFailed,
                       "error: search command failed:\n" + output);
  }
  if (output == "(no output)") return ToolSuccess("(no matches)");

  bool byte_limited = static_cast<int64_t>(output.size()) >= bytes;
  size_t scan = 0, cut = std::string::npos;
  int64_t lines = 0;
  while (lines < max_results) {
    size_t newline = output.find('\n', scan);
    if (newline == std::string::npos) break;
    ++lines;
    scan = newline + 1;
    if (lines == max_results) cut = scan;
  }
  if (lines < max_results && scan < output.size()) ++lines;
  bool more_results = cut != std::string::npos && cut < output.size();
  if (more_results) output.resize(cut);
  std::string header =
      "[" + std::string(ripgrep ? "ripgrep" : "grep") + " · " +
      std::to_string(std::min(lines, max_results)) +
      (more_results ? "+ result lines; more available" : " result lines") +
      "]\n";
  if (byte_limited) header += "[output byte limit reached]\n";
  return ToolSuccess(header + output);
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SHELL_H_
