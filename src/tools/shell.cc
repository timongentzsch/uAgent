// Copyright 2026 Timon Gentzsch

#include "include/tools/shell.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/core/capture.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/sandbox.h"
#include "include/core/sandbox_watch.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/files.h"

namespace uagent {
namespace {

// The shell `bash` means on this host. Needed under a sandbox wrapper only:
// there a missing shell fails as the wrapper, not as an errno to retry on.
std::string ResolveShellExecutable(const std::string& shell) {
  if (shell != "bash") return shell;
  return access("/bin/bash", X_OK) == 0 ? "/bin/bash" : shell;
}

// `program` is the argv to run: `shell -c command`, or a program run directly.
// Returns the posix_spawnp errno; `pid` is set on 0.
int SpawnShellWithFallback(const std::vector<std::string>& program,
                           const std::vector<std::string>& wrapper,
                           std::vector<SpawnFd> fds, bool session,
                           char* const* environment, pid_t& pid) {
  SpawnSpec spec{
      .environment = environment,
      .group = session ? SpawnSpec::Group::kSession : SpawnSpec::Group::kOwn,
      .default_signals = {SIGINT, SIGTERM, SIGHUP, SIGPIPE},
      .fds = std::move(fds)};
  const std::string& shell = program.front();
  auto spawn = [&](const std::string& executable) {
    // With a wrapper, the wrapper is the program and the shell its argument.
    spec.argv = wrapper;
    spec.argv.push_back(executable);
    spec.argv.insert(spec.argv.end(), program.begin() + 1, program.end());
    return SpawnProcess(spec, pid);
  };
  int error = spawn(wrapper.empty() ? shell : ResolveShellExecutable(shell));
  // `bash` falls back to its usual absolute path when PATH lacks it, never to
  // sh: its pipelines may mask failures.
  const bool fallback = wrapper.empty() && shell == "bash";
  if (error != 0 && fallback) error = spawn("/bin/bash");
  return error;
}

// Spawn `program` with stdin at `stdin_fd` (/dev/null when -1) and both
// output streams on the log, in a session of its own when `detach`.
int SpawnLoggedShell(const std::vector<std::string>& program,
                     const std::vector<std::string>& wrapper, int log_fd,
                     bool detach, char* const* environment, pid_t& pid,
                     int stdin_fd = -1) {
  return SpawnShellWithFallback(program, wrapper,
                                {{STDIN_FILENO, stdin_fd},
                                 {STDOUT_FILENO, log_fd},
                                 {STDERR_FILENO, log_fd}},
                                detach, environment, pid);
}

// master_fd is set only on success; the caller owns it from then on.
int SpawnPtyShell(const std::vector<std::string>& program,
                  const std::vector<std::string>& wrapper,
                  char* const* environment, pid_t& pid, int& master_fd) {
  master_fd = -1;
  Fd master;
  std::string slave_path;
  if (const int error = OpenPty(master, slave_path, 24, 80); error != 0) {
    return error;
  }
  int error = SpawnShellWithFallback(program, wrapper,
                                     {{STDIN_FILENO, -1, slave_path},
                                      {STDOUT_FILENO, STDIN_FILENO},
                                      {STDERR_FILENO, STDIN_FILENO}},
                                     /*session=*/true, environment, pid);
  if (error == 0) master_fd = master.Release();
  return error;
}

// How long a command stopped at its deadline or by an interrupt has to clean
// up before it is killed.
constexpr auto kStopGrace = std::chrono::seconds(1);

void SignalShellGroup(pid_t pid, int signal_number) {
  if (kill(-pid, signal_number) != 0) (void)kill(pid, signal_number);
}

bool WaitForTerminal(ActivitySupervisor& supervisor,
                     const std::shared_ptr<ActivitySession>& session,
                     std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    uint64_t generation = supervisor.Generation();
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (ActivityTerminal(session->state)) return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    supervisor.WaitForChange(generation, deadline);
  }
}

ToolResult ActivityLimitError() {
  return ToolFailure(
      ToolErrorCode::kLimitExceeded,
      "activity limit reached (" + std::to_string(MaxActivities()) + ")");
}

// The wrapper argv for one spawn, or why the command cannot run: a session
// told to confine that cannot must not spawn unconfined.
std::string SandboxWrapperFor(const ShellCommand& spec,
                              const SandboxStatus& status,
                              std::vector<std::string>* wrapper) {
  if (!spec.sandbox) return {};
  if (status.mode == SandboxMode::kRefused) {
    return "the sandbox cannot be enforced: " + status.reason +
           ". Set UAGENT_SANDBOX=off to run commands unconfined";
  }
  if (status.mode != SandboxMode::kEnforced) return {};
  *wrapper = SandboxWrapperArgv(status);
  if (wrapper->empty()) {
    return "the sandbox policy is too large to enforce (" +
           std::to_string(status.policy.writable_roots.size()) +
           " writable roots and " +
           std::to_string(status.policy.unreadable.size()) +
           " closed folders); remove entries from sandbox.write, or move "
           "folders out of the home folder or the workspace's parent";
  }
  return {};
}

// What a wrapped command may not change but this host cannot deny, recorded
// before it runs; null when nothing is wrapped or the host denies it outright.
std::shared_ptr<const SandboxWatch> WatchUnenforced(
    const SandboxStatus& status, const std::vector<std::string>& wrapper) {
  if (wrapper.empty()) return nullptr;
  std::vector<std::string> paths = SandboxUnenforcedWrites(status);
  return paths.empty() ? nullptr
                       : std::make_shared<const SandboxWatch>(std::move(paths));
}

// A receipt with nothing to show but where the work went: its hint tells the
// model, and its link takes a person there.
ToolResult LinkResult(std::string hint, json link) {
  ToolResult result = ToolSuccess("");
  result.hint = std::move(hint);
  result.body_kind = "link";
  result.parts = json::array({std::move(link)});
  return result;
}

// A detached terminal outlives its turn: its output goes to a rotating log
// through a pump child, and an identical live command is reused.
ShellCommandResult StartDetachedShell(
    ActivitySupervisor& supervisor, ShellCommand& spec,
    const std::vector<std::string>& wrapper,
    std::shared_ptr<const SandboxWatch> watch) {
  const std::string& cmd = spec.command;
  if (std::optional<DetachedActivity> existing =
          FindRunningDetachedActivity(cmd)) {
    return {LinkResult(kDetachedMarker + std::to_string(existing->pid) +
                           ", log: " + existing->log +
                           " — reused existing live activity id " +
                           std::to_string(existing->pid) +
                           "; verify readiness with activity output",
                       LinkPart("activity", existing->pid, "Open activity"))};
  }
  ScopedTempFile pending(UagentDir(kTerminalLogsDir) + "/pending-" +
                         std::to_string(getpid()) + "-XXXXXX");
  std::string log = pending.Path();
  if (!pending) {
    return {
        ToolFailure(ToolErrorCode::kInternal, "cannot create log file " + log)};
  }
  fchmod(pending.Get(), kPrivateFileMode);
  std::string bounded_cmd = "set -o pipefail; (" + cmd + ") 2>&1 | " +
                            ShellQuote(ExecutablePath()) + " --log-pump " +
                            ShellQuote(log) + " " +
                            std::to_string(ActivityLogBytes());
  pid_t pid = -1;
  ChildEnvironment child_environment(spec.environment, spec.environment_policy);
  int spawn_error =
      SpawnLoggedShell({spec.shell, "-c", bounded_cmd}, wrapper, pending.Get(),
                       /*detach=*/true, child_environment.Data(), pid);
  pending.Close();
  if (spawn_error != 0) {
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
  log = pending.Release();
  // Past the spawn this call owns a live child: no failure may leave it
  // running without a record to find it by.
  auto fail_and_reap = [&](ToolResult error) {
    KillProcess(pid);
    // The record may already be on disk: a reader that finds it after the
    // child is reaped would kill whatever inherits the pid next.
    unlink(DetachedRecordPath(pid).c_str());
    RemoveLog(log);
    return ShellCommandResult{std::move(error), std::nullopt,
                              /*launched=*/true};
  };
  ToolResult saved = SaveDetachedRecord(pid, log, cmd);
  if (!saved.Ok()) return fail_and_reap(std::move(saved));
  Activity job{.pid = pid,
               .log = log,
               .cmd = cmd,
               .kind = ActivityKind::kDetached,
               .id = pid,
               .display_label = std::move(spec.activity_label),
               .watch = std::move(watch)};
  if (!supervisor.TryAdd(std::move(job), MaxActivities())) {
    return fail_and_reap(ActivityLimitError());
  }
  return {LinkResult(kDetachedMarker + std::to_string(pid) + ", log: " + log +
                         " — activity id " + std::to_string(pid) +
                         "; verify readiness with activity output",
                     LinkPart("activity", pid, "Open activity")),
          std::nullopt, /*launched=*/true};
}

}  // namespace

std::string RootCommand(const std::string& command, const std::string& host,
                        bool tty, const std::string& nonce) {
  // Without a terminal only sudo reads the password pipe; the command's stdin
  // is /dev/null. -k makes sudo prompt even with cached credentials, so the
  // password line never reaches the command.
  std::string sudo =
      (nonce.empty() ? std::string("sudo -n") : "sudo -k -S -p " + nonce) +
      " -- bash -o pipefail -c " +
      ShellQuote(tty ? command : "exec </dev/null; " + command);
  if (host.empty()) return sudo;
  // BatchMode: a login that wants a password fails instead of waiting on a
  // prompt nobody answers.
  return std::string("ssh -o BatchMode=yes ") + (tty ? "-t " : "") + "-- " +
         ShellQuote(host) + " " + ShellQuote(sudo);
}

bool PasswordlessSudo(const std::string& host) {
  const std::vector<std::string> probe =
      host.empty() ? std::vector<std::string>{"sudo", "-n", "true"}
                   : std::vector<std::string>{"ssh", "-o", "BatchMode=yes",
                                              "--",  host, "sudo -n true"};
  return CaptureProcess(probe).status == 0;
}

ShellCommandResult RunShellCommand(ActivitySupervisor& supervisor,
                                   const ToolContext& context,
                                   ShellCommand spec) {
  const std::string& cmd = spec.command;
  const std::string& shell = spec.shell;
  if (spec.yield_ms > 0) {
    spec.yield_ms = std::clamp(spec.yield_ms, kMinYieldMs, kMaxYieldMs);
  }
  if (spec.detach && !spec.argv.empty()) {
    return {ToolFailure(ToolErrorCode::kInvalidArguments,
                        "a detached command runs through a shell")};
  }
  if (shell.empty() || shell.find('\0') != std::string::npos) {
    return {ToolFailure(ToolErrorCode::kInvalidArguments,
                        "shell must be a non-empty executable name or path")};
  }
  // One status for the whole command: the approval mode picks it, and can
  // change while the command runs.
  const SandboxStatus& sandbox = SandboxRuntime();
  std::vector<std::string> wrapper;
  if (std::string error = SandboxWrapperFor(spec, sandbox, &wrapper);
      !error.empty()) {
    return {ToolFailure(ToolErrorCode::kUnavailable, error)};
  }
  std::shared_ptr<const SandboxWatch> watch = WatchUnenforced(sandbox, wrapper);
  if (spec.detach) return StartDetachedShell(supervisor, spec, wrapper, watch);

  std::optional<ActivityReservation> reservation =
      supervisor.ReserveActivity(MaxActivities());
  if (!reservation) return {ActivityLimitError()};
  int64_t window =
      spec.immediate ? 0 : context.RemainingSeconds(int64_t{1} << 30);
  ScopedTempFile pending(UagentDir(kActivityDir) + "/pending-" +
                         std::to_string(getpid()) + "-XXXXXX");
  std::string log = pending.Path();
  if (!pending) {
    return {
        ToolFailure(ToolErrorCode::kInternal, "cannot create log file " + log)};
  }
  fchmod(pending.Get(), kPrivateFileMode);
  int64_t interaction_cap = ActivityOutputCap(spec.max_output_chars);
  pid_t pid = -1;
  int master_fd = -1;
  int pipe_fds[2] = {-1, -1};
  bool tty = spec.tty;
  if (!tty && pipe(pipe_fds) != 0) {
    return {ToolFailure(ToolErrorCode::kInternal,
                        "cannot create process output pipe")};
  }
  Fd pipe_read(pipe_fds[0]), pipe_write(pipe_fds[1]);
  // A root command without a terminal reads sudo's password from a pipe the
  // activity answers on, as a terminal's input is answered.
  int stdin_fds[2] = {-1, -1};
  if (!tty && spec.secret && pipe(stdin_fds) != 0) {
    return {ToolFailure(ToolErrorCode::kInternal,
                        "cannot create process input pipe")};
  }
  Fd stdin_read(stdin_fds[0]), stdin_write(stdin_fds[1]);
  // Its own end must not reach the command, or its stdin would never end.
  if (stdin_write) (void)fcntl(stdin_write.Get(), F_SETFD, FD_CLOEXEC);
  auto session = std::make_shared<ActivitySession>();
  session->tty = tty;
  session->nonce = spec.nonce;
  session->secret = spec.secret;
  ChildEnvironment child_environment(spec.environment, spec.environment_policy);
  const std::vector<std::string> program =
      spec.argv.empty()
          ? (std::filesystem::path(shell).filename() == "bash" ||
                     std::filesystem::path(shell).filename() == "zsh"
                 ? std::vector<std::string>{shell, "-o", "pipefail", "-c", cmd}
                 : std::vector<std::string>{shell, "-c", cmd})
          : spec.argv;
  int spawn_error =
      tty ? SpawnPtyShell(program, wrapper, child_environment.Data(), pid,
                          master_fd)
          : SpawnLoggedShell(program, wrapper, pipe_fds[1], /*detach=*/false,
                             child_environment.Data(), pid, stdin_read.Get());
  pipe_write.Reset();
  stdin_read.Reset();
  Fd master(master_fd);
  if (spawn_error != 0) {
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
  // Named after the pid and the activity id: a pid alone is reused within a
  // run, and an activity id alone repeats in a child agent sharing the folder.
  std::string named = UagentDir(kActivityDir) + "/" + std::to_string(getpid()) +
                      "-" + std::to_string(reservation->Id()) + ".log";
  if (rename(pending.Path().c_str(), named.c_str()) == 0) {
    log = named;  // child's fd stays valid
  }
  // ReleaseFd, not Release: the supervisor still tails the file through this
  // descriptor.
  int log_fd = pending.ReleaseFd();

  Fd input(tty ? dup(master.Get()) : stdin_write.Release());
  if (tty && !input) {
    KillProcess(pid);
    RemoveLog(log);
    return {ToolFailure(
        ToolErrorCode::kInternal,
        "cannot duplicate PTY input: " + std::string(strerror(errno)))};
  }

  Activity foreground{.pid = pid,
                      .log = log,
                      .cmd = cmd,
                      .id = reservation->Id(),
                      .session = session,
                      .display_label = std::move(spec.activity_label),
                      .watch = watch};
  std::optional<int64_t> registered = reservation->Register(foreground);
  if (!registered) {
    KillProcess(pid);
    RemoveLog(log);
    return {ActivityLimitError()};
  }
  int64_t activity_id = *registered;
  int output_fd = tty ? master.Release() : pipe_read.Release();
  supervisor.RegisterIo(session, output_fd, input.Release(), log_fd,
                        ActivityLogBytes());

  // Registered for the signal handler's kill sweep from spawn onward, so a
  // Ctrl-C at any point, including the handover below, reaches the child.
  ActivityTrackSignal(pid, true);
  bool cancelled = false;
  bool handed_off = false;
  bool exited = false;
  auto deadline = DeadlineAfter(window);
  if (spec.yield_ms > 0) {
    deadline = std::min(deadline, std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(spec.yield_ms));
  }
  for (;;) {
    uint64_t generation = supervisor.Generation();
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      handed_off = session->background_requested;
      exited = ActivityTerminal(session->state);
    }
    // An interrupt comes before the deadline and the handover: a command
    // being stopped is not moved to the background instead.
    if (!exited && AbortRequested()) {
      cancelled = true;
      handed_off = false;
      break;
    }
    // Guidance is read now, as a wait yields to it: the command carries on
    // in the background.
    if (!exited && SteeringYieldRequested()) handed_off = true;
    if (handed_off || exited || std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    supervisor.WaitForChange(generation, deadline);
  }

  if (cancelled) {
    (void)TerminateGroup(supervisor, pid, kStopGrace, false);
    exited = WaitForTerminal(supervisor, session, DeadlineAfter(2));
  }
  // Every path below that does not background the child takes its signal
  // registration back out.

  auto finish = [&](auto build) {
    int status = 0;
    std::string output;
    bool rejected = false;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      status = session->wait_status.value_or(0);
      output = LimitOutput(session->transcript.Snapshot(), interaction_cap);
      rejected = session->secret_rejected;
    }
    // The transcript above is what the caller reads; the log is only kept.
    std::optional<ToolArtifact> artifact =
        KeepCompletedLog(log, ToolResultCap(),
                         /*failed=*/!ExitedOk(status));
    ToolResult result = build(std::move(output), status);
    result.body_kind = "tail";
    // What the sandbox allowed, beside any failure it may have caused: its
    // refusals read as ordinary errors, in every wording and language.
    if (!result.Ok() && !wrapper.empty()) {
      AddHint(result, "[sandbox: " + SandboxNote(sandbox) + "]");
    }
    AddHint(result, SandboxRestoreNote(watch.get(), CanonicalCwd()));
    if (rejected) {
      AddHint(result,
              "[sudo rejected the password; the next root command asks again]");
    }
    result.artifact = std::move(artifact);
    return ShellCommandResult{std::move(result), status, /*launched=*/true};
  };

  if (cancelled) {
    ActivityTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    RemoveLog(log);
    (void)SandboxRestoreNote(watch.get(), CanonicalCwd());
    return {ToolCancelled("error: command cancelled by user"), std::nullopt,
            /*launched=*/true};
  }
  if (exited) {
    ActivityTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    return finish([](std::string output, int status) {
      return ExitResult(std::move(output), status);
    });
  }
  if (!spec.background && !handed_off && spec.yield_ms <= 0) {
    ActivityTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    (void)TerminateGroup(supervisor, pid, kStopGrace, false);
    WaitForTerminal(supervisor, session, DeadlineAfter(2));
    return finish([](std::string output, int) {
      if (!output.empty() && output.back() != '\n') output += '\n';
      output += "error: command exceeded its execution deadline";
      return ToolTimedOut(std::move(output));
    });
  }

  std::optional<Activity> moved = supervisor.MoveForegroundToBackground(pid);
  if (!moved) {
    ActivityTrackSignal(pid, false);
    SignalShellGroup(pid, SIGKILL);
    RemoveLog(log);
    return {ToolFailure(ToolErrorCode::kInternal,
                        "foreground activity ownership was lost"),
            std::nullopt, /*launched=*/true};
  }
  // An interrupt that arrived during the handover stops the command all the
  // same: nothing else would, once it is in the background.
  if (!spec.immediate && AbortRequested()) {
    (void)ToolActivityStop(supervisor, activity_id);
    return {ToolCancelled("error: command cancelled by user"), std::nullopt,
            /*launched=*/true};
  }
  std::string initial_output;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    initial_output =
        LimitOutput(session->pending_output.Drain(), interaction_cap);
    session->last_used = std::chrono::steady_clock::now();
  }
  ToolResult running = LinkResult(
      kRunningActivityMarker + std::to_string(activity_id) +
          (handed_off ? " moved to background; " : "; ") +
          (supervisor.WakesSession()
               ? "it keeps running while you work, and its result reaches "
                 "you when it ends, even after this turn"
               : "it keeps running while you work; poll or wait on it for "
                 "output"),
      LinkPart("activity", activity_id, "Open activity"));
  running.background = activity_id;
  if (!initial_output.empty()) {
    running.output = std::move(initial_output);
    running.body_kind = "tail";
  }
  return {std::move(running), std::nullopt, /*launched=*/true};
}

