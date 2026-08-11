// Copyright 2026 Timon Gentzsch

#include "include/tools/shell.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/jobs.h"

namespace uagent {

ShellCommandResult RunShellCommand(ProcessSupervisor& supervisor,
                                   const ToolContext& context,
                                   ShellCommand spec) {
  const std::string& cmd = spec.command;
  const std::string& shell = spec.shell;
  const bool detach = spec.detach;
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
  int64_t window = (detach || spec.immediate)
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
      detach ? "set -o pipefail; (" + cmd + ") 2>&1 | " +
                   ShellQuote(ExecutablePath()) + " --log-pump " +
                   ShellQuote(log) + " " + std::to_string(log_bytes)
             : "ulimit -f " + std::to_string((log_bytes - 1) / 1024 + 1) +
                   "; " + cmd;
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
  ChildEnvironment child_environment(spec.environment, spec.environment_policy);
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
      KillProcess(pid);
      RemoveLog(log);
      return {std::move(saved)};
    }
  }

  // Ctrl+C during the window is a hard stop: kill it too
  TrackPid(g_child_pgids, kFgMax, pid, true);
  int status = 0;
  bool exited = false;
  bool cancelled = false;
  auto deadline = DeadlineAfter(window);
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    if (AbortRequested()) {
      KillProcess(pid, &status);
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
  if (!spec.background) {
    KillProcess(pid, &status);
    if (detach) unlink(DetachedRecordPath(pid).c_str());
    CollectedLog collected = CollectCompletedLog(log, ToolResultCap());
    std::string output = std::move(collected.output);
    if (!output.empty() && output.back() != '\n') output += '\n';
    output += "error: command exceeded its execution deadline";
    ToolResult result = ToolTimedOut(std::move(output));
    result.artifact = std::move(collected.artifact);
    return {std::move(result), status};
  }
  bool is_task = spec.job_kind == "task";
  BgJob job{pid, log, cmd, detach, std::move(spec.job_kind)};
  if (!supervisor.TryAdd(std::move(job), max_jobs)) {
    KillProcess(pid, &status);
    RemoveLog(log);
    return {ToolFailure(ToolErrorCode::kLimitExceeded,
                        "error: background job limit reached (" +
                            std::to_string(max_jobs) + ")")};
  }
  if (detach) {
    return {ToolSuccess("[detached] pid " + std::to_string(pid) + ", log: " +
                        log + " — activity id " + std::to_string(pid) +
                        "; verify readiness with activity output")};
  }
  BgTrackSignal(pid, true);
  if (is_task) {
    return {ToolSuccess("[started] task id " + std::to_string(pid) +
                        "; result will be delivered automatically; inspect "
                        "activity output for progress/readiness, or wait only "
                        "when the next step is blocked")};
  }
  return {ToolSuccess("[running] pid " + std::to_string(pid) +
                      "; result will be delivered automatically; use "
                      "activity output to inspect it")};
}

ToolResult ToolRunApprovedShell(ProcessSupervisor& supervisor,
                                const std::string& command,
                                const ToolContext& context, bool detach,
                                const std::string& shell) {
  return RunShellCommand(
             supervisor, context,
             {.command = command,
              .shell = shell,
              .background = detach,
              .detach = detach,
              .environment_policy = ChildEnvironmentPolicy::kApprovedShell})
      .result;
}

bool StartsWithShellWord(const std::string& command, const std::string& word) {
  std::string trimmed = Trim(command);
  return trimmed == word ||
         (trimmed.starts_with(word) && trimmed.size() > word.size() &&
          std::isspace(static_cast<unsigned char>(trimmed[word.size()])));
}

std::string RunCommandPolicyError(const std::string& command) {
  if (StartsWithShellWord(command, "sudo")) {
    return "error: privileged commands are unavailable. Do not use sudo; "
           "use workspace or user-local tools, or adapt to installed "
           "dependencies.";
  }
  for (const char* executable : {"python", "python3", "pip", "pip3"}) {
    if (StartsWithShellWord(command, executable)) {
      return "error: do not invoke bare Python or pip through run. For project "
             "Python use its existing runner (for example uv run or pytest); "
             "for one-off computation use run_python with dependencies in the "
             "scratch script's PEP 723 header.";
    }
  }
  return "";
}

