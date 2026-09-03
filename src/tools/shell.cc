// Copyright 2026 Timon Gentzsch

#include "include/tools/shell.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"

namespace uagent {
namespace {

// POSIX specifies `short` for posix_spawnattr_setflags; fixed-width types are
// not guaranteed to have that ABI.
using PosixSpawnFlags = short;  // NOLINT: required by the POSIX ABI.

void ConfigureShellSpawn(posix_spawnattr_t& attributes,
                         PosixSpawnFlags group_flag) {
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
}

// The shell that `bash` means on this host. Only needed under a sandbox
// wrapper: there the spawn resolves the wrapper, so a missing shell surfaces
// as the wrapper's own failure rather than as an errno the retry below could
// read. Resolving in the parent keeps the same three candidates in the same
// order.
std::string ResolveShellExecutable(const std::string& shell) {
  if (shell != "bash") return shell;
  for (const char* candidate : {"/bin/bash", "/bin/sh"}) {
    if (access(candidate, X_OK) == 0) return candidate;
  }
  return shell;
}

int SpawnShellWithFallback(const std::string& shell, std::string& command,
                           const std::vector<std::string>& wrapper,
                           const posix_spawn_file_actions_t& actions,
                           const posix_spawnattr_t& attributes,
                           char* const* environment, pid_t& pid) {
  auto spawn = [&](const std::string& executable) {
    std::vector<char*> argv;
    argv.reserve(wrapper.size() + 4);
    for (const std::string& word : wrapper) {
      argv.push_back(const_cast<char*>(word.c_str()));
    }
    argv.push_back(const_cast<char*>(executable.c_str()));
    argv.push_back(const_cast<char*>("-c"));
    argv.push_back(command.data());
    argv.push_back(nullptr);
    // The program is the wrapper when there is one; the shell is then just its
    // first argument, and `command` stays unwrapped either way.
    const char* program = wrapper.empty() ? executable.c_str() : argv[0];
    return posix_spawnp(&pid, program, &actions, &attributes, argv.data(),
                        environment);
  };
  if (!wrapper.empty()) return spawn(ResolveShellExecutable(shell));
  int error = spawn(shell);
  // `bash` is the documented default, so fall back to the usual absolute
  // paths before giving up on a PATH that does not have it.
  if (error != 0 && shell == "bash") error = spawn("/bin/bash");
  if (error != 0 && shell == "bash") error = spawn("/bin/sh");
  return error;
}

// Spawn `shell -c command` with stdin at /dev/null, both output streams on the
// log, default signal dispositions, and its own process group (or session, for
// a detached terminal). Returns the posix_spawnp errno; `pid` is set on 0.
int SpawnLoggedShell(const std::string& shell, std::string& command,
                     const std::vector<std::string>& wrapper, int log_fd,
                     bool detach, char* const* environment, pid_t& pid) {
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, log_fd);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  PosixSpawnFlags group_flag = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_SETSID
  if (detach) group_flag = POSIX_SPAWN_SETSID;
#endif
  ConfigureShellSpawn(attributes, group_flag);
  int error = SpawnShellWithFallback(shell, command, wrapper, actions,
                                     attributes, environment, pid);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  return error;
}

// master_fd is set only on success; the caller owns it from then on.
int SpawnPtyShell(const std::string& shell, std::string& command,
                  const std::vector<std::string>& wrapper,
                  char* const* environment, pid_t& pid, int& master_fd) {
#if defined(__unix__) || defined(__APPLE__)
  master_fd = -1;
  // Owned here until the child is running; every failure below closes it.
  Fd master(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
  if (!master) return errno;
  if (grantpt(master.Get()) != 0 || unlockpt(master.Get()) != 0) return errno;
  // ptsname() returns a static buffer, and `run` is parallel-safe.
  char slave_path[PATH_MAX];
  const int pts_error = ptsname_r(master.Get(), slave_path, sizeof(slave_path));
  if (pts_error != 0) return pts_error > 0 ? pts_error : errno;
  const char* slave_name = slave_path;
  winsize initial_size{};
  initial_size.ws_row = 24;
  initial_size.ws_col = 80;
  (void)ioctl(master.Get(), TIOCSWINSZ, &initial_size);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, slave_name, O_RDWR,
                                   0);
  posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, master.Get());
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  PosixSpawnFlags group_flag = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_SETSID
  group_flag = POSIX_SPAWN_SETSID;
#endif
  ConfigureShellSpawn(attributes, group_flag);
  int error = SpawnShellWithFallback(shell, command, wrapper, actions,
                                     attributes, environment, pid);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (error == 0) master_fd = master.Release();
  return error;
#else
  (void)shell;
  (void)command;
  (void)wrapper;
  (void)environment;
  (void)pid;
  (void)master_fd;
  return ENOTSUP;
#endif
}