ToolResult ToolGrep(ActivitySupervisor& supervisor, const std::string& pattern,
                    const std::string& path, const GrepOptions& options,
                    const ToolContext& context) {
  const auto& [glob, context_lines, files_only, literal, matching_files] =
      options;
  std::string target = path.empty() ? "." : path;
  std::error_code path_error;
  auto status = std::filesystem::status(target, path_error);
  if (path_error || (!std::filesystem::is_regular_file(status) &&
                     !std::filesystem::is_directory(status))) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        "search path is not a readable file or directory: " + target);
  }
  int64_t max_results = GrepResults();
  int64_t bytes = ToolResultCap() > 0 ? ToolResultCap() : 1024;
  bool ripgrep = ExecutableOnPath("rg");
  // Ripgrep reports files in completion order and results are cut at a cap:
  // unsorted, a repeated search would return another arbitrary subset.
  const char* sorted = " --sort path";
  std::string command;
  if (files_only) {
    if (ripgrep) {
      command = std::string("rg --files --color=never") + sorted;
      if (!glob.empty()) command += " --glob " + ShellQuote(glob);
      command += " -- " + ShellQuote(target) +
                 " | rg --line-number --color=never " + (literal ? "-F " : "") +
                 "-- " + ShellQuote(pattern);
    } else {
      command = "find " + ShellQuote(target) + " -type f";
      if (!glob.empty()) command += " -name " + ShellQuote(glob);
      command += std::string(" -print | grep ") + (literal ? "-F" : "-E") +
                 " -n -- " + ShellQuote(pattern);
    }
  } else {
    command = ripgrep ? std::string(
                            "rg --line-number --column --no-heading "
                            "--color=never") +
                            sorted
                      : std::string("grep -r -n -H -I --exclude-dir=.git");
    command += literal ? " -F" : (ripgrep ? "" : " -E");
    if (matching_files) command += " -l";
    if (context_lines > 0 && !matching_files) {
      command += ripgrep ? " --context " : " -C ";
      command += std::to_string(context_lines);
    }
    if (!glob.empty()) {
      command += ripgrep ? " --glob " : " --include=";
      command += ShellQuote(glob);
    }
    command += " -- " + ShellQuote(pattern) + " " + ShellQuote(target);
  }
  // PIPESTATUS[0], not pipefail: head closing the pipe makes the whole
  // pipeline exit 141, which would swallow a producer error.
  command += " 2>&1 | head -n " + std::to_string(max_results + 1) +
             " | head -c " + std::to_string(bytes) + "; exit ${PIPESTATUS[0]}";
  ShellCommandResult execution =
      RunShellCommand(supervisor, context, {.command = std::move(command)});
  ToolResult outcome = std::move(execution.result);
  std::string output = std::move(outcome.output);
  int wait_status = execution.wait_status.value_or(-1);
  int exit_code = execution.wait_status && WIFEXITED(wait_status)
                      ? WEXITSTATUS(wait_status)
                      : -1;
  const std::string unit = files_only || matching_files ? "file" : "line";
  if (exit_code == 1) return ListResult("(no matches)", 0, unit);
  // 141: head closed the pipe on a producer with more to say.
  if (exit_code != 141 && !outcome.Ok()) {
    outcome.output = outcome.error == ToolErrorCode::kProcessFailed
                         ? std::string(kToolErrorPrefix) +
                               "search command failed:\n" + output
                         : std::move(output);
    outcome.body_kind = "text";
    return outcome;
  }
  if (output == "(no output)") return ListResult("(no matches)", 0, unit);

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
  const int64_t shown = std::min(lines, max_results);
  ToolResult result = ListResult(std::move(output), shown, unit);
  AddHint(
      result,
      "[" + std::string(ripgrep ? "ripgrep" : "grep") +
          (files_only ? " files · " : " · ") + std::to_string(shown) +
          (more_results ? "+ result lines; more available" : " result lines") +
          "]");
  if (byte_limited) AddHint(result, "[output byte limit reached]");
  return result;
}

}  // namespace uagent