bool PythonScriptHasDependencies(const std::string& source) {
  size_t metadata = source.find("# /// script");
  if (metadata == std::string::npos) return false;
  size_t dependencies = source.find("dependencies", metadata);
  size_t end = source.find("# ///", metadata + 12);
  if (dependencies == std::string::npos || end == std::string::npos ||
      dependencies >= end) {
    return true;  // malformed metadata is not safe for the plain fallback
  }
  size_t open = source.find('[', dependencies);
  size_t close = open == std::string::npos ? open : source.find(']', open + 1);
  if (open == std::string::npos || close == std::string::npos || close >= end) {
    return true;
  }
  std::string list = source.substr(open + 1, close - open - 1);
  for (size_t start = 0; start < list.size();) {
    size_t newline = list.find('\n', start);
    std::string line = Trim(list.substr(start, newline - start));
    if (line.starts_with('#')) line = Trim(line.substr(1));
    if (!line.empty()) return true;
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  return false;
}

ToolResult ToolRunPython(ProcessSupervisor& supervisor,
                         const std::filesystem::path& workspace,
                         const std::string& relative_path, const json& code,
                         const json& packages, const ToolContext& context) {
  namespace fs = std::filesystem;
  constexpr std::string_view kScratchPrefix = ".uagent/scratch/";
  fs::path requested(relative_path.starts_with(kScratchPrefix)
                         ? relative_path.substr(kScratchPrefix.size())
                         : relative_path);
  if (relative_path.empty() || requested.is_absolute() ||
      requested.extension() != ".py") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: path must be a relative .py file under "
                       ".uagent/scratch");
  }
  for (const fs::path& component : requested) {
    if (component == "..") {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: Python script path must not contain ..");
    }
  }

  fs::path scratch = workspace / ".uagent" / "scratch";
  std::error_code ec;
  fs::create_directories(scratch, ec);
  if (ec) {
    return ToolFailure(
        ToolErrorCode::kInternal,
        "error: cannot create Python scratch directory: " + ec.message());
  }
  scratch = fs::canonical(scratch, ec);
  fs::path script = CanonicalAccessPath((scratch / requested).string());
  if (ec || !PathWithin(script, scratch)) {
    return ToolFailure(ToolErrorCode::kPermissionDenied,
                       "error: Python script path escapes .uagent/scratch");
  }

  std::string write_error;
  fs::path ignore = scratch / ".gitignore";
  if (!fs::exists(ignore, ec) &&
      !AtomicWriteFile(ignore.string(), "*\n", 0644,
                       /*preserve_mode=*/false, write_error)) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + write_error);
  }

  bool create = code.is_string();
  bool replaced = false;
  if (create != packages.is_array() || (!create && !packages.is_null())) {
    return ToolFailure(
        ToolErrorCode::kInvalidArguments,
        "error: creation requires code plus a packages array; rerunning "
        "requires code=null and packages=null");
  }

  if (create) {
    bool exists = fs::exists(script, ec);
    if (ec) {
      return ToolFailure(
          ToolErrorCode::kInternal,
          "error: cannot inspect Python scratch script: " + ec.message());
    }
    if (exists && !fs::is_regular_file(script, ec)) {
      return ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "error: Python scratch path exists but is not a regular file: " +
              requested.generic_string());
    }
    std::string body = code.get<std::string>();
    if (body.find('\0') != std::string::npos) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: Python code contains NUL");
    }
    if (body.find("# /// script") != std::string::npos) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: code must contain only the script body; "
                         "packages generate the PEP 723 header");
    }
    std::string source = "# /// script\n# dependencies = [\n";
    for (const json& value : packages) {
      std::string package = value.get<std::string>();
      if (package.find_first_of("\r\n") != std::string::npos ||
          package.find('\0') != std::string::npos) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: invalid package entry");
      }
      source += "#   " + JsonDump(package) + ",\n";
    }
    source += "# ]\n# ///\n\n" + body;
    if (source.back() != '\n') source += '\n';
    if (exists) {
      std::ifstream prior_input(script);
      std::string prior{std::istreambuf_iterator<char>(prior_input),
                        std::istreambuf_iterator<char>()};
      if (prior == source) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: code is identical to .uagent/scratch/" +
                               requested.generic_string() +
                               "; rerun with code=null and packages=null");
      }
      replaced = true;
    }
    if (!AtomicWriteFile(script.string(), source, 0644,
                         /*preserve_mode=*/true, write_error)) {
      return ToolFailure(ToolErrorCode::kInternal, "error: " + write_error);
    }
  } else if (!fs::is_regular_file(script, ec)) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        "error: Python scratch script does not exist: " + relative_path);
  }

  std::ifstream input(script);
  std::string source{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  bool uv = ExecutableOnPath("uv");
  if (!uv && PythonScriptHasDependencies(source)) {
    return ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: this script declares third-party dependencies and requires "
        "uv on PATH. Install uv or edit the PEP 723 dependency list");
  }
  std::string command =
      uv ? "UV_NO_PROGRESS=1 MPLBACKEND=Agg uv run --quiet --no-project "
           "--script " +
               ShellQuote(script.string())
         : "MPLBACKEND=Agg python3 " + ShellQuote(script.string());
  ShellCommandResult result =
      RunShellCommand(supervisor, context, {.command = std::move(command)});
  if (result.result.error == ToolErrorCode::kProcessFailed) {
    std::string hint =
        result.result.output.find("No module named") != std::string::npos
            ? " Add every third-party dependency to the script's PEP 723 "
              "header; do not install it with pip or run."
            : "";
    result.result.output =
        "error: Python execution failed." + hint + "\n" + result.result.output;
  }
  std::string lifecycle =
      create ? (replaced ? " · overwrote" : " · wrote") : "";
  lifecycle +=
      result.result.Ok()
          ? " · executed"
          : " · execution " +
                std::string(CompletionStatusName(result.result.status));
  result.result.output = "[script: .uagent/scratch/" +
                         requested.generic_string() + lifecycle + "]\n" +
                         result.result.output;
  return std::move(result.result);
}

