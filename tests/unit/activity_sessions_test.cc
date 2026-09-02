// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/tools/child_agent.h"
#include "include/tools/jobs.h"
#include "include/tools/output_buffer.h"
#include "include/tools/shell.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestSignalAndFileWatch() {
  RequestAbort();
  ClearAbort();
  pollfd stale_abort = {AbortWakeFd(), POLLIN, 0};
  CHECK(poll(&stale_abort, 1, 0) == 1);
  NormalizeAbortWake();
  stale_abort.revents = 0;
  CHECK(poll(&stale_abort, 1, 0) == 0);

  int resize_wake[2] = {-1, -1};
  CHECK(pipe(resize_wake) == 0);
  if (resize_wake[0] >= 0) {
    SetTerminalWakeFd(resize_wake[1]);
    InstallSigwinchHandler();
    raise(SIGWINCH);
    pollfd resize_event = {resize_wake[0], POLLIN, 0};
    CHECK(poll(&resize_event, 1, 100) == 1);
    SetTerminalWakeFd(-1);
    close(resize_wake[0]);
    close(resize_wake[1]);
    g_terminal_resized = 0;
  }

  FileWaitResult missing_file = WaitForFileChange(
      "/tmp/uagent-file-watch-does-not-exist", {},
      std::chrono::steady_clock::now() + std::chrono::milliseconds(30));
  CHECK(missing_file == FileWaitResult::kTimedOut);

  char watched_path[] = "/tmp/uagent-file-watch-XXXXXX";
  int watched_fd = mkstemp(watched_path);
  CHECK(watched_fd >= 0);
  if (watched_fd >= 0) {
    FileStamp observed = SnapshotFile(watched_path);
    FileWaitResult file_changed = FileWaitResult::kTimedOut;
    std::thread watcher([&] {
      file_changed = WaitForFileChange(
          watched_path, observed,
          std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    CHECK(write(watched_fd, "x", 1) == 1);
    watcher.join();
    CHECK(file_changed == FileWaitResult::kChanged);

    observed = SnapshotFile(watched_path);
    FileWaitResult steering_changed = FileWaitResult::kTimedOut;
    std::thread steering_watcher([&] {
      steering_changed = WaitForFileChange(
          watched_path, observed,
          std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    SteeringState().Queue("watch steering");
    steering_watcher.join();
    CHECK(steering_changed == FileWaitResult::kSteering);
    std::vector<std::string> watch_steering = SteeringState().TakeQueued();
    CHECK(watch_steering.size() == 1 && watch_steering[0] == "watch steering");

    // A line submitted while a slash command is still running is queued as
    // steering, and that command never reads the queue. Draining it here is
    // what turns it into the next prompt instead of a line the status bar
    // counts forever and nothing ever runs.
    CHECK(TakeStrandedSteering().empty());
    SteeringState().Queue("first");
    SteeringState().Queue("second");
    CHECK(TakeStrandedSteering() == "first\nsecond");
    CHECK(SteeringState().QueuedCount() == 0);
    CHECK(TakeStrandedSteering().empty());
    close(watched_fd);
    unlink(watched_path);
  }
}

void TestActivityBufferAndAdmission() {
  HeadTailBuffer buffer(10);
  buffer.Push("abcdefghij");
  buffer.Push("klmnop");
  CHECK(buffer.Snapshot().starts_with("abcde"));
  CHECK(buffer.Snapshot().ends_with("lmnop"));
  CHECK(buffer.Snapshot().find("6 bytes omitted") != std::string::npos);
  CHECK(!buffer.Drain().empty());
  CHECK(buffer.Drain().empty());

  // The status row asks for one line and must not pay for the megabyte behind
  // it, so the tail is read where it lands rather than through Snapshot().
  HeadTailBuffer progress;
  CHECK(progress.TailLine().empty());
  progress.Push("· reading\n· writing\n");
  CHECK(progress.TailLine() == "· writing");
  // A process that just printed a newline has not stopped saying what it was
  // saying: trailing blanks name the line before them.
  progress.Push("\n  \n");
  CHECK(progress.TailLine() == "· writing");
  // Bounded at a codepoint boundary, never mid-sequence.
  HeadTailBuffer wide;
  wide.Push("ααα\n");
  CHECK(wide.TailLine(3) == "α");
  // Past the budget the newest bytes are all that survive, and the line comes
  // from those rather than from the prefix the buffer also kept.
  HeadTailBuffer spilled(10);
  spilled.Push("first\nsecond\nthird");
  CHECK(spilled.TailLine() == "third");

  // What the status row reads: delegated children only, each with its newest
  // progress line, and a count that agrees with the list it came from.
  ProcessSupervisor delegating;
  auto delegated = std::make_shared<ActivitySession>();
  {
    std::lock_guard<std::mutex> lock(delegated->mutex);
    delegated->transcript.Push("· reading\n· writing\n");
  }
  CHECK(delegating.TryAdd({899980, "", "child", false, "subagent", 0, delegated,
                           "haiku", "", "agent-1a2b3c4d"},
                          4));
  CHECK(delegating.TryAdd({899981, "", "plain", false, "command"}, 4));
  std::vector<SubagentView> views = delegating.SubagentViews();
  REQUIRE(views.size() == 1);
  CHECK(views[0].source_id == "agent-1a2b3c4d");
  CHECK(views[0].label == "haiku");
  CHECK(views[0].tail == "· writing");
  CHECK(delegating.Count(ActivityKind::kSubagent) == 1);
  CHECK(delegating.Count() == 2);
  // Looking is not draining: the tool that joins the child still needs every
  // byte the transcript holds.
  CHECK(delegating.SubagentViews()[0].tail == "· writing");
  // The child's answer is not progress. Once it prints its JSON envelope the
  // newest line on the stream is the result the parent will deliver whole, so
  // the row says nothing rather than a fragment of it.
  {
    std::lock_guard<std::mutex> lock(delegated->mutex);
    delegated->transcript.Push("{\"format\":3,\"answer\":\"done\"}\n");
  }
  CHECK(delegating.SubagentViews()[0].tail.empty());

  ProcessSupervisor admission;
  std::optional<ActivityReservation> first_slot = admission.ReserveActivity(1);
  CHECK(first_slot.has_value());
  CHECK(!admission.ReserveActivity(1).has_value());
  CHECK(!admission.TryAdd({899999, "", "busy", false, ""}, 1));
  first_slot.reset();
  CHECK(admission.ReserveActivity(1).has_value());

  // The id is fixed at reservation, so a log can be named after it before the
  // job exists, and it is the id the job actually commits under.
  ProcessSupervisor reserved;
  std::optional<ActivityReservation> early = reserved.ReserveActivity(4);
  std::optional<ActivityReservation> later = reserved.ReserveActivity(4);
  REQUIRE(early.has_value());
  REQUIRE(later.has_value());
  CHECK(early->Id() > 0 && later->Id() > early->Id());
  const int64_t promised = early->Id();
  std::optional<int64_t> committed =
      early->Register({899997, "", "reserved", false, "", promised});
  REQUIRE(committed.has_value());
  CHECK(*committed == promised);

  // The subagent flag survives the same move, so a released reservation gives
  // the child counter back; losing it would refuse delegation for the rest of
  // the run.
  ProcessSupervisor children;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::optional<ActivityReservation> child = children.ReserveActivity(4, 1);
    CHECK(child.has_value());
    CHECK(!children.ReserveActivity(4, 1).has_value());
  }

  ProcessSupervisor retained;
  std::vector<int64_t> retained_ids;
  for (int index = 0; index < 17; ++index) {
    auto session = std::make_shared<ActivitySession>();
    CHECK(retained.TryAdd(
        {static_cast<pid_t>(900000 + index), "", "done", false, "", 0, session},
        32));
    int64_t id = ActivityId(retained.Snapshot().back());
    retained_ids.push_back(id);
    std::optional<BgJob> completed = retained.Take(id);
    REQUIRE(completed.has_value());
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      CHECK(TransitionActivityLocked(*session, ActivityState::kExited));
      CHECK(TransitionActivityLocked(*session, ActivityState::kDrained));
    }
    retained.Retain(std::move(*completed));
  }
  CHECK(!retained.Find(retained_ids.front()).has_value());
  CHECK(retained.Find(retained_ids.back()).has_value());
}

void TestActivityStateGraph() {
  ActivitySession session;
  {
    std::lock_guard<std::mutex> lock(session.mutex);
    CHECK(TransitionActivityLocked(session, ActivityState::kRunning));
    CHECK(!TransitionActivityLocked(session, ActivityState::kDrained));
    CHECK(session.state == ActivityState::kRunning);
    CHECK(TransitionActivityLocked(session, ActivityState::kExited));
    CHECK(TransitionActivityLocked(session, ActivityState::kDrained));
    CHECK(TransitionActivityLocked(session, ActivityState::kDelivered));
    CHECK(!TransitionActivityLocked(session, ActivityState::kStopped));
  }

  auto stopped = std::make_shared<ActivitySession>();
  {
    std::lock_guard<std::mutex> stopped_lock(stopped->mutex);
    CHECK(TransitionActivityLocked(*stopped, ActivityState::kRunning));
    CHECK(TransitionActivityLocked(*stopped, ActivityState::kStopped));
    CHECK(!TransitionActivityLocked(*stopped, ActivityState::kDelivered));
  }
  ProcessSupervisor supervisor;
  CHECK(supervisor.TryAdd({899990, "", "stopped", false, "", 0, stopped}, 1));
  int64_t id = ActivityId(supervisor.Snapshot().front());
  std::optional<BgJob> job = supervisor.Take(id);
  REQUIRE(job.has_value());
  supervisor.Retain(std::move(*job));
  CHECK(!supervisor.Find(id).has_value());
}

void TestActivitySessions() {
  ToolContext context{std::chrono::steady_clock::now() +
                      std::chrono::seconds(10)};
  ProcessSupervisor automatic_yield;
  setenv("UAGENT_RUN_YIELD_MS", "250", 1);
  std::vector<Tool> yield_tools = BuiltinTools(automatic_yield);
  const Tool* public_run = FindTool(yield_tools, "run");
  CHECK(public_run != nullptr);
  if (public_run) {
    ToolResult yielded = public_run->run({{"command", "sleep 5"}}, context);
    CHECK(yielded.Ok());
    CHECK(yielded.output.find("[running] activity") != std::string::npos);
    for (const BgJob& job : automatic_yield.Snapshot()) {
      CHECK(ToolActivityStop(automatic_yield, ActivityId(job)).Ok());
    }
  }
  unsetenv("UAGENT_RUN_YIELD_MS");

  ProcessSupervisor pty_processes;
  ShellCommandResult started = RunShellCommand(
      pty_processes, context,
      {.command = "if [ -t 0 ]; then echo tty=yes; else echo tty=no; fi; "
                  "read value; echo got:$value",
       .background = false,
       .tty = true,
       .yield_ms = 250});
  CHECK(started.result.Ok());
  std::vector<BgJob> pty_jobs = pty_processes.Snapshot();
  CHECK(pty_jobs.size() == 1);
  if (!pty_jobs.empty()) {
    int64_t id = ActivityId(pty_jobs[0]);
    CHECK(id != pty_jobs[0].pid);
    CHECK(started.result.output.find("tty=yes") != std::string::npos);
    std::vector<Tool> pty_tools = BuiltinTools(pty_processes);
    const Tool* activity = FindTool(pty_tools, "activity");
    CHECK(activity != nullptr);
    ToolResult initial =
        activity ? activity->run({{"operation", "poll"}, {"id", id}}, context)
                 : ToolFailure(ToolErrorCode::kInternal, "missing activity");
    CHECK(initial.output.find("(no new output)") != std::string::npos);
    CHECK(initial.no_change);
    CHECK(activity && activity
                          ->run({{"operation", "resize"},
                                 {"id", id},
                                 {"rows", 30},
                                 {"cols", 100},
                                 {"wait_ms", 0}},
                                context)
                          .Ok());
    ToolResult input =
        activity ? activity->run({{"operation", "write"},
                                  {"id", id},
                                  {"chars", "hello\n"},
                                  {"wait_ms", 2000}},
                                 context)
                 : ToolFailure(ToolErrorCode::kInternal, "missing activity");
    CHECK(input.Ok());
    CHECK(input.output.find("got:hello") != std::string::npos);
    ToolResult completed =
        ToolActivityWait(pty_processes, {id}, "all", 2000, context);
    CHECK(completed.Ok());
    CHECK(completed.output.find("exit code 0") != std::string::npos);
  }

  // A persisted PID must never authorize signalling a different process that
  // later acquired the same numeric id. Corrupt the recorded start identity,
  // drop in-memory ownership, and prove stop cleans metadata without killing
  // the still-live process group.
  CHECK(!ProcessIdentity(getpid()).empty());
  CHECK(ProcessIdentity(pid_t{99999999}).empty());
  TestWorkspace detached_workspace("detached-identity");
  ProcessSupervisor detached_launcher;
  ShellCommandResult detached = RunShellCommand(
      detached_launcher, context,
      {.command = "sleep 20", .detach = true, .immediate = true});
  CHECK(detached.result.Ok());
  std::vector<BgJob> detached_jobs = detached_launcher.Snapshot();
  CHECK(detached_jobs.size() == 1);
  if (!detached_jobs.empty()) {
    pid_t pid = detached_jobs.front().pid;
    std::optional<BgJob> released = detached_launcher.Take(pid);
    CHECK(released.has_value());
    // The log lives under the records directory, not in it: Phase C makes the
    // log subdirectory writable by sandboxed commands, and a record beside it
    // would be forgeable.
    CHECK(std::filesystem::path(detached_jobs.front().log).parent_path() ==
          std::filesystem::path(UagentDir(kTerminalLogsDir)));
    std::string record_path = DetachedRecordPath(pid);
    CHECK(std::filesystem::path(record_path).parent_path() ==
          std::filesystem::path(UagentDir(kTerminalsDir)));
    std::ifstream input(record_path);
    json record = json::parse(input, nullptr, false);
    CHECK(record.is_object());
    record["process_identity"] = "not-the-live-process";
    input.close();
    std::ofstream output(record_path, std::ios::trunc);
    output << JsonDump(record, 2) << '\n';
    output.close();

    ProcessSupervisor detached_reader;
    ToolResult poll = ToolActivityOutput(detached_reader, pid);
    CHECK(poll.Ok());
    CHECK(poll.activity_terminal);
    CHECK(poll.output.starts_with("[exited · activity "));
    ToolResult stopped = ToolActivityStop(detached_reader, pid);
    CHECK(stopped.Ok());
    CHECK(stopped.output.find("already exited") != std::string::npos);
    CHECK(ProcessGroupAlive(pid));
    CHECK(!std::filesystem::exists(record_path));
    KillProcess(pid);
  }
}

void TestActivityDescriptorAndInputPolicy() {
  ToolContext context{std::chrono::steady_clock::now() +
                      std::chrono::seconds(10)};

  // Descriptor ownership is a type, but only a count proves it. A PTY run and
  // a pipe run each take a log, an output end and (for a PTY) an input end;
  // if any of those outlive their activity, this grows per iteration.
  {
    ProcessSupervisor leak_check;
    auto open_descriptors = [] {
      size_t count = 0;
      std::error_code ec;
      for (const auto& entry :
           std::filesystem::directory_iterator("/dev/fd", ec)) {
        (void)entry;
        ++count;
      }
      return count;
    };
    // One warm-up pair: the supervisor's I/O thread and wake pipe are created
    // on first use and are not per-activity.
    for (bool tty : {false, true}) {
      (void)RunShellCommand(leak_check, context,
                            {.command = "printf x", .tty = tty});
    }
    const size_t settled = open_descriptors();
    for (int index = 0; index < 6; ++index) {
      for (bool tty : {false, true}) {
        CHECK(RunShellCommand(leak_check, context,
                              {.command = "printf x", .tty = tty})
                  .result.Ok());
      }
    }
    // Retained activities keep their transcript, never their descriptors.
    // The supervisor closes them as it reaps, which an instrumented build can
    // lag a few milliseconds behind, so the count is given time to come back
    // down. A real leak never does, and still fails here.
    size_t open_now = open_descriptors();
    for (int attempt = 0; attempt < 500 && open_now > settled + 2; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      open_now = open_descriptors();
    }
    CHECK(open_now <= settled + 2);
  }

  ProcessSupervisor non_tty;
  CHECK(RunShellCommand(
            non_tty, context,
            {.command = "sleep 10", .background = true, .immediate = true})
            .result.Ok());
  std::vector<BgJob> pipe_jobs = non_tty.Snapshot();
  CHECK(pipe_jobs.size() == 1);
  if (!pipe_jobs.empty()) {
    int64_t id = ActivityId(pipe_jobs[0]);
    ToolResult rejected = ToolActivityInput(non_tty, id, "hello\n", 0, context);
    CHECK(!rejected.Ok());
    CHECK(rejected.error == ToolErrorCode::kUnavailable);
    CHECK(rejected.output.find("does not accept input") != std::string::npos);
    CHECK(rejected.output.find("tty=true") != std::string::npos);
    ToolResult no_pty = ToolActivityInput(non_tty, id, "", 0, context, 30, 100);
    CHECK(!no_pty.Ok());
    CHECK(no_pty.error == ToolErrorCode::kUnavailable);
    CHECK(no_pty.output.find("has no PTY to resize") != std::string::npos);
    ToolResult bad_dimensions =
        ToolActivityInput(non_tty, id, "", 0, context, 0, 100);
    CHECK(!bad_dimensions.Ok());
    CHECK(bad_dimensions.error == ToolErrorCode::kInvalidArguments);
    CHECK(bad_dimensions.output.find("1..1000") != std::string::npos);
    CHECK(ToolActivityInput(non_tty, id, "\x03", 0, context).Ok());
    (void)ToolActivityWait(non_tty, {id}, "all", 2000, context);
  }
}

void TestActivityWaitAndDelivery() {
  ToolContext context{std::chrono::steady_clock::now() +
                      std::chrono::seconds(10)};

  // A wait the turn deadline cuts short has to say so. Reporting only that it
  // timed out reads as though the requested wait elapsed, which invites the
  // caller to conclude the activity is stuck and stop waiting on it. The
  // per-call budget no longer truncates a wait at all — the tool declares
  // timeout_s 0 — so an explicit WithTimeout stands in for the turn's.
  ProcessSupervisor capped_wait;
  CHECK(RunShellCommand(
            capped_wait, context,
            {.command = "sleep 10", .background = true, .immediate = true})
            .result.Ok());
  std::vector<BgJob> capped_jobs = capped_wait.Snapshot();
  CHECK(capped_jobs.size() == 1);
  if (!capped_jobs.empty()) {
    int64_t id = ActivityId(capped_jobs[0]);
    ToolResult capped = ToolActivityWait(capped_wait, {id}, "all", 300000,
                                         context.WithTimeout(1));
    CHECK(capped.Ok());
    CHECK(capped.output.find("requested, capped by the turn deadline") !=
          std::string::npos);
    CHECK(capped.output.find("call again to keep waiting") !=
          std::string::npos);
    // Reaching wait_ms on its own is a different fact and must not blame the
    // deadline, or the caller learns to ignore the distinction.
    ToolResult plain = ToolActivityWait(capped_wait, {id}, "all", 300, context);
    CHECK(plain.Ok());
    CHECK(plain.output.find("[waited ") != std::string::npos);
    CHECK(plain.output.find("capped by") == std::string::npos);
  }

  ProcessSupervisor closed_input;
  auto closed_session = std::make_shared<ActivitySession>();
  closed_session->tty = true;
  CHECK(closed_input.TryAdd(
      {899998, "", "closed input", false, "", 0, closed_session}, 1));
  std::vector<BgJob> closed_jobs = closed_input.Snapshot();
  CHECK(closed_jobs.size() == 1);
  if (!closed_jobs.empty()) {
    int64_t id = ActivityId(closed_jobs[0]);
    ToolResult closed =
        ToolActivityInput(closed_input, id, "hello", 0, context);
    CHECK(!closed.Ok());
    CHECK(closed.error == ToolErrorCode::kUnavailable);
    CHECK(closed.output.find("input is closed") != std::string::npos);
    CHECK(closed_input.Take(id).has_value());
  }

  ProcessSupervisor invalid_pty;
  auto invalid_pty_session = std::make_shared<ActivitySession>();
  invalid_pty_session->tty = true;
  invalid_pty_session->input_fd.Reset(open("/dev/null", O_RDWR));
  CHECK(invalid_pty_session->input_fd.Valid());
  CHECK(invalid_pty.TryAdd(
      {899997, "", "invalid PTY", false, "", 0, invalid_pty_session}, 1));
  std::vector<BgJob> invalid_pty_jobs = invalid_pty.Snapshot();
  CHECK(invalid_pty_jobs.size() == 1);
  if (!invalid_pty_jobs.empty()) {
    int64_t id = ActivityId(invalid_pty_jobs[0]);
    ToolResult failed_resize =
        ToolActivityInput(invalid_pty, id, "", 0, context, 30, 100);
    CHECK(!failed_resize.Ok());
    CHECK(failed_resize.error == ToolErrorCode::kProcessFailed);
    CHECK(failed_resize.output.find("could not resize activity " +
                                    std::to_string(id)) != std::string::npos);
    CHECK(failed_resize.output.find(" PTY: ") != std::string::npos);
    CHECK(invalid_pty.Take(id).has_value());
  }

  ProcessSupervisor steering_wait;
  CHECK(RunShellCommand(
            steering_wait, context,
            {.command = "sleep 5", .background = true, .immediate = true})
            .result.Ok());
  std::vector<BgJob> steering_jobs = steering_wait.Snapshot();
  CHECK(steering_jobs.size() == 1);
  if (!steering_jobs.empty()) {
    std::thread interrupt([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      SteeringState().Request();
      steering_wait.Wake();
    });
    auto started_wait = std::chrono::steady_clock::now();
    ToolResult interrupted = ToolActivityWait(
        steering_wait, {ActivityId(steering_jobs[0])}, "all", 5000, context);
    auto wait_time = std::chrono::steady_clock::now() - started_wait;
    interrupt.join();
    CHECK(!interrupted.Ok());
    CHECK(interrupted.output.find("wait interrupted") != std::string::npos);
    CHECK(wait_time < std::chrono::milliseconds(500));
    CHECK(SteeringState().Take());
    CHECK(ToolActivityStop(steering_wait, ActivityId(steering_jobs[0])).Ok());
  }

  ProcessSupervisor steering_yield;
  CHECK(RunShellCommand(
            steering_yield, context,
            {.command = "sleep 5", .background = true, .immediate = true})
            .result.Ok());
  std::vector<BgJob> steering_yield_jobs = steering_yield.Snapshot();
  CHECK(steering_yield_jobs.size() == 1);
  if (!steering_yield_jobs.empty()) {
    int64_t id = ActivityId(steering_yield_jobs[0]);
    ToolResult yielded_wait;
    std::thread waiter([&] {
      yielded_wait =
          ToolActivityWait(steering_yield, {id}, "all", 5000, context);
    });
    SteeringState().Queue("change course");
    steering_yield.Wake();
    waiter.join();
    CHECK(yielded_wait.Ok());
    CHECK(yielded_wait.output.find("wait yielded for queued steering") !=
          std::string::npos);
    CHECK(steering_yield.IsLive(id));
    CHECK(ProcessGroupAlive(steering_yield_jobs[0].pid));
    std::vector<std::string> queued = SteeringState().TakeQueued();
    CHECK(queued.size() == 1 && queued[0] == "change course");

    ToolResult yielded_output;
    std::thread output_waiter([&] {
      yielded_output =
          ToolActivityOutput(steering_yield, id, 5000, {}, context);
    });
    SteeringState().Queue("inspect output");
    steering_yield.Wake();
    output_waiter.join();
    CHECK(yielded_output.Ok());
    CHECK(yielded_output.output.find("wait yielded for queued steering") !=
          std::string::npos);
    CHECK(steering_yield.IsLive(id));
    CHECK(ProcessGroupAlive(steering_yield_jobs[0].pid));
    queued = SteeringState().TakeQueued();
    CHECK(queued.size() == 1 && queued[0] == "inspect output");
    CHECK(ToolActivityStop(steering_yield, id).Ok());
  }

  ProcessSupervisor no_duplicate_completion;
  ShellCommandResult yielded_once =
      RunShellCommand(no_duplicate_completion, context,
                      {.command = "printf once; sleep 0.5", .yield_ms = 250});
  CHECK(yielded_once.result.output.find("once") != std::string::npos);
  std::vector<BgJob> once_jobs = no_duplicate_completion.Snapshot();
  CHECK(once_jobs.size() == 1);
  if (!once_jobs.empty()) {
    ToolResult final =
        ToolActivityWait(no_duplicate_completion, {ActivityId(once_jobs[0])},
                         "all", 2000, context);
    CHECK(final.Ok());
    CHECK(final.output.find("(no new output)") != std::string::npos);
    CHECK(final.output.find("\nonce") == std::string::npos);
  }

  {
    ProcessSupervisor bounded_completion;
    ScopedEnv scoped_result_cap("UAGENT_TOOL_RESULT_CHARS", "8000");
    CHECK(RunShellCommand(bounded_completion, context,
                          {.command = "printf '%7000s' x",
                           .background = true,
                           .immediate = true})
              .result.Ok());
    std::vector<BgJob> bounded_jobs = bounded_completion.Snapshot();
    CHECK(bounded_jobs.size() == 1);
    if (!bounded_jobs.empty()) {
      WaitForActivityDrain(bounded_completion, bounded_jobs[0]);
    }
    std::vector<std::string> bounded_notes =
        BgTakeCompleted(bounded_completion);
    CHECK(bounded_notes.size() == 1);
    CHECK(!bounded_notes.empty() && bounded_notes[0].size() < 6500);
    CHECK(!bounded_notes.empty() &&
          bounded_notes[0].find("bytes omitted") != std::string::npos);
    if (!bounded_jobs.empty()) {
      ToolResult replay =
          ToolActivityOutput(bounded_completion, ActivityId(bounded_jobs[0]), 0,
                             {}, context, 8000);
      CHECK(replay.output.find("complete transcript replay") !=
            std::string::npos);
    }
  }

  ProcessSupervisor incremental;
  CHECK(
      RunShellCommand(incremental, context,
                      {.command = "printf 'Server '; sleep 0.1; printf ready; "
                                  "sleep 0.1; printf done",
                       .background = true,
                       .immediate = true})
          .result.Ok());
  std::vector<BgJob> incremental_jobs = incremental.Snapshot();
  CHECK(incremental_jobs.size() == 1);
  if (!incremental_jobs.empty()) {
    int64_t id = ActivityId(incremental_jobs[0]);
    ToolResult marker =
        ToolActivityOutput(incremental, id, 2000, "Server ready", context);
    CHECK(marker.Ok());
    CHECK(marker.output.find("Server ready") != std::string::npos);
    ToolResult no_duplicate = ToolActivityOutput(incremental, id);
    CHECK(no_duplicate.output.find("Server ready") == std::string::npos);
    CHECK(no_duplicate.no_change);
    ToolResult final =
        ToolActivityWait(incremental, {id}, "all", 2000, context);
    CHECK(final.Ok());
    CHECK(final.output.find("done") != std::string::npos);
  }

  // The race is created by the foreground handoff below, not by the command's
  // own duration, so the command stays as short as the shell allows.
  for (int iteration = 0; iteration < 50; ++iteration) {
    ProcessSupervisor race;
    ShellCommandResult result;
    std::thread command([&] {
      result = RunShellCommand(race, context, {.command = "printf race"});
    });
    (void)race.WaitForForeground(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    (void)race.RequestForegroundBackground();
    command.join();
    CHECK(race.ForegroundCount() == 0);
    CHECK(result.result.Ok());
    for (const BgJob& job : race.Snapshot()) {
      (void)ToolActivityWait(race, {ActivityId(job)}, "all", 1000, context);
    }
  }

  ProcessSupervisor memory_activity;
  CHECK(RunShellCommand(memory_activity, context,
                        {.command = "printf memory-done",
                         .background = true,
                         .immediate = true,
                         .job_kind = "memory",
                         .activity_label = "extracting from source-123",
                         .receipt_path = "/tmp/receipt-123.json",
                         .source_id = "source-123"})
            .result.Ok());
  std::vector<BgJob> memory_jobs = memory_activity.Snapshot();
  CHECK(memory_jobs.size() == 1);
  CHECK(ToolActivityList(memory_activity).output.find("[memory] activity") !=
        std::string::npos);
  // A wait consumes the completion it observes, so extraction must stay out of
  // reach: the harness drains it into the memory audit instead. It is still
  // listed, just not waitable.
  CHECK(ToolActivityWait(memory_activity, {}, "any", 0, context)
            .output.find("no waitable activities") != std::string::npos);
  if (!memory_jobs.empty()) {
    ToolResult named = ToolActivityWait(
        memory_activity, {ActivityId(memory_jobs[0])}, "all", 0, context);
    CHECK(!named.Ok());
    CHECK(named.output.find("not waitable") != std::string::npos);
  }
  CHECK(ToolActivityList(memory_activity)
            .output.find("extracting from source-123") != std::string::npos);
  if (!memory_jobs.empty()) {
    WaitForActivityDrain(memory_activity, memory_jobs[0]);
    std::vector<BackgroundCompletion> completed =
        BgTakeCompletedDetails(memory_activity, "memory");
    CHECK(completed.size() == 1);
    if (!completed.empty()) {
      CHECK(completed[0].kind == ActivityKind::kMemory);
      CHECK(completed[0].display_label == "extracting from source-123");
      CHECK(completed[0].receipt_path == "/tmp/receipt-123.json");
      CHECK(completed[0].source_id == "source-123");
      CHECK(completed[0].output.find("memory-done") != std::string::npos);
    }
  }

  ProcessSupervisor delivery_race;
  CHECK(RunShellCommand(delivery_race, context,
                        {.command = "sleep 0.05; printf \"$RACE_VALUE\"",
                         .background = true,
                         .immediate = true,
                         .environment = {{"RACE_VALUE", "delivery-token"}}})
            .result.Ok());
  std::vector<BgJob> delivery_jobs = delivery_race.Snapshot();
  CHECK(delivery_jobs.size() == 1);
  if (!delivery_jobs.empty()) {
    int64_t id = ActivityId(delivery_jobs[0]);
    CHECK(WaitForActivityDrain(delivery_race, delivery_jobs[0]));
    ToolResult explicit_wait;
    std::vector<std::string> automatic;
    std::atomic<bool> start{false};
    std::thread waiter([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      explicit_wait =
          ToolActivityWait(delivery_race, {id}, "all", 1000, context);
    });
    std::thread drainer([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      automatic = BgTakeCompleted(delivery_race);
    });
    start.store(true, std::memory_order_release);
    waiter.join();
    drainer.join();
    std::string delivered = explicit_wait.output;
    for (const std::string& note : automatic) delivered += note;
    size_t first = delivered.find("delivery-token");
    CHECK(first != std::string::npos);
    CHECK(delivered.find("delivery-token", first + 1) == std::string::npos);
  }

  ProcessSupervisor parallel_handoff;
  std::array<ShellCommandResult, 3> parallel_results;
  std::array<std::thread, 3> parallel_threads;
  for (size_t index = 0; index < parallel_threads.size(); ++index) {
    parallel_threads[index] = std::thread([&, index] {
      parallel_results[index] = RunShellCommand(
          parallel_handoff, context,
          {.command = "sleep 5; printf parallel-" + std::to_string(index)});
    });
  }
  CHECK(parallel_handoff.WaitForForeground(
      3, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  CHECK(parallel_handoff.RequestForegroundBackground());
  for (std::thread& thread : parallel_threads) thread.join();
  CHECK(parallel_handoff.ForegroundCount() == 0);
  CHECK(parallel_handoff.PendingCount() == 3);
  for (const BgJob& job : parallel_handoff.Snapshot()) {
    CHECK(ToolActivityStop(parallel_handoff, ActivityId(job)).Ok());
  }

  ProcessSupervisor handoff;
  ShellCommandResult handoff_result;
  std::thread foreground([&] {
    handoff_result = RunShellCommand(handoff, context, {.command = "sleep 10"});
  });
  CHECK(handoff.WaitForForeground(
      1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  CHECK(handoff.RequestForegroundBackground());
  foreground.join();
  CHECK(handoff_result.result.Ok());
  CHECK(handoff_result.result.output.find("moved to background") !=
        std::string::npos);
  std::vector<BgJob> handed_off = handoff.Snapshot();
  CHECK(handed_off.size() == 1);
  if (!handed_off.empty()) {
    CHECK(ProcessGroupAlive(handed_off[0].pid));
    CHECK(ToolActivityStop(handoff, ActivityId(handed_off[0])).Ok());
  }
}

void TestDetachedActivityOwnership() {
  ToolContext context{std::chrono::steady_clock::now() +
                      std::chrono::seconds(10)};
  TestWorkspace workspace("detached-ownership");

  // A detached leader may already have been reaped by another lifecycle
  // owner when automatic completion observes it. ECHILD plus a dead process
  // group is still a completed activity, not one that remains supervised
  // forever.
  ProcessSupervisor reaped_completion;
  ShellCommandResult detached = RunShellCommand(
      reaped_completion, context,
      {.command = "printf reaped-detached", .detach = true, .immediate = true});
  CHECK(detached.result.Ok());
  std::vector<BgJob> detached_jobs = reaped_completion.Snapshot();
  CHECK(detached_jobs.size() == 1);
  if (!detached_jobs.empty()) {
    const BgJob& job = detached_jobs.front();
    int status = 0;
    CHECK(WaitPid(job.pid, &status) == job.pid);
    for (int attempt = 0; attempt < 100 && ProcessGroupAlive(job.pid);
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!ProcessGroupAlive(job.pid));
    errno = 0;
    CHECK(WaitPid(job.pid, &status) < 0);
    CHECK(errno == ECHILD);

    std::vector<std::string> completed = BgTakeCompleted(reaped_completion);
    CHECK(completed.size() == 1);
    CHECK(!completed.empty() &&
          completed.front().find("reaped-detached") != std::string::npos);
    CHECK(!reaped_completion.Find(job.pid).has_value());
    CHECK(!std::filesystem::exists(DetachedRecordPath(job.pid)));
    CHECK(!std::filesystem::exists(job.log));
  }

  // Stopping an ordinary supervised job owns that job's log, but not a
  // detached record that happens to use the same process id. Such a record can
  // survive PID reuse and must only be unlinked for detached activities.
  ProcessSupervisor background_stop;
  CHECK(RunShellCommand(
            background_stop, context,
            {.command = "sleep 20", .background = true, .immediate = true})
            .result.Ok());
  std::vector<BgJob> background_jobs = background_stop.Snapshot();
  CHECK(background_jobs.size() == 1);
  if (!background_jobs.empty()) {
    const BgJob& job = background_jobs.front();
    std::string sentinel_log =
        (workspace.root / "detached-sentinel.log").string();
    {
      std::ofstream output(sentinel_log);
      output << "detached sentinel\n";
    }
    CHECK(SaveDetachedRecord(job.pid, sentinel_log, "detached sentinel").Ok());
    std::string record_path = DetachedRecordPath(job.pid);
    CHECK(std::filesystem::exists(record_path));

    ToolResult stopped = ToolActivityStop(background_stop, ActivityId(job));
    CHECK(stopped.Ok());
    CHECK(std::filesystem::exists(record_path));
    CHECK(std::filesystem::exists(sentinel_log));

    unlink(record_path.c_str());
    RemoveLog(sentinel_log);
  }

  // Subagents, ordinary commands and detached terminals share one pool, so an
  // unbounded fan-out of children would leave the parent unable to run the
  // build, test or search it needs to check their work. Children stop short of
  // the ceiling; the parent's own command still gets a slot.
  {
    ScopedEnv pool("UAGENT_MAX_BACKGROUND_JOBS", "4");

    // The parent's own busy jobs must never refuse delegation: a child
    // competes against other children, not against its parent.
    ProcessSupervisor parent_busy;
    CHECK(parent_busy.TryAdd({999801, "", "own-a", false, "command"}, 4));
    CHECK(parent_busy.TryAdd({999802, "", "own-b", false, "command"}, 4));
    ShellCommandResult admitted = RunShellCommand(
        parent_busy, context,
        {.command = "printf child-admitted", .job_kind = "subagent"});
    CHECK(admitted.result.Ok());
    CHECK(admitted.result.output.find("child-admitted") != std::string::npos);
    (void)parent_busy.TakeAllForShutdown();

    // Children stop short of the ceiling so the parent keeps slots of its own.
    ProcessSupervisor children_busy;
    CHECK(children_busy.TryAdd({999803, "", "child-a", false, "subagent"}, 4));
    CHECK(children_busy.TryAdd({999804, "", "child-b", false, "subagent"}, 4));
    ShellCommandResult refused = RunShellCommand(children_busy, context,
                                                 {.command = "echo third-child",
                                                  .immediate = true,
                                                  .job_kind = "subagent"});
    CHECK(!refused.result.Ok());
    CHECK(!refused.launched);
    CHECK(refused.result.output.find("at most 2 concurrent children") !=
          std::string::npos);

    ShellCommandResult own =
        RunShellCommand(children_busy, context, {.command = "printf own-work"});
    CHECK(own.result.Ok());
    CHECK(own.result.output.find("own-work") != std::string::npos);
    (void)children_busy.TakeAllForShutdown();

    // Detached terminals outlive the session, so the pool bounds them too.
    ProcessSupervisor detached_pool;
    for (pid_t pid = 999811; pid < 999815; ++pid) {
      CHECK(detached_pool.TryAdd({pid, "", "held", true, "command"}, 4));
    }
    CHECK(!detached_pool.TryAdd({999815, "", "overflow", true, "command"}, 4));
    ShellCommandResult refused_detach = RunShellCommand(
        detached_pool, context,
        {.command = "sleep 21", .detach = true, .immediate = true});
    CHECK(!refused_detach.result.Ok());
    CHECK(refused_detach.result.output.find("background job limit reached") !=
          std::string::npos);
    CHECK(!FindRunningDetachedActivity("sleep 21").has_value());
    // The record is written before the pool decides, so the refusal has to
    // take it back out: a stale record aims a later stop at whatever inherits
    // the reaped pid.
    CHECK(DetachedRecords().empty());
    (void)detached_pool.TakeAllForShutdown();
  }

  // A pool too small to hold the headroom still admits a child: delegation
  // stays possible, and the parent waits rather than the reverse.
  {
    ScopedEnv tiny("UAGENT_MAX_BACKGROUND_JOBS", "1");
    ProcessSupervisor single;
    ShellCommandResult child = RunShellCommand(
        single, context,
        {.command = "printf tiny-pool", .job_kind = "subagent"});
    CHECK(child.result.Ok());
    CHECK(child.result.output.find("tiny-pool") != std::string::npos);
    CHECK(single.TryAdd({999821, "", "holder", false, "subagent"}, 1));
    ShellCommandResult blocked =
        RunShellCommand(single, context, {.command = "printf blocked"});
    CHECK(!blocked.result.Ok());
    CHECK(blocked.result.output.find("background job limit reached (1)") !=
          std::string::npos);
    (void)single.TakeAllForShutdown();
  }
}

void TestCollaboratorMail() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("collaborator-mail");
  const fs::path dir = fs::path(UagentDir("collaborators"));

  // Order is the contract: guidance read out of sequence is guidance the
  // coordinator did not give.
  CHECK(WriteCollaboratorMail("agent-aaaa1111", "first").Ok());
  CHECK(WriteCollaboratorMail("agent-aaaa1111", "second").Ok());
  CHECK(WriteCollaboratorMail("agent-bbbb2222", "other").Ok());
  std::vector<std::string> taken = TakeCollaboratorMail("agent-aaaa1111");
  CHECK(taken == std::vector<std::string>({"first", "second"}));
  // Consumed on read, and only the addressee's: a second take returns nothing
  // while the other collaborator's message is still waiting.
  CHECK(TakeCollaboratorMail("agent-aaaa1111").empty());
  CHECK(TakeCollaboratorMail("agent-bbbb2222") ==
        std::vector<std::string>({"other"}));

  // Unreadable mail is dropped rather than retried: left in place it would be
  // reread on every step for as long as the record survives.
  const fs::path corrupt =
      dir / "agent-cccc3333.mail-19700101T000000Z-1-0000.json";
  std::ofstream(corrupt) << "{not json";
  CHECK(TakeCollaboratorMail("agent-cccc3333").empty());
  CHECK(!fs::exists(corrupt));

  // An id that could not name a file is answered with silence, not a path
  // assembled out of it.
  CHECK(TakeCollaboratorMail("../escape").empty());

  // Mail prunes with the record it belongs to, and while it is unread it is
  // what keeps that record from looking stale.
  ScopedEnv days("UAGENT_DEBUG_DAYS", "1");
  const fs::path record = dir / "agent-dddd4444.json";
  std::ofstream(record) << "{}\n";
  const auto stale = fs::file_time_type::clock::now() - std::chrono::hours(72);
  fs::last_write_time(record, stale);
  const fs::path forgotten = dir / "agent-eeee5555.json";
  std::ofstream(forgotten) << "{}\n";
  fs::last_write_time(forgotten, stale);
  CHECK(WriteCollaboratorMail("agent-dddd4444", "still waiting").Ok());
  MaintainArtifacts();
  CHECK(fs::exists(record));
  CHECK(TakeCollaboratorMail("agent-dddd4444") ==
        std::vector<std::string>({"still waiting"}));
  CHECK(!fs::exists(forgotten));
}

}  // namespace uagent
