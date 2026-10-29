// Copyright 2026 Timon Gentzsch

#include "include/tools/shell.h"

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

// The shell that `bash` means on this host. Only needed under a sandbox
// wrapper: there the spawn resolves the wrapper, so a missing shell surfaces
// as the wrapper's own failure rather than as an errno the retry below could
// read. Resolving in the parent keeps the same Bash candidates in the same
// order.
std::string ResolveShellExecutable(const std::string& shell) {
  if (shell != "bash") return shell;
  for (const char* candidate : {"/bin/bash"}) {
    if (access(candidate, X_OK) == 0) return candidate;
  }
  return shell;
}

// `program` is the argv to run: `shell -c command`, or a program run directly.
// It gets its own process group (or session), the default signal dispositions
// and `fds`. Returns the posix_spawnp errno; `pid` is set on 0.
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
    // The program is the wrapper when there is one; the shell is then just its
    // first argument, and the rest stays unwrapped either way.
    spec.argv = wrapper;
    spec.argv.push_back(executable);
    spec.argv.insert(spec.argv.end(), program.begin() + 1, program.end());
    return SpawnProcess(spec, pid);
  };
  int error = spawn(wrapper.empty() ? shell : ResolveShellExecutable(shell));
  // `bash` is the documented default, so fall back to the usual absolute
  // path before giving up on a PATH that does not have it. Do not silently
  // downgrade to sh: its pipelines may mask failures.
  const bool fallback = wrapper.empty() && shell == "bash";
  if (error != 0 && fallback) error = spawn("/bin/bash");
  return error;
}

// Spawn `program` with stdin at /dev/null and both output streams on the log,
// in a session of its own when `detach`.
int SpawnLoggedShell(const std::vector<std::string>& program,
                     const std::vector<std::string>& wrapper, int log_fd,
                     bool detach, char* const* environment, pid_t& pid) {
  return SpawnShellWithFallback(
      program, wrapper,
      {{STDIN_FILENO}, {STDOUT_FILENO, log_fd}, {STDERR_FILENO, log_fd}},
      detach, environment, pid);
}