void SignalShellGroup(pid_t pid, int signal_number) {
  if (kill(-pid, signal_number) != 0) (void)kill(pid, signal_number);
}

bool WaitForTerminal(ProcessSupervisor& supervisor,
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

ToolResult JobLimitError(int64_t max_jobs) {
  return ToolFailure(
      ToolErrorCode::kLimitExceeded,
      "error: background job limit reached (" + std::to_string(max_jobs) + ")");
}

// A child refused for headroom must not read as the pool being full: the
// parent can still run its own commands, and that is the point.
ToolResult DelegatedJobLimitError(int64_t max_children, int64_t max_jobs) {
  return ToolFailure(ToolErrorCode::kLimitExceeded,
                     "error: no free background slot for a delegated child (at "
                     "most " +
                         std::to_string(max_children) +
                         " concurrent children of " + std::to_string(max_jobs) +
                         " background slots; the rest stay reserved for this "
                         "agent's own commands). Wait for a child to finish");
}

// Resolves the wrapper argv for one spawn, or reports why this command cannot
// run. Both failures are the same rule: a session that was told to confine and
// cannot must not spawn anyway. That would be a silent unsandboxed command --
// the one outcome the sandbox exists to rule out, arriving without a word.
std::string SandboxWrapperFor(const ShellCommand& spec,
                              std::vector<std::string>* wrapper) {
  // Yolo is an explicit session-wide choice to run without approval or OS
  // confinement. Read it per spawn so /yolo takes effect immediately and
  // toggling it off restores the configured sandbox for the next command.
  if (!spec.sandbox || ApprovalIsAutomatic()) return {};
  const SandboxStatus& status = SandboxRuntime();
  if (status.mode == SandboxMode::kRefused) {
    return "error: UAGENT_SANDBOX is on but cannot be enforced: " +
           status.reason + ". Set UAGENT_SANDBOX=0 to run commands unconfined";
  }
  if (status.mode != SandboxMode::kEnforced) return {};
  *wrapper = SandboxWrapperArgv(status);
  if (wrapper->empty()) {
    return "error: the sandbox policy is too large to enforce (" +
           std::to_string(status.policy.writable_roots.size()) +
           " writable roots); remove entries from UAGENT_SANDBOX_WRITE";
  }
  return {};
}

// A refused write arrives as the kernel's errno, which names a permission and
// not the policy that withheld it -- so a command that failed for the one
// reason this session introduced is the one that explains itself worst. Added
// only to a wrapped command that actually failed, and it points at the place
// the writable roots are listed rather than repeating them here.
std::string SandboxHint(const std::string& output) {
  for (std::string_view denial :
       {"Operation not permitted", "Permission denied"}) {
    if (output.find(denial) != std::string::npos) {
      return "\n[sandbox: writes are confined to the workspace; /context lists "
             "every writable root]";
    }
  }
  return {};
}

// A detached terminal outlives the turn that starts it: its output goes to a
// rotating log through a pump child, it has no session, no pipe and no
// deadline, and an identical live command is reused rather than started twice.
ShellCommandResult StartDetachedShell(ProcessSupervisor& supervisor,
                                      ShellCommand& spec,
                                      const std::vector<std::string>& wrapper) {
  const std::string& cmd = spec.command;
  if (std::optional<DetachedActivity> existing =
          FindRunningDetachedActivity(cmd)) {
    return {ToolSuccess("[detached] pid " + std::to_string(existing->pid) +
                        ", log: " + existing->log +
                        " — reused existing live activity id " +
                        std::to_string(existing->pid) +
                        "; verify readiness with activity output")};
  }
  int64_t max_jobs = MaxBackgroundJobs();
  std::string log;
  Fd lfd(CreateTempFile(UagentDir(kTerminalLogsDir) + "/pending-" +
                            std::to_string(getpid()) + "-XXXXXX",
                        log));
  if (!lfd) {
    return {ToolFailure(ToolErrorCode::kInternal,
                        "error: cannot create log file " + log)};
  }
  fchmod(lfd.Get(), kPrivateFileMode);
  std::string bounded_cmd =
      "set -o pipefail; (" + cmd + ") 2>&1 | " + ShellQuote(ExecutablePath()) +
      " --log-pump " + ShellQuote(log) + " " + std::to_string(BashLogBytes());
  pid_t pid = -1;
  ChildEnvironment child_environment(spec.environment, spec.environment_policy);
  int spawn_error =
      SpawnLoggedShell(spec.shell, bounded_cmd, wrapper, lfd.Get(),
                       /*detach=*/true, child_environment.Data(), pid);
  lfd.Reset();
  if (spawn_error != 0) {
    unlink(log.c_str());
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
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
  BgJob job{pid,
            log,
            cmd,
            true,
            spec.job_kind,
            pid,
            nullptr,
            std::move(spec.activity_label),
            std::move(spec.receipt_path),
            std::move(spec.source_id)};
  if (!supervisor.TryAdd(std::move(job), max_jobs)) {
    return fail_and_reap(JobLimitError(max_jobs));
  }
  return {ToolSuccess("[detached] pid " + std::to_string(pid) + ", log: " +
                      log + " — activity id " + std::to_string(pid) +
                      "; verify readiness with activity output"),
          std::nullopt, /*launched=*/true};
}

}  // namespace

ShellCommandResult RunShellCommand(ProcessSupervisor& supervisor,
                                   const ToolContext& context,
                                   ShellCommand spec) {
  const std::string& cmd = spec.command;
  const std::string& shell = spec.shell;
  if (spec.yield_ms > 0) {
    spec.yield_ms = std::clamp(spec.yield_ms, kMinYieldMs, kMaxYieldMs);
  }
  if (shell.empty() || shell.find('\0') != std::string::npos) {
    return {ToolFailure(
        ToolErrorCode::kInvalidArguments,
        "error: shell must be a non-empty executable name or path")};
  }
  std::vector<std::string> wrapper;
  if (std::string error = SandboxWrapperFor(spec, &wrapper); !error.empty()) {
    return {ToolFailure(ToolErrorCode::kUnavailable, error)};
  }
  if (spec.detach) return StartDetachedShell(supervisor, spec, wrapper);

  // Everything below is the supervised foreground lifecycle.
  int64_t max_jobs = MaxBackgroundJobs();
  bool is_subagent =
      ParseActivityKind(spec.job_kind) == ActivityKind::kSubagent;
  int64_t max_children = std::max<int64_t>(1, max_jobs - kDelegatedJobHeadroom);
  std::optional<ActivityReservation> reservation =
      supervisor.ReserveActivity(max_jobs, is_subagent ? max_children : 0);
  if (!reservation) {
    return {is_subagent ? DelegatedJobLimitError(max_children, max_jobs)
                        : JobLimitError(max_jobs)};
  }
  int64_t window =
      spec.immediate ? 0 : context.RemainingSeconds(int64_t{1} << 30);
  std::string log;
  Fd lfd(CreateTempFile(
      UagentDir(kBgDir) + "/pending-" + std::to_string(getpid()) + "-XXXXXX",
      log));
  if (!lfd) {
    return {ToolFailure(ToolErrorCode::kInternal,
                        "error: cannot create log file " + log)};
  }
  fchmod(lfd.Get(), kPrivateFileMode);
  int64_t log_bytes = BashLogBytes();
  int64_t interaction_cap = ActivityOutputCap(spec.max_output_chars);
  std::string bounded_cmd = cmd;
  pid_t pid = -1;
  int master_fd = -1;
  int pipe_fds[2] = {-1, -1};
  bool tty = spec.tty;
  if (!tty && pipe(pipe_fds) != 0) {
    unlink(log.c_str());
    return {ToolFailure(ToolErrorCode::kInternal,
                        "error: cannot create process output pipe")};
  }
  // Owned from here: every early return closes them, and only the hand-off to
  // the supervisor releases them.
  Fd pipe_read(pipe_fds[0]), pipe_write(pipe_fds[1]);
  auto session = std::make_shared<ActivitySession>();
  session->tty = tty;
  ChildEnvironment child_environment(spec.environment, spec.environment_policy);
  int spawn_error =
      tty ? SpawnPtyShell(shell, bounded_cmd, wrapper, child_environment.Data(),
                          pid, master_fd)
          : SpawnLoggedShell(shell, bounded_cmd, wrapper, pipe_fds[1],
                             /*detach=*/false, child_environment.Data(), pid);
  pipe_write.Reset();
  Fd master(master_fd);
  if (spawn_error != 0) {
    unlink(log.c_str());
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "error: cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
  // Named after the pid and the activity id: a pid alone is reused within one
  // run and a rename onto a completed job's uncollected log would destroy it,
  // while an activity id alone counts from the same base in every process --
  // and a child agent shares this directory with its parent.
  std::string named = UagentDir(kBgDir) + "/" + std::to_string(getpid()) + "-" +
                      std::to_string(reservation->Id()) + ".log";
  if (rename(log.c_str(), named.c_str()) == 0) {
    log = named;  // child's fd stays valid
  }

  Fd input(tty ? dup(master.Get()) : -1);
  if (tty && !input) {
    KillProcess(pid);
    RemoveLog(log);
    return {ToolFailure(
        ToolErrorCode::kInternal,
        "error: cannot duplicate PTY input: " + std::string(strerror(errno)))};
  }

  BgJob foreground{pid,
                   log,
                   cmd,
                   false,
                   spec.job_kind,
                   reservation->Id(),
                   session,
                   std::move(spec.activity_label),
                   std::move(spec.receipt_path),
                   std::move(spec.source_id),
                   std::move(spec.completion_notes)};
  std::optional<int64_t> registered = reservation->Register(foreground);
  if (!registered) {
    KillProcess(pid);
    RemoveLog(log);
    return {JobLimitError(max_jobs)};
  }
  int64_t activity_id = *registered;
  int output_fd = tty ? master.Release() : pipe_read.Release();
  supervisor.RegisterIo(session, output_fd, input.Release(), lfd.Release(),
                        log_bytes);

  TrackPid(g_child_pgids, kFgMax, pid, true);
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
    if (handed_off || exited || std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (AbortRequested()) {
      cancelled = true;
      SignalShellGroup(pid, SIGKILL);
      supervisor.Wake();
    }
    supervisor.WaitForChange(generation, deadline);
  }

  if (cancelled) {
    auto stop_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    exited = WaitForTerminal(supervisor, session, stop_deadline);
  }
  // Joined before it is dropped, so a Ctrl-C landing in the handover still
  // reaches the child; every path below that does not background it takes the
  // registration back out.
  BgTrackSignal(pid, true);
  TrackPid(g_child_pgids, kFgMax, pid, false);

  auto finish = [&](auto build) {
    int status = 0;
    std::string output;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      status = session->wait_status.value_or(0);
      output = LimitOutput(session->transcript.Snapshot(), interaction_cap);
    }
    CollectedLog collected = CollectCompletedLog(
        log, ToolResultCap(),
        /*failed=*/!(WIFEXITED(status) && WEXITSTATUS(status) == 0));
    if (collected.artifact) output = std::move(collected.output);
    ToolResult result = build(std::move(output), status);
    if (!result.Ok() && !wrapper.empty()) {
      result.output += SandboxHint(result.output);
    }
    result.artifact = std::move(collected.artifact);
    return ShellCommandResult{std::move(result), status, /*launched=*/true};
  };

  if (cancelled) {
    BgTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    RemoveLog(log);
    return {ToolCancelled("error: command cancelled by user"), std::nullopt,
            /*launched=*/true};
  }
  if (exited) {
    BgTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    return finish([](std::string output, int status) {
      output += FmtExit(status, /*show_ok=*/false);
      return ProcessResult(std::move(output), status);
    });
  }
  if (!spec.background && !handed_off && spec.yield_ms <= 0) {
    BgTrackSignal(pid, false);
    (void)supervisor.RemoveForeground(pid);
    SignalShellGroup(pid, SIGKILL);
    auto stop_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    WaitForTerminal(supervisor, session, stop_deadline);
    return finish([](std::string output, int) {
      if (!output.empty() && output.back() != '\n') output += '\n';
      output += "error: command exceeded its execution deadline";
      return ToolTimedOut(std::move(output));
    });
  }

  std::string subagent_label =
      spec.job_kind.empty() ? "subagent" : spec.job_kind;
  std::optional<BgJob> moved = supervisor.MoveForegroundToBackground(pid);
  if (!moved) {
    BgTrackSignal(pid, false);
    SignalShellGroup(pid, SIGKILL);
    RemoveLog(log);
    return {ToolFailure(ToolErrorCode::kInternal,
                        "error: foreground activity ownership was lost"),
            std::nullopt, /*launched=*/true};
  }
  if (is_subagent) {
    return {ToolSuccess("[started] " + subagent_label + " id " +
                        std::to_string(activity_id) +
                        "; completion is added to the next natural model call "
                        "without starting one; inspect activity output for "
                        "progress/readiness, or wait when the next step is "
                        "blocked"),
            std::nullopt, /*launched=*/true};
  }
  std::string initial_output;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    initial_output =
        LimitOutput(session->pending_output.Drain(), interaction_cap);
    session->last_used = std::chrono::steady_clock::now();
  }
  std::string output = "[running] activity " + std::to_string(activity_id) +
                       (handed_off ? " moved to background; " : "; ") +
                       "it keeps running while you work; poll or wait on it "
                       "for output";
  if (!initial_output.empty()) output += "\n" + initial_output;
  return {ToolSuccess(std::move(output)), std::nullopt, /*launched=*/true};
}