ToolResult ToolGrep(ProcessSupervisor& supervisor, const std::string& pattern,
                    const std::string& path, const std::string& glob,
                    int64_t context_lines, const ToolContext& context,
                    bool files_only) {
  if (files_only && context_lines > 0) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: grep context is only available in content mode");
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
  if (files_only) {
    if (ripgrep) {
      command = "rg --files --color=never";
      if (!glob.empty()) command += " --glob " + ShellQuote(glob);
      command += " -- " + ShellQuote(target) +
                 " | rg --line-number --color=never -- " + ShellQuote(pattern);
    } else {
      command = "find " + ShellQuote(target) + " -type f";
      if (!glob.empty()) command += " -name " + ShellQuote(glob);
      command += " -print | grep -E -n -- " + ShellQuote(pattern);
    }
  } else {
    command = ripgrep ? "rg --line-number --column --no-heading --color=never"
                      : "grep -r -E -n -H -I --exclude-dir=.git";
    if (context_lines > 0) {
      command += ripgrep ? " --context " : " -C ";
      command += std::to_string(context_lines);
    }
    if (!glob.empty()) {
      command += ripgrep ? " --glob " : " --include=";
      command += ShellQuote(glob);
    }
    command += " -- " + ShellQuote(pattern) + " " + ShellQuote(target);
  }
  command = "set -o pipefail; " + command + " 2>&1 | head -n " +
            std::to_string(max_results + 1) + " | head -c " +
            std::to_string(bytes);
  ShellCommandResult execution =
      RunShellCommand(supervisor, context, {.command = std::move(command)});
  ToolResult outcome = std::move(execution.result);
  std::string output = std::move(outcome.output);
  int wait_status = execution.wait_status.value_or(-1);
  int exit_code = execution.wait_status && WIFEXITED(wait_status)
                      ? WEXITSTATUS(wait_status)
                      : -1;
  if (exit_code == 1) return ToolSuccess("(no matches)");
  if (exit_code == 141) {
    std::string suffix = FmtExit(wait_status, false);
    if (output.size() >= suffix.size()) {
      output.resize(output.size() - suffix.size());
    }
  } else if (!outcome.Ok()) {
    if (outcome.error == ToolErrorCode::kProcessFailed) {
      return ToolFailure(ToolErrorCode::kProcessFailed,
                         "error: search command failed:\n" + output);
    }
    outcome.output = std::move(output);
    return outcome;
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
      "[" + std::string(ripgrep ? "ripgrep" : "grep") +
      (files_only ? " files · " : " · ") +
      std::to_string(std::min(lines, max_results)) +
      (more_results ? "+ result lines; more available" : " result lines") +
      "]\n";
  if (byte_limited) header += "[output byte limit reached]\n";
  return ToolSuccess(header + output);
}

}  // namespace uagent