// master_fd is set only on success; the caller owns it from then on.
int SpawnPtyShell(const std::vector<std::string>& program,
                  const std::vector<std::string>& wrapper,
                  char* const* environment, pid_t& pid, int& master_fd) {
  master_fd = -1;
  // Owned here until the child is running; every failure below closes it.
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
// up after itself (a browser removing its temporary profile) before it is
// killed.
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

// Resolves the wrapper argv for one spawn, or reports why this command cannot
// run. Both failures are the same rule: a session that was told to confine and
// cannot must not spawn anyway. That would be a silent unsandboxed command --
// the one outcome the sandbox exists to rule out, arriving without a word.
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

// A refused write arrives as the kernel's errno, which names a permission and
// not the policy that withheld it, and a refused connection as a failed name
// lookup -- so a command that failed for the one reason this session
// introduced is the one that explains itself worst. Added only to a wrapped
// command that actually failed, and it names the roots: the model cannot run
// /context, and probing for them costs it a round per guess.
std::string SandboxHint(const SandboxStatus& status,
                        const std::string& output) {
  static constexpr std::string_view kDenied[] = {"Operation not permitted",
                                                 "Permission denied"};
  static constexpr std::string_view kOffline[] = {
      "Could not resolve host",    "nodename nor servname",
      "Name or service not known", "Temporary failure in name resolution",
      "Network is unreachable",    "ENOTFOUND"};
  const auto says = [&](const auto& phrases) {
    return std::ranges::any_of(phrases, [&](std::string_view phrase) {
      return output.find(phrase) != std::string::npos;
    });
  };
  if (says(kDenied) || (!status.policy.allow_network && says(kOffline))) {
    return "[sandbox: " + SandboxNote(status) + "]";
  }
  return {};
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

// A detached terminal outlives the turn that starts it: its output goes to a
// rotating log through a pump child, it has no session, no pipe and no
// deadline, and an identical live command is reused rather than started twice.
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
               .receipt_path = std::move(spec.receipt_path),
               .source_id = std::move(spec.source_id),
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

  // Everything below is the supervised foreground lifecycle.
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
  // Owned from here: every early return closes them, and only the hand-off to
  // the supervisor releases them.
  Fd pipe_read(pipe_fds[0]), pipe_write(pipe_fds[1]);
  auto session = std::make_shared<ActivitySession>();
  session->tty = tty;
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
                             child_environment.Data(), pid);
  pipe_write.Reset();
  Fd master(master_fd);
  if (spawn_error != 0) {
    return {ToolFailure(
        ToolErrorCode::kUnavailable,
        "cannot spawn shell: " + std::string(strerror(spawn_error)))};
  }
  // Named after the pid and the activity id: a pid alone is reused within one
  // run and a rename onto a completed activity's uncollected log would destroy
  // it, while an activity id alone counts from the same base in every process
  // -- and a child agent shares this directory with its parent.
  std::string named = UagentDir(kActivityDir) + "/" + std::to_string(getpid()) +
                      "-" + std::to_string(reservation->Id()) + ".log";
  if (rename(pending.Path().c_str(), named.c_str()) == 0) {
    log = named;  // child's fd stays valid
  }
  // The supervisor owns the log from here; `log` already holds whichever
  // path won above. ReleaseFd keeps the file and hands over the descriptor;
  // Release would close it while the supervisor still needs to tail it.
  int log_fd = pending.ReleaseFd();

  Fd input(tty ? dup(master.Get()) : -1);
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
                      .kind = spec.activity_kind,
                      .id = reservation->Id(),
                      .session = session,
                      .display_label = std::move(spec.activity_label),
                      .receipt_path = std::move(spec.receipt_path),
                      .source_id = std::move(spec.source_id),
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
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      status = session->wait_status.value_or(0);
      output = LimitOutput(session->transcript.Snapshot(), interaction_cap);
    }
    // The transcript above is what the caller reads; the log is only kept.
    std::optional<ToolArtifact> artifact =
        KeepCompletedLog(log, ToolResultCap(),
                         /*failed=*/!ExitedOk(status));
    ToolResult result = build(std::move(output), status);
    result.body_kind = "tail";
    if (!result.Ok() && !wrapper.empty()) {
      AddHint(result, SandboxHint(sandbox, result.output));
    }
    AddHint(result, SandboxRestoreNote(watch.get(), CanonicalCwd()));
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
    return finish([&cmd](std::string output, int status) {
      ToolResult result = ExitResult(std::move(output), status);
      // pkill -f and its kin match on the whole command line, and this
      // command's own shell carries the pattern in its.
      if (WIFSIGNALED(status) && (cmd.find("pkill -f") != std::string::npos ||
                                  cmd.find("pgrep -f") != std::string::npos)) {
        AddHint(result,
                "[the pattern matched this command's own shell; match the "
                "process name instead, e.g. pkill -x NAME]");
      }
      return result;
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
  // An interrupt that arrived while a command being waited for was handed
  // over stops it all the same: nothing else would, once it is in the
  // background.
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

std::string ScratchArgvLabel(const json& args) {
  std::string label;
  if (!args.is_array()) return label;
  for (const json& value : args) {
    if (!value.is_string()) continue;
    const std::string& arg = value.get_ref<const std::string&>();
    label += ' ';
    label += arg.empty() || arg.find_first_of(" \t\n'\"\\") != std::string::npos
                 ? ShellQuote(arg)
                 : arg;
  }
  return label;
}

namespace {
// How many changed entries a scratch result names before it counts the rest.
constexpr size_t kChangedNames = 20;

// The folder root's entries and when each was last written. A script runs
// in the folder, so this is where the files it names without a folder land;
// a folder it wrote into shows as changed itself. µAgent's own is left out.
std::map<std::string, std::filesystem::file_time_type> RootEntries(
    const std::filesystem::path& workspace) {
  namespace fs = std::filesystem;
  std::map<std::string, fs::file_time_type> entries;
  std::error_code ec;
  for (fs::directory_iterator entry(workspace, ec), end; !ec && entry != end;
       entry.increment(ec)) {
    std::string name = entry->path().filename().string();
    if (name == kWorkspaceDir) {
      continue;
    }
    std::error_code ignored;
    if (entry->is_directory(ignored)) name += '/';
    entries[std::move(name)] = entry->last_write_time(ignored);
  }
  return entries;
}
}  // namespace

std::optional<std::filesystem::path> ScratchScriptPath(
    const std::filesystem::path& workspace, const std::string& relative_path,
    std::string& error) {
  namespace fs = std::filesystem;
  constexpr std::string_view kScratchPrefix = ".uagent/scratch/";
  fs::path requested(relative_path.starts_with(kScratchPrefix)
                         ? relative_path.substr(kScratchPrefix.size())
                         : relative_path);
  if (relative_path.empty() || requested.is_absolute() ||
      (requested.extension() != ".sh" && requested.extension() != ".py" &&
       requested.extension() != ".js" && requested.extension() != ".mjs")) {
    error =
        "path must be a relative .py, .sh, .js or .mjs file under "
        ".uagent/scratch";
    return std::nullopt;
  }
  for (const fs::path& component : requested) {
    if (component == "..") {
      error = "scratch script path must not contain ..";
      return std::nullopt;
    }
  }
  fs::path scratch = ScratchDir(workspace);
  fs::path script = CanonicalAccessPath((scratch / requested).string());
  if (!PathWithin(script, CanonicalAccessPath(scratch.string()))) {
    error = "scratch script path escapes .uagent/scratch";
    return std::nullopt;
  }
  return script;
}

ToolResult ToolRunScratch(ActivitySupervisor& supervisor,
                          const std::filesystem::path& workspace,
                          const std::string& relative_path, const json& args,
                          const ToolContext& context) {
  namespace fs = std::filesystem;
  std::string error;
  const auto script = ScratchScriptPath(workspace, relative_path, error);
  if (!script) {
    return ToolFailure(ToolErrorCode::kInvalidArguments, error);
  }
  std::error_code ec;
  if (!fs::is_regular_file(*script, ec)) {
    return ToolFailure(
        ToolErrorCode::kNotFound,
        "no script at .uagent/scratch/" +
            fs::relative(*script, ScratchDir(workspace), ec).generic_string() +
            "; this call supplied no code. Pass code with path to scratch "
            "to create and run it, or write the file first with write_file");
  }
  // The scratch directory is private to the agent: keep it out of git.
  const fs::path ignore = script->parent_path() / ".gitignore";
  std::string write_error;
  if (script->parent_path().filename() == kScratchDir &&
      !fs::exists(ignore, ec) &&
      !AtomicWriteFile(ignore.string(), "*\n", kSharedFileMode,
                       /*preserve_mode=*/false, write_error)) {
    return ToolFailure(ToolErrorCode::kInternal, write_error);
  }
  if (!args.is_null() && !args.is_array()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "args must be an array of strings");
  }
  std::string argv;
  if (args.is_array()) {
    for (const json& value : args) {
      if (!value.is_string() ||
          value.get_ref<const std::string&>().find('\0') != std::string::npos) {
        return ToolFailure(ToolErrorCode::kInvalidArguments,
                           "args must be strings without NUL");
      }
      argv += ' ' + ShellQuote(value.get<std::string>());
    }
  }
  const bool shell_script = script->extension() == ".sh";
  std::string command;
  if (shell_script) {
    command = "sh " + ShellQuote(script->string()) + argv;
  } else if (script->extension() == ".js" || script->extension() == ".mjs") {
    if (!ExecutableOnPath("node")) {
      return ToolFailure(ToolErrorCode::kUnavailable,
                         "JavaScript scratch scripts require node on PATH");
    }
    command = "node " + ShellQuote(script->string()) + argv;
  } else {
    const std::string source = ReadFile(*script, EditFileBytes()).value_or("");
    const bool uv = ExecutableOnPath("uv");
    if (!uv && PythonScriptHasDependencies(source)) {
      return ToolFailure(
          ToolErrorCode::kUnavailable,
          "this script declares third-party dependencies and requires "
          "uv on PATH. Install uv or edit the PEP 723 dependency list");
    }
    command =
        uv ? "UV_NO_PROGRESS=1 MPLBACKEND=Agg uv run --quiet --no-project "
             "--script " +
                 ShellQuote(script->string()) + argv
           : "MPLBACKEND=Agg python3 " + ShellQuote(script->string()) + argv;
  }
  const auto before = RootEntries(workspace);
  ShellCommandResult result =
      RunShellCommand(supervisor, context, {.command = std::move(command)});
  // What the script left behind, said with its output: a model that saved a
  // chart by a bare name otherwise spends its next call looking for it.
  if (result.result.Ok()) {
    std::string changed;
    size_t count = 0;
    for (const auto& [name, written] : RootEntries(workspace)) {
      const auto was = before.find(name);
      if (was != before.end() && was->second == written) continue;
      if (++count <= kChangedNames) changed += (count > 1 ? ", " : "") + name;
    }
    if (count > kChangedNames) {
      changed += " +" + std::to_string(count - kChangedNames) + " more";
    }
    if (count > 0) {
      AddHint(result.result, "[changed in the folder: " + changed + "]");
    }
  }
  if (result.result.error == ToolErrorCode::kProcessFailed && !shell_script) {
    std::string hint =
        result.result.output.find("No module named") != std::string::npos
            ? " Add every third-party dependency to the script's PEP 723 "
              "header (# /// script); do not install it with pip or run."
            : "";
    result.result.output =
        "error: Python execution failed." + hint + "\n" + result.result.output;
  }
  if (!result.result.Ok()) {
    AddHint(result.result,
            "[execution " +
                std::string(CompletionStatusName(result.result.status)) + "]");
  }
  return std::move(result.result);
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