ToolResult ToolRunApprovedShell(ProcessSupervisor& supervisor,
                                const std::string& command,
                                const ToolContext& context, bool detach,
                                const std::string& shell, bool tty,
                                int64_t yield_ms, int64_t max_output_chars,
                                bool sandbox) {
  return RunShellCommand(
             supervisor, context,
             {.command = command,
              .shell = shell,
              .background = detach,
              .detach = detach,
              .tty = tty,
              .sandbox = sandbox,
              .yield_ms = yield_ms,
              .max_output_chars = max_output_chars,
              .environment_policy = ChildEnvironmentPolicy::kApprovedShell})
      .result;
}

bool StartsWithShellWord(const std::string& command, const std::string& word) {
  std::string trimmed = Trim(command);
  return trimmed == word ||
         (trimmed.starts_with(word) && trimmed.size() > word.size() &&
          std::isspace(static_cast<unsigned char>(trimmed[word.size()])));
}

std::string PrivilegedCommandError(const std::string& command) {
  if (StartsWithShellWord(command, "sudo")) {
    return "error: privileged commands are unavailable. Do not use sudo; "
           "use workspace or user-local tools, or adapt to installed "
           "dependencies.";
  }
  return "";
}

// The same rule read over a script instead of a command. `run`'s check looks
// at a first word, so against a multi-line body it would only ever inspect
// line one. Only the privileged-command half carries over: the other half
// routes bare Python at `scratch`, which is circular advice to give from
// inside it.
//
// This is parity with `run`, not better than it -- neither sees `x && sudo y`,
// because neither parses shell. The boundary that does hold is the OS sandbox,
// and a scratch script is always inside it.
std::string ScriptCommandPolicyError(const std::string& script) {
  size_t number = 0;
  for (size_t at = 0; at <= script.size();) {
    size_t end = script.find('\n', at);
    if (end == std::string::npos) end = script.size();
    std::string line = Trim(script.substr(at, end - at));
    ++number;
    if (!line.empty() && line.front() != '#') {
      std::string error = PrivilegedCommandError(line);
      if (!error.empty()) {
        return "error: line " + std::to_string(number) + ": " +
               error.substr(std::string_view("error: ").size());
      }
    }
    if (end == script.size()) break;
    at = end + 1;
  }
  return "";
}

std::string RunCommandPolicyError(const std::string& command) {
  std::string privileged = PrivilegedCommandError(command);
  if (!privileged.empty()) return privileged;
  for (const char* executable : {"python", "python3", "pip", "pip3"}) {
    if (StartsWithShellWord(command, executable)) {
      return "error: do not invoke bare Python or pip through run. For project "
             "Python use its existing runner (for example uv run or pytest); "
             "for one-off computation use scratch with dependencies in the "
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

ToolResult ToolRunScratch(ProcessSupervisor& supervisor,
                          const std::filesystem::path& workspace,
                          const std::string& relative_path, const json& code,
                          const json& packages, const ToolContext& context) {
  namespace fs = std::filesystem;
  constexpr std::string_view kScratchPrefix = ".uagent/scratch/";
  fs::path requested(relative_path.starts_with(kScratchPrefix)
                         ? relative_path.substr(kScratchPrefix.size())
                         : relative_path);
  const bool shell_script = requested.extension() == ".sh";
  if (relative_path.empty() || requested.is_absolute() ||
      (!shell_script && requested.extension() != ".py")) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: path must be a relative .py or .sh file under "
                       ".uagent/scratch");
  }
  for (const fs::path& component : requested) {
    if (component == "..") {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: scratch script path must not contain ..");
    }
  }

  fs::path scratch = workspace / ".uagent" / "scratch";
  std::error_code ec;
  fs::create_directories(scratch, ec);
  if (ec) {
    return ToolFailure(
        ToolErrorCode::kInternal,
        "error: cannot create scratch directory: " + ec.message());
  }
  scratch = fs::canonical(scratch, ec);
  fs::path script = CanonicalAccessPath((scratch / requested).string());
  if (ec || !PathWithin(script, scratch)) {
    return ToolFailure(ToolErrorCode::kPermissionDenied,
                       "error: scratch script path escapes .uagent/scratch");
  }

  std::string write_error;
  fs::path ignore = scratch / ".gitignore";
  if (!fs::exists(ignore, ec) &&
      !AtomicWriteFile(ignore.string(), "*\n", kSharedFileMode,
                       /*preserve_mode=*/false, write_error)) {
    return ToolFailure(ToolErrorCode::kInternal, "error: " + write_error);
  }

  bool create = code.is_string();
  bool replaced = false;
  std::string source;
  std::string prior;
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
          "error: cannot inspect scratch script: " + ec.message());
    }
    if (exists && !fs::is_regular_file(script, ec)) {
      return ToolFailure(
          ToolErrorCode::kInvalidArguments,
          "error: scratch path exists but is not a regular file: " +
              requested.generic_string());
    }
    std::string body = code.get<std::string>();
    if (body.find('\0') != std::string::npos) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: script code contains NUL");
    }
    if (shell_script) {
      if (!packages.empty()) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: a .sh script takes no packages; pass [] and "
                           "install nothing, or use a .py script under uv");
      }
      source = body;
      if (source.empty() || source.back() != '\n') source += '\n';
    } else {
      if (body.find("# /// script") != std::string::npos) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: code must contain only the script body; "
                           "packages generate the PEP 723 header");
      }
      source = "# /// script\n# dependencies = [\n";
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
    }
    if (exists) {
      std::ifstream prior_input(script);
      prior.assign(std::istreambuf_iterator<char>(prior_input),
                   std::istreambuf_iterator<char>());
      if (prior == source) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "error: code is identical to .uagent/scratch/" +
                               requested.generic_string() +
                               "; rerun with code=null and packages=null");
      }
      replaced = true;
    }
    if (!AtomicWriteFile(script.string(), source, kSharedFileMode,
                         /*preserve_mode=*/true, write_error)) {
      return ToolFailure(ToolErrorCode::kInternal, "error: " + write_error);
    }
  } else if (!fs::is_regular_file(script, ec)) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        "error: scratch script does not exist: " + relative_path);
  }

  if (!create) {  // a rerun executes whatever is on disk now
    std::ifstream input(script);
    source.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
  }
  std::string command;
  if (shell_script) {
    // Checked here rather than at write: a rerun executes whatever is on disk,
    // and the file is writable by edit_file between the two.
    std::string policy = ScriptCommandPolicyError(source);
    if (!policy.empty()) {
      return ToolFailure(ToolErrorCode::kPermissionDenied, policy);
    }
    command = "sh " + ShellQuote(script.string());
  } else {
    bool uv = ExecutableOnPath("uv");
    if (!uv && PythonScriptHasDependencies(source)) {
      return ToolFailure(
          ToolErrorCode::kUnavailable,
          "error: this script declares third-party dependencies and requires "
          "uv on PATH. Install uv or edit the PEP 723 dependency list");
    }
    command =
        uv ? "UV_NO_PROGRESS=1 MPLBACKEND=Agg uv run --quiet --no-project "
             "--script " +
                 ShellQuote(script.string())
           : "MPLBACKEND=Agg python3 " + ShellQuote(script.string());
  }
  ShellCommandResult result =
      RunShellCommand(supervisor, context, {.command = std::move(command)});
  if (result.result.error == ToolErrorCode::kProcessFailed && !shell_script) {
    std::string hint =
        result.result.output.find("No module named") != std::string::npos
            ? " Add every third-party dependency to the script's PEP 723 "
              "header; do not install it with pip or run."
            : "";
    result.result.output =
        "error: Python execution failed." + hint + "\n" + result.result.output;
  }
  if (create) {
    result.result.display =
        WholeFileDiffDisplay(script.string(), prior, source, replaced);
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
  // Ripgrep searches files in parallel and reports them in completion order, so
  // two identical searches can disagree. That is not cosmetic here: results are
  // truncated at a cap, so an unordered search returns an arbitrary subset, and
  // a repeated search looks changed when nothing changed. Path order costs
  // ripgrep's parallelism and buys a result the agent can compare and reuse.
  const char* sorted = " --sort path";
  std::string command;
  if (files_only) {
    if (ripgrep) {
      command = std::string("rg --files --color=never") + sorted;
      if (!glob.empty()) command += " --glob " + ShellQuote(glob);
      command += " -- " + ShellQuote(target) +
                 " | rg --line-number --color=never -- " + ShellQuote(pattern);
    } else {
      command = "find " + ShellQuote(target) + " -type f";
      if (!glob.empty()) command += " -name " + ShellQuote(glob);
      command += " -print | grep -E -n -- " + ShellQuote(pattern);
    }
  } else {
    command = ripgrep ? std::string(
                            "rg --line-number --column --no-heading "
                            "--color=never") +
                            sorted
                      : std::string("grep -r -E -n -H -I --exclude-dir=.git");
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
  // PIPESTATUS[0], not pipefail: head closing the pipe makes the whole
  // pipeline exit 141, which would read as "truncated" and swallow a producer
  // error. The producer's own 141 still means head cut it short.
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
