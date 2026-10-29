// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/child_agent.h"
#include "include/agent/transcript.h"
#include "include/app/launch.h"
#include "include/app/panel.h"
#include "include/app/session.h"
#include "include/app/session_host.h"
#include "include/core/capture.h"
#include "include/core/config.h"
#include "include/core/defaults.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/mailbox.h"
#include "include/core/output_buffer.h"
#include "include/core/platform.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/tools/registry.h"
#include "include/tools/session.h"
#include "include/tools/shell.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {
// A background command's first output belongs to whichever call reads it
// first, and on a loaded host that is the call that started it. A command
// held here prints nothing until Open(), so what it prints is still to come
// when the start has returned.
class Gate {
 public:
  ~Gate() { unlink(path_.c_str()); }
  std::string Hold(const std::string& command) const {
    return "while [ ! -e '" + path_ + "' ]; do sleep 0.01; done; " + command;
  }
  void Open() const { std::ofstream{path_}; }

 private:
  std::string path_ =
      (std::filesystem::temp_directory_path() /
       ("uagent-gate-" + std::to_string(getpid()) + "-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())))
          .string();
};

// Blocks until the activity behind `job` has drained its output, or until the
// timeout expires; returns whether it drained.
inline bool WaitForActivityDrain(
    ActivitySupervisor& supervisor, const Activity& job,
    std::chrono::milliseconds timeout =
        std::chrono::milliseconds(BudgetMs(2000))) {
  if (!job.session) return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(job.session->mutex);
      if (job.session->state == ActivityState::kDrained) return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    const uint64_t generation = supervisor.Generation();
    supervisor.WaitForChange(generation, deadline);
  }
}

// The completion notes the model reads: each header over its output.
std::vector<std::string> CompletionNotes(ActivitySupervisor& supervisor) {
  std::vector<std::string> notes;
  for (const ActivityCompletion& completion :
       ActivityTakeCompletedDetails(supervisor)) {
    notes.push_back(ActivityCompletionText(completion, completion.output));
  }
  return notes;
}

}  // namespace

void TestSignalAndFileWatch() {
  RequestAbort();
  ClearAbort();
  pollfd stale_abort = {AbortWakeFd(), POLLIN, 0};
  CHECK(poll(&stale_abort, 1, 0) == 1);
  NormalizeAbortWake();
  stale_abort.revents = 0;
  CHECK(poll(&stale_abort, 1, 0) == 0);

  int terminal_wake[2] = {-1, -1};
  CHECK(pipe(terminal_wake) == 0);
  if (terminal_wake[0] >= 0) {
    SetTerminalWakeFd(terminal_wake[1]);
    InstallSigwinchHandler();
    raise(SIGWINCH);
    pollfd resize_event = {terminal_wake[0], POLLIN, 0};
    CHECK(poll(&resize_event, 1, 100) == 1);
    char wake_byte = 0;
    CHECK(read(terminal_wake[0], &wake_byte, 1) == 1);

    // A process-directed SIGINT can land on a worker, so the UI must receive
    // its own wake instead of relying on poll returning EINTR in that thread.
    SetQuitGesture(true);
    std::thread interrupted([] { SigintHandler(SIGINT); });
    resize_event.revents = 0;
    CHECK(poll(&resize_event, 1, 1000) == 1);
    interrupted.join();
    SetQuitGesture(false);
    CHECK(TakeIdleInterrupt());
    CHECK(!TakeIdleInterrupt());
    SetTerminalWakeFd(-1);
    close(terminal_wake[0]);
    close(terminal_wake[1]);
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

    // A write between processing state and opening the watcher must wake the
    // host too. Sampling only on entry would mistake these bytes for old data.
    observed = SnapshotFile(watched_path);
    CHECK(write(watched_fd, "queued", 6) == 6);
    CHECK(WaitForAnyFileChange(
              {watched_path},
              std::chrono::steady_clock::now() + std::chrono::seconds(2), -1,
              {{watched_path, observed}}) == FileWaitResult::kChanged);

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
    std::vector<Steering::Message> watch_steering =
        SteeringState().TakeMessages();
    CHECK(watch_steering.size() == 1 &&
          watch_steering[0].text == "watch steering");

    SteeringState().Queue("first");
    SteeringState().Queue("second");
    std::vector<Steering::Message> queued = SteeringState().TakeMessages();
    CHECK(queued.size() == 2 && queued[0].text == "first" &&
          queued[1].text == "second");
    CHECK(SteeringState().QueuedCount() == 0);

    // The next auto-start message is taken alone; the rest keep their order
    // and payloads, deferred ones included.
    SteeringState().Queue("deferred", "", false);
    SteeringState().Queue("auto", "first-id");
    SteeringState().Queue("later", "second-id", true, json::array({"a"}));
    std::optional<Steering::Message> next = SteeringState().TakeNextAutoStart();
    CHECK(next && next->text == "auto" && next->request_id == "first-id");
    std::vector<Steering::Message> rest = SteeringState().TakeMessages();
    CHECK(rest.size() == 2 && rest[0].text == "deferred" &&
          rest[1].text == "later" && rest[1].request_id == "second-id" &&
          rest[1].attachments.size() == 1);
    CHECK(!SteeringState().TakeNextAutoStart());

    // "Queue next" waits out the running turn: the turn's steps take only
    // the steer beside it and nothing yields to it; the turn's end starts it.
    SteeringState().Queue(
        {.text = "next", .request_id = "", .after_turn = true});
    SteeringState().Queue("steer");
    CHECK(SteeringState().SteerCount() == 1 && SteeringYieldRequested());
    std::vector<Steering::Message> now = SteeringState().TakeMessages();
    CHECK(now.size() == 1 && now[0].text == "steer");
    CHECK(SteeringState().QueuedCount() == 1 && !SteeringYieldRequested());
    next = SteeringState().TakeNextAutoStart();
    CHECK(next && next->text == "next");
    // Insert promotes the same payload, preserving identity, files and the
    // other after-turn entry. A stale click cannot interrupt another step.
    SteeringState().Queue(
        {.text = "held", .request_id = "held-id", .after_turn = true});
    SteeringState().Queue({.text = "insert",
                           .request_id = "insert-id",
                           .attachments = json::array({"file"}),
                           .after_turn = true});
    CHECK(!SteeringState().InsertNow("missing"));
    CHECK(!SteeringState().Requested());
    CHECK(SteeringState().InsertNow("insert-id"));
    CHECK(SteeringState().Requested());
    now = SteeringState().TakeMessages();
    CHECK(now.size() == 1 && now[0].request_id == "insert-id" &&
          now[0].attachments == json::array({"file"}) && !now[0].after_turn);
    CHECK(SteeringState().Take());
    CHECK(!SteeringState().InsertNow("insert-id"));
    CHECK(!SteeringState().Requested());
    CHECK(SteeringState().Recall("held-id"));
    close(watched_fd);
    unlink(watched_path);
  }

  // A catalogue change wakes the host so it can register a newly introduced
  // project path; the following external prompt edit is then notification
  // driven even though the file did not exist during the first wait.
  char project_root[] = "/tmp/uagent-watch-project-XXXXXX";
  REQUIRE(mkdtemp(project_root) != nullptr);
  int catalogue_wake[2] = {-1, -1};
  REQUIRE(pipe(catalogue_wake) == 0);
  const std::string prompt = std::string(project_root) + "/.uagent/PANEL.md";
  FileWaitResult catalogue_changed = FileWaitResult::kTimedOut;
  std::thread catalogue_watcher([&] {
    catalogue_changed = WaitForAnyFileChange(
        {prompt}, std::chrono::steady_clock::now() + std::chrono::seconds(2),
        catalogue_wake[0]);
  });
  CHECK(write(catalogue_wake[1], "x", 1) == 1);
  catalogue_watcher.join();
  CHECK(catalogue_changed == FileWaitResult::kInterrupted);
  char byte = 0;
  CHECK(read(catalogue_wake[0], &byte, 1) == 1);

  // The stamp taken before the watcher starts counts a write that beats it.
  const std::map<std::string, FileStamp> absent = {
      {prompt, SnapshotFile(prompt)}};
  FileWaitResult prompt_changed = FileWaitResult::kTimedOut;
  std::thread prompt_watcher([&] {
    prompt_changed =
        WaitForAnyFileChange({prompt},
                             std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(BudgetMs(2000)),
                             catalogue_wake[0], absent);
  });
  CreatePrivateDirectories(std::filesystem::path(prompt).parent_path());
  std::ofstream(prompt) << R"({"prompt":"changed externally"})";
  prompt_watcher.join();
  CHECK(prompt_changed == FileWaitResult::kChanged);

  std::vector<std::string> converging(512);
  for (size_t i = 0; i < converging.size(); ++i) {
    converging[i] = std::string(project_root) + "/missing/" +
                    std::to_string(i) + "/prompt.json";
  }
  FileWaitResult duplicate_changed = FileWaitResult::kTimedOut;
  std::thread duplicate_watcher([&] {
    duplicate_changed = WaitForAnyFileChange(
        converging, std::chrono::steady_clock::now() + std::chrono::seconds(2),
        catalogue_wake[0]);
  });
  CHECK(write(catalogue_wake[1], "x", 1) == 1);
  duplicate_watcher.join();
  CHECK(duplicate_changed == FileWaitResult::kInterrupted);
  CHECK(read(catalogue_wake[0], &byte, 1) == 1);

  char host_home[] = "/tmp/uagent-watch-host-XXXXXX";
  REQUIRE(mkdtemp(host_home) != nullptr);
  {
    ScopedEnv home("HOME", host_home);
    const std::string project = std::string(host_home) + "/project";
    const std::string web = std::string(host_home) + "/.uagent/web";
    const std::string parent =
        UagentDir(kHistoryDir) + "/" + WorkspaceId(project);
    CreatePrivateDirectories(web + "/drafts");
    CreatePrivateDirectories(parent);
    REQUIRE(PathExists(web + "/drafts"));
    REQUIRE(PathExists(parent));
    for (size_t i = 0; i < 300; ++i) {
      const std::string path = parent + "/web-" + std::to_string(i) + ".json";
      const std::string id = HashHex(path);
      std::ofstream(web + "/drafts/" + id + ".json")
          << JsonDump({{"id", id}, {"path", path}, {"cwd", project}});
    }
    session::SessionHost host("test", 4096, {}, web);
    host.LoadDrafts();
    const auto paths = host.PresencePaths();
    CHECK(paths.size() == 2);
    CHECK(std::count(paths.begin(), paths.end(), parent) == 1);
  }
  std::error_code host_remove_error;
  std::filesystem::remove_all(host_home, host_remove_error);
  close(catalogue_wake[0]);
  close(catalogue_wake[1]);
  std::error_code remove_error;
  std::filesystem::remove_all(project_root, remove_error);
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
  // Byte by byte past twice its tail, the buffer keeps and counts what one
  // trimmed on every push would: the tail is compacted only now and then.
  HeadTailBuffer amortised(10);
  HeadTailBuffer at_once(10);
  std::string pushed;
  for (char letter = 'a'; letter <= 'z'; ++letter) {
    amortised.Push(std::string(1, letter));
    pushed += letter;
    at_once = HeadTailBuffer(10);
    at_once.Push(pushed);
    CHECK(amortised.Snapshot() == at_once.Snapshot());
  }
  CHECK(amortised.Snapshot().find("16 bytes omitted") != std::string::npos);

  // A cut never splits a codepoint.
  HeadTailBuffer multibyte(9);
  multibyte.Push("ééééééééé");
  CHECK(multibyte.Snapshot().find("\xc3\n") == std::string::npos);
  CHECK(multibyte.Snapshot().find("\n\xa9") == std::string::npos);

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
  ActivitySupervisor delegating;
  auto delegated = std::make_shared<ActivitySession>();
  {
    std::lock_guard<std::mutex> lock(delegated->mutex);
    delegated->transcript.Push("· reading\n· writing\n");
  }
  CHECK(delegating.TryAdd({.pid = 899980,
                           .cmd = "child",
                           .kind = ActivityKind::kSubagent,
                           .session = delegated,
                           .display_label = "haiku",
                           .source_id = "agent-1a2b3c4d"},
                          4));
  CHECK(delegating.TryAdd({.pid = 899981, .cmd = "plain"}, 4));
  std::vector<SubagentView> views = delegating.SubagentViews();
  REQUIRE(views.size() == 1);
  CHECK(views[0].source_id == "agent-1a2b3c4d");
  CHECK(views[0].label == "haiku");
  CHECK(views[0].tail == "writing");
  CHECK(std::ranges::count(delegating.Snapshot(), ActivityKind::kSubagent,
                           &Activity::kind) == 1);
  CHECK(delegating.Count() == 2);
  // Looking is not draining: the tool that joins the child still needs every
  // byte the transcript holds.
  CHECK(delegating.SubagentViews()[0].tail == "writing");
  // The child's answer is not progress. Once it prints its JSON envelope the
  // newest line on the stream is the result the parent will deliver whole, so
  // the row says nothing rather than a fragment of it.
  {
    std::lock_guard<std::mutex> lock(delegated->mutex);
    delegated->transcript.Push("{\"format\":3,\"answer\":\"done\"}\n");
  }
  CHECK(delegating.SubagentViews()[0].tail.empty());
  CHECK(!delegating.ActivityViews()[0].contains("progress"));
  const int64_t delegated_id = views[0].id;
  const json first_inspection = delegating.InspectActivity(delegated_id);
  CHECK(first_inspection == delegating.InspectActivity(delegated_id));
  CHECK(first_inspection["command"] == "child");
  CHECK(JsonValue(first_inspection, "output", "").find("done") !=
        std::string::npos);
  {
    std::lock_guard lock(delegated->mutex);
    delegated->state = ActivityState::kExited;
    delegated->wait_status = 0;
    delegated->exited_at = std::chrono::steady_clock::now();
  }
  CHECK(delegating.ActivityViews()[0]["status"] == "finishing");

  // The status row counts what the turn waits on apart from what runs behind
  // it, backgrounded or detached.
  ActivitySupervisor counted;
  CHECK(counted.TryAdd({.pid = 899970, .cmd = "backgrounded"}, 8));
  CHECK(counted.TryAdd(
      {.pid = 899971, .cmd = "server", .kind = ActivityKind::kDetached}, 8));
  std::optional<ActivityReservation> waiting = counted.ReserveActivity(8);
  REQUIRE(waiting.has_value());
  REQUIRE(waiting->Register({.pid = 899972, .cmd = "waited on"}).has_value());
  const session::ActivityCounts counts =
      session::CountActivities({{"activities", counted.ActivityViews()}});
  CHECK(counts.foreground == 1);
  CHECK(counts.background == 2);

  ActivitySupervisor admission;
  std::optional<ActivityReservation> first_slot = admission.ReserveActivity(1);
  CHECK(first_slot.has_value());
  CHECK(!admission.ReserveActivity(1).has_value());
  CHECK(!admission.TryAdd({.pid = 899999, .cmd = "busy"}, 1));
  first_slot.reset();
  CHECK(admission.ReserveActivity(1).has_value());

  // The id is fixed at reservation, so a log can be named after it before the
  // job exists, and it is the id the job actually commits under.
  ActivitySupervisor reserved;
  std::optional<ActivityReservation> early = reserved.ReserveActivity(4);
  std::optional<ActivityReservation> later = reserved.ReserveActivity(4);
  REQUIRE(early.has_value());
  REQUIRE(later.has_value());
  CHECK(early->Id() > 0 && later->Id() > early->Id());
  const int64_t promised = early->Id();
  std::optional<int64_t> committed =
      early->Register({.pid = 899997, .cmd = "reserved", .id = promised});
  REQUIRE(committed.has_value());
  CHECK(*committed == promised);

  // The subagent flag survives the same move, so a released reservation gives
  // the child counter back; losing it would refuse delegation for the rest of
  // the run.
  ActivitySupervisor children;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::optional<ActivityReservation> child = children.ReserveActivity(4, 1);
    CHECK(child.has_value());
    CHECK(!children.ReserveActivity(4, 1).has_value());
  }

  ActivitySupervisor retained;
  std::vector<int64_t> retained_ids;
  for (int index = 0; index < 17; ++index) {
    auto session = std::make_shared<ActivitySession>();
    CHECK(retained.TryAdd({.pid = static_cast<pid_t>(900000 + index),
                           .cmd = "done",
                           .session = session},
                          32));
    int64_t id = ActivityId(retained.Snapshot().back());
    retained_ids.push_back(id);
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      CHECK(TransitionActivityLocked(*session, ActivityState::kExited));
      CHECK(TransitionActivityLocked(*session, ActivityState::kDrained));
    }
    std::optional<Activity> completed = retained.Take(id, /*retain=*/true);
    REQUIRE(completed.has_value());
    CHECK(retained.Find(id).has_value());
    CHECK(!retained.InspectActivity(id).contains("error"));
    CHECK(!retained.IsLive(id));
    CHECK(!retained.Take(id, /*retain=*/true).has_value());
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
  ActivitySupervisor supervisor;
  CHECK(supervisor.TryAdd({.pid = 899990, .cmd = "stopped", .session = stopped},
                          1));
  int64_t id = ActivityId(supervisor.Snapshot().front());
  std::optional<Activity> job = supervisor.Take(id, /*retain=*/true);
  REQUIRE(job.has_value());
  CHECK(supervisor.Find(id).has_value());
  CHECK(supervisor.ActivityViews()[0]["status"] == "stopped");
  CHECK(CompletionNotes(supervisor).empty());
  // Rows carry the full command (bounded) beside the 160-char label, so the
  // activity popup never degrades to the abbreviated label once inspect can
  // no longer reach a finished job.
  const std::string long_command(300, 'x');
  CHECK(supervisor.TryAdd(
      {.pid = 899991, .cmd = long_command, .session = stopped}, 4));
  json long_row;
  for (const json& row : supervisor.ActivityViews()) {
    if (row.value("command", "") == long_command) long_row = row;
  }
  CHECK(!long_row.is_null());
  CHECK(long_row.value("label", "").size() < long_command.size());
}

void TestActivitySessions() {
  // Preserve producer failures through output filters in both pipes and PTYs.
  ActivitySupervisor pipeline;
  for (bool tty : {false, true}) {
    ShellCommandResult failed = RunShellCommand(
        pipeline, {},
        {.command = "sh -c 'echo failed; exit 7' | tail -n 1", .tty = tty});
    CHECK(failed.result.error == ToolErrorCode::kProcessFailed);
    // The exit is a fact; the model still reads it, a person does not.
    CHECK(failed.result.facts.exit && failed.result.facts.exit->code == 7);
    CHECK(failed.result.hint.find("exit code 7") != std::string::npos);
    CHECK(failed.result.output.find("exit code") == std::string::npos);
    CHECK(failed.wait_status.has_value());
  }
  // An explicitly requested POSIX shell retains its own semantics.
  CHECK(RunShellCommand(pipeline, {},
                        {.command = "false | cat", .shell = "/bin/sh"})
            .result.Ok());
  CHECK(RunShellCommand(pipeline, {}, {.command = "printf 'FAIL (expected)'"})
            .result.Ok());
  CHECK(RunShellCommand(pipeline, {}, {.command = "false | cat; test $? -ne 0"})
            .result.Ok());
  ShellCommandResult background =
      RunShellCommand(pipeline, {},
                      {.command = "sleep 0.1; sh -c 'exit 7' | cat",
                       .background = true,
                       .immediate = true});
  CHECK(background.result.Ok());
  const auto pipeline_jobs = pipeline.Snapshot();
  CHECK(!pipeline_jobs.empty());
  if (!pipeline_jobs.empty()) {
    ToolResult completed =
        ToolActivityWait(pipeline, {ActivityId(pipeline_jobs.back())}, "all",
                         BudgetMs(2000), {});
    CHECK(ModelText(completed).find("exit code 7") != std::string::npos);
    CHECK(completed.facts.exit && completed.facts.exit->code == 7);
  }

  ToolContext context{std::chrono::steady_clock::now() +
                      std::chrono::seconds(10)};
  ActivitySupervisor automatic_yield;
  std::vector<Tool> yield_tools = BuiltinTools(automatic_yield);
  const Tool* public_run = FindTool(yield_tools, "run");
  CHECK(public_run != nullptr);
  if (public_run) {
    ToolResult yielded = public_run->run(
        {{"command", "sleep 60"}, {"yield_ms", kMinYieldMs}}, context);
    CHECK(yielded.Ok());
    CHECK(yielded.hint.starts_with(kRunningActivityMarker));
    CHECK(!yielded.output.starts_with(kRunningActivityMarker));
    for (const Activity& job : automatic_yield.Snapshot()) {
      CHECK_OK(ToolActivityStop(automatic_yield, ActivityId(job)));
    }
  }

  ActivitySupervisor pty_processes;
  ShellCommandResult started = RunShellCommand(
      pty_processes, context,
      {.command = "read value; "
                  "if [ -t 0 ]; then echo tty=yes; else echo tty=no; fi; "
                  "echo got:$value",
       .background = false,
       .tty = true,
       .yield_ms = 250});
  CHECK(started.result.Ok());
  std::vector<Activity> pty_jobs = pty_processes.Snapshot();
  CHECK(pty_jobs.size() == 1);
  if (!pty_jobs.empty()) {
    int64_t id = ActivityId(pty_jobs[0]);
    CHECK(id != pty_jobs[0].pid);
    std::vector<Tool> pty_tools = BuiltinTools(pty_processes);
    const Tool* activity = FindTool(pty_tools, "activity");
    CHECK(activity != nullptr);
    // The child cannot emit output until write releases its initial read.
    ToolResult initial =
        activity ? activity->run({{"operation", "poll"}, {"id", id}}, context)
                 : ToolFailure(ToolErrorCode::kInternal, "missing activity");
    CHECK(initial.output.find("(no new output)") != std::string::npos);
    CHECK(initial.facts.no_change);
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
                                  {"wait_ms", BudgetMs(2000)}},
                                 context)
                 : ToolFailure(ToolErrorCode::kInternal, "missing activity");
    CHECK(input.Ok());
    ToolResult completed =
        ToolActivityWait(pty_processes, {id}, "all", BudgetMs(2000), context);
    CHECK(completed.Ok());
    // write may return as soon as the PTY echoes input; the command's response
    // can arrive in the following wait result.
    CHECK((input.output + completed.output).find("tty=yes") !=
          std::string::npos);
    CHECK((input.output + completed.output).find("got:hello") !=
          std::string::npos);
    CHECK(ModelText(completed).find("exit code 0") != std::string::npos);
  }

  // A persisted PID must never authorize signalling a different process that
  // later acquired the same numeric id. Corrupt the recorded start identity,
  // drop in-memory ownership, and prove stop cleans metadata without killing
  // the still-live process group.
  CHECK(!ProcessIdentity(getpid()).empty());
  CHECK(ProcessIdentity(pid_t{99999999}).empty());
  TestWorkspace detached_workspace("detached-identity");
  ActivitySupervisor detached_launcher;
  ShellCommandResult detached = RunShellCommand(
      detached_launcher, context,
      {.command = "sleep 20", .detach = true, .immediate = true});
  CHECK(detached.result.Ok());
  std::vector<Activity> detached_jobs = detached_launcher.Snapshot();
  CHECK(detached_jobs.size() == 1);
  if (!detached_jobs.empty()) {
    pid_t pid = detached_jobs.front().pid;
    std::optional<Activity> released = detached_launcher.Take(pid);
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

    ActivitySupervisor detached_reader;
    ToolResult poll = ToolActivityOutput(detached_reader, pid);
    CHECK(poll.Ok());
    CHECK(poll.activity_terminal);
    CHECK(poll.hint.starts_with("[exited · activity "));
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
    ActivitySupervisor leak_check;
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
    CHECK(WaitUntil([&] { return open_descriptors() <= settled + 2; },
                    BudgetMs(5000)));
  }

  ActivitySupervisor non_tty;
  CHECK(RunShellCommand(
            non_tty, context,
            {.command = "sleep 10", .background = true, .immediate = true})
            .result.Ok());
  std::vector<Activity> pipe_jobs = non_tty.Snapshot();
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
  ActivitySupervisor capped_wait;
  CHECK(RunShellCommand(
            capped_wait, context,
            {.command = "sleep 10", .background = true, .immediate = true})
            .result.Ok());
  std::vector<Activity> capped_jobs = capped_wait.Snapshot();
  CHECK(capped_jobs.size() == 1);
  if (!capped_jobs.empty()) {
    int64_t id = ActivityId(capped_jobs[0]);
    ToolResult capped = ToolActivityWait(capped_wait, {id}, "all", 300000,
                                         context.WithTimeout(1));
    CHECK(capped.Ok());
    CHECK(capped.hint.find("requested, capped by the turn deadline") !=
          std::string::npos);
    CHECK(capped.hint.find("call again to keep waiting") != std::string::npos);
    CHECK(capped.facts.no_change);
    // Reaching wait_ms on its own is a different fact and must not blame the
    // deadline, or the caller learns to ignore the distinction.
    ToolResult plain = ToolActivityWait(capped_wait, {id}, "all", 300, context);
    CHECK(plain.Ok());
    CHECK(plain.hint.find("[waited ") != std::string::npos);
    CHECK(plain.hint.find("capped by") == std::string::npos);
  }

  ActivitySupervisor closed_input;
  auto closed_session = std::make_shared<ActivitySession>();
  closed_session->tty = true;
  CHECK(closed_input.TryAdd(
      {.pid = 899998, .cmd = "closed input", .session = closed_session}, 1));
  std::vector<Activity> closed_jobs = closed_input.Snapshot();
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

  ActivitySupervisor invalid_pty;
  auto invalid_pty_session = std::make_shared<ActivitySession>();
  invalid_pty_session->tty = true;
  invalid_pty_session->input_fd.Reset(open("/dev/null", O_RDWR));
  CHECK(invalid_pty_session->input_fd.Valid());
  CHECK(invalid_pty.TryAdd(
      {.pid = 899997, .cmd = "invalid PTY", .session = invalid_pty_session},
      1));
  std::vector<Activity> invalid_pty_jobs = invalid_pty.Snapshot();
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

  ActivitySupervisor steering_wait;
  CHECK(RunShellCommand(
            steering_wait, context,
            {.command = "sleep 60", .background = true, .immediate = true})
            .result.Ok());
  std::vector<Activity> steering_jobs = steering_wait.Snapshot();
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
    CHECK(interrupted.output.find("wait cancelled") != std::string::npos);
    CHECK(wait_time < std::chrono::milliseconds(500));
    CHECK(SteeringState().Take());
    CHECK_OK(ToolActivityStop(steering_wait, ActivityId(steering_jobs[0])));
  }

  ActivitySupervisor steering_yield;
  CHECK(RunShellCommand(
            steering_yield, context,
            {.command = "sleep 60", .background = true, .immediate = true})
            .result.Ok());
  std::vector<Activity> steering_yield_jobs = steering_yield.Snapshot();
  CHECK(steering_yield_jobs.size() == 1);
  if (!steering_yield_jobs.empty()) {
    int64_t id = ActivityId(steering_yield_jobs[0]);
    ToolResult yielded_output;
    std::thread output_waiter([&] {
      yielded_output =
          ToolActivityOutput(steering_yield, id, 5000, {}, context);
    });
    SteeringState().Queue("inspect output");
    steering_yield.Wake();
    output_waiter.join();
    CHECK(yielded_output.Ok());
    CHECK(yielded_output.hint.find("wait yielded for queued steering") !=
          std::string::npos);
    CHECK(steering_yield.IsLive(id));
    CHECK(ProcessGroupAlive(steering_yield_jobs[0].pid));
    std::vector<Steering::Message> queued = SteeringState().TakeMessages();
    CHECK(queued.size() == 1 && queued[0].text == "inspect output");
    CHECK_OK(ToolActivityStop(steering_yield, id));
  }

  ActivitySupervisor no_duplicate_completion;
  ShellCommandResult yielded_once =
      RunShellCommand(no_duplicate_completion, context,
                      {.command = "printf once; sleep 0.5", .yield_ms = 250});
  const bool yielded_once_output =
      yielded_once.result.output.find("once") != std::string::npos;
  std::vector<Activity> once_jobs = no_duplicate_completion.Snapshot();
  CHECK(once_jobs.size() == 1);
  if (!once_jobs.empty()) {
    ToolResult final =
        ToolActivityWait(no_duplicate_completion, {ActivityId(once_jobs[0])},
                         "all", BudgetMs(2000), context);
    CHECK(final.Ok());
    const bool final_once_output =
        final.output.find("once") != std::string::npos;
    // Scheduling may put the first read on either side of the yield deadline.
    // Whichever call receives the bytes owns them; completion must not replay
    // them or lose them.
    CHECK(yielded_once_output != final_once_output);
    if (yielded_once_output) {
      CHECK(final.output.find("(no new output)") != std::string::npos);
    }
  }

  {
    ActivitySupervisor bounded_completion;
    ScopedEnv scoped_result_cap("UAGENT_TOOL_RESULT_CHARS", "8000");
    Gate gate;
    CHECK(RunShellCommand(bounded_completion, context,
                          {.command = gate.Hold("printf '%7000s' x"),
                           .background = true,
                           .immediate = true})
              .result.Ok());
    gate.Open();
    std::vector<Activity> bounded_jobs = bounded_completion.Snapshot();
    CHECK(bounded_jobs.size() == 1);
    if (!bounded_jobs.empty()) {
      WaitForActivityDrain(bounded_completion, bounded_jobs[0]);
    }
    std::vector<std::string> bounded_notes =
        CompletionNotes(bounded_completion);
    CHECK(bounded_notes.size() == 1);
    CHECK(!bounded_notes.empty() && bounded_notes[0].size() < 6500);
    CHECK(!bounded_notes.empty() &&
          bounded_notes[0].find("bytes omitted") != std::string::npos);
    if (!bounded_jobs.empty()) {
      ToolResult replay =
          ToolActivityOutput(bounded_completion, ActivityId(bounded_jobs[0]), 0,
                             {}, context, 8000);
      CHECK(replay.hint.find("complete transcript replay") !=
            std::string::npos);
    }
  }

  ActivitySupervisor incremental;
  Gate incremental_gate;
  CHECK(RunShellCommand(
            incremental, context,
            {.command = incremental_gate.Hold(
                 "printf 'Server '; sleep 0.1; printf ready; sleep 0.1; "
                 "printf done"),
             .background = true,
             .immediate = true})
            .result.Ok());
  incremental_gate.Open();
  std::vector<Activity> incremental_jobs = incremental.Snapshot();
  CHECK(incremental_jobs.size() == 1);
  if (!incremental_jobs.empty()) {
    int64_t id = ActivityId(incremental_jobs[0]);
    ToolResult marker =
        ToolActivityOutput(incremental, id, 2000, "Server ready", context);
    CHECK(marker.Ok());
    CHECK(marker.output.find("Server ready") != std::string::npos);
    ToolResult no_duplicate = ToolActivityOutput(incremental, id);
    CHECK(no_duplicate.output.find("Server ready") == std::string::npos);
    CHECK(no_duplicate.facts.no_change);
    ToolResult final =
        ToolActivityWait(incremental, {id}, "all", BudgetMs(2000), context);
    CHECK(final.Ok());
    CHECK(final.output.find("done") != std::string::npos);
  }

  // The race is created by the foreground handoff below, not by the command's
  // own duration, so the command stays as short as the shell allows.
  for (int iteration = 0; iteration < 50; ++iteration) {
    ActivitySupervisor race;
    ShellCommandResult result;
    std::thread command([&] {
      result = RunShellCommand(race, context, {.command = "printf race"});
    });
    (void)race.WaitForForeground(
        1, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    (void)race.RequestForegroundBackground();
    command.join();
    CHECK(!race.WaitForForeground(1, std::chrono::steady_clock::now()));
    CHECK(result.result.Ok());
    for (const Activity& job : race.Snapshot()) {
      (void)ToolActivityWait(race, {ActivityId(job)}, "all", 1000, context);
    }
  }

  ActivitySupervisor memory_activity;
  Gate memory_gate;
  CHECK(RunShellCommand(memory_activity, context,
                        {.command = memory_gate.Hold("printf memory-done"),
                         .background = true,
                         .immediate = true,
                         .activity_kind = ActivityKind::kMemory,
                         .activity_label = "extracting from source-123",
                         .receipt_path = "/tmp/receipt-123.json",
                         .source_id = "source-123"})
            .result.Ok());
  memory_gate.Open();
  std::vector<Activity> memory_jobs = memory_activity.Snapshot();
  CHECK(memory_jobs.size() == 1);
  CHECK(
      ToolActivityOutput(memory_activity, 0).output.find("[memory] activity") !=
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
  CHECK(ToolActivityOutput(memory_activity, 0)
            .output.find("extracting from source-123") != std::string::npos);
  if (!memory_jobs.empty()) {
    WaitForActivityDrain(memory_activity, memory_jobs[0]);
    std::vector<ActivityCompletion> completed =
        ActivityTakeCompletedDetails(memory_activity, "memory");
    CHECK(completed.size() == 1);
    if (!completed.empty()) {
      CHECK(completed[0].kind == ActivityKind::kMemory);
      CHECK(completed[0].display_label == "extracting from source-123");
      CHECK(completed[0].receipt_path == "/tmp/receipt-123.json");
      CHECK(completed[0].source_id == "source-123");
      CHECK(completed[0].output.find("memory-done") != std::string::npos);
    }
  }

  ActivitySupervisor delivery_race;
  Gate delivery_gate;
  CHECK(
      RunShellCommand(delivery_race, context,
                      {.command = delivery_gate.Hold("printf \"$RACE_VALUE\""),
                       .background = true,
                       .immediate = true,
                       .environment = {{"RACE_VALUE", "delivery-token"}}})
          .result.Ok());
  delivery_gate.Open();
  std::vector<Activity> delivery_jobs = delivery_race.Snapshot();
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
      automatic = CompletionNotes(delivery_race);
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

  ActivitySupervisor parallel_handoff;
  std::array<ShellCommandResult, 3> parallel_results;
  std::array<std::thread, 3> parallel_threads;
  for (size_t index = 0; index < parallel_threads.size(); ++index) {
    parallel_threads[index] = std::thread([&, index] {
      parallel_results[index] = RunShellCommand(
          parallel_handoff, context,
          // Long enough to still be running when it is stopped below.
          {.command = "sleep 60; printf parallel-" + std::to_string(index)});
    });
  }
  CHECK(parallel_handoff.WaitForForeground(
      3, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  CHECK(parallel_handoff.RequestForegroundBackground());
  for (std::thread& thread : parallel_threads) thread.join();
  CHECK(
      !parallel_handoff.WaitForForeground(1, std::chrono::steady_clock::now()));
  CHECK(parallel_handoff.PendingCount() == 3);
  for (const Activity& job : parallel_handoff.Snapshot()) {
    CHECK_OK(ToolActivityStop(parallel_handoff, ActivityId(job)));
  }

  ActivitySupervisor handoff;
  ShellCommandResult handoff_result;
  std::thread foreground([&] {
    handoff_result = RunShellCommand(handoff, context, {.command = "sleep 10"});
  });
  CHECK(handoff.WaitForForeground(
      1, std::chrono::steady_clock::now() + std::chrono::seconds(2)));
  CHECK(handoff.RequestForegroundBackground());
  foreground.join();
  CHECK(handoff_result.result.Ok());
  CHECK(handoff_result.result.hint.find("moved to background") !=
        std::string::npos);
  std::vector<Activity> handed_off = handoff.Snapshot();
  CHECK(handed_off.size() == 1);
  if (!handed_off.empty()) {
    CHECK(ProcessGroupAlive(handed_off[0].pid));
    CHECK_OK(ToolActivityStop(handoff, ActivityId(handed_off[0])));
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
  ActivitySupervisor reaped_completion;
  ShellCommandResult detached = RunShellCommand(
      reaped_completion, context,
      {.command = "printf reaped-detached", .detach = true, .immediate = true});
  CHECK(detached.result.Ok());
  std::vector<Activity> detached_jobs = reaped_completion.Snapshot();
  CHECK(detached_jobs.size() == 1);
  if (!detached_jobs.empty()) {
    const Activity& job = detached_jobs.front();
    int status = 0;
    CHECK(WaitPid(job.pid, &status) == job.pid);
    CHECK(
        WaitUntil([&] { return !ProcessGroupAlive(job.pid); }, BudgetMs(1000)));
    errno = 0;
    CHECK(WaitPid(job.pid, &status) < 0);
    CHECK(errno == ECHILD);

    std::vector<std::string> completed = CompletionNotes(reaped_completion);
    CHECK(completed.size() == 1);
    CHECK(!completed.empty() &&
          completed.front().find("reaped-detached") != std::string::npos);
    CHECK(!reaped_completion.Find(job.pid).has_value());
    CHECK(!std::filesystem::exists(DetachedRecordPath(job.pid)));
    CHECK(!std::filesystem::exists(job.log));
  }

  // A detached command gives out its id, and a wait that names it waits for
  // it; a wait for everything does not, since it may never end.
  ActivitySupervisor detached_wait;
  Gate detached_gate;
  CHECK(RunShellCommand(detached_wait, context,
                        {.command = detached_gate.Hold("printf waited-for"),
                         .detach = true,
                         .immediate = true})
            .result.Ok());
  std::vector<Activity> waited_jobs = detached_wait.Snapshot();
  CHECK(waited_jobs.size() == 1);
  if (!waited_jobs.empty()) {
    CHECK(ToolActivityWait(detached_wait, {}, "all", 0, context)
              .output.find("no waitable activities") != std::string::npos);
    detached_gate.Open();
    ToolResult waited =
        ToolActivityWait(detached_wait, {ActivityId(waited_jobs[0])}, "all",
                         BudgetMs(5000), context);
    CHECK_OK(waited);
    CHECK(ModelText(waited).find("waited-for") != std::string::npos);
  }

  // A detached command that fails while something it started lives on is
  // reported as failed when the last of it is gone, not as the success a
  // process with no child left to ask would pass for.
  ActivitySupervisor detached_failure;
  // Its output is piped to this executable as the log pump, and the pipe's
  // last failure is the command's: a stand-in that succeeds lets the
  // command's own status through. A copy: SetExecutablePath replaces the
  // string ExecutablePath refers to.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  const std::string executable = ExecutablePath();
  SetExecutablePath("/usr/bin/true");
  CHECK(
      RunShellCommand(
          detached_failure, context,
          {.command = "sleep 0.5 & exit 7", .detach = true, .immediate = true})
          .result.Ok());
  SetExecutablePath(executable);
  std::vector<Activity> failed_jobs = detached_failure.Snapshot();
  CHECK(failed_jobs.size() == 1);
  if (!failed_jobs.empty()) {
    ToolResult failed =
        ToolActivityWait(detached_failure, {ActivityId(failed_jobs[0])}, "all",
                         BudgetMs(5000), context);
    CHECK(ModelText(failed).find("exit code 7") != std::string::npos);
  }

  // Stopping an ordinary supervised job owns that job's log, but not a
  // detached record that happens to use the same process id. Such a record can
  // survive PID reuse and must only be unlinked for detached activities.
  ActivitySupervisor background_stop;
  CHECK(RunShellCommand(
            background_stop, context,
            {.command = "sleep 20", .background = true, .immediate = true})
            .result.Ok());
  std::vector<Activity> background_activities = background_stop.Snapshot();
  CHECK(background_activities.size() == 1);
  if (!background_activities.empty()) {
    const Activity& job = background_activities.front();
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
    // The parent's own busy jobs must never refuse delegation: a child
    // competes against other children, not against its parent.
    ActivitySupervisor parent_busy;
    CHECK(parent_busy.TryAdd({.pid = 999801, .cmd = "own-a"}, MaxActivities()));
    CHECK(parent_busy.TryAdd({.pid = 999802, .cmd = "own-b"}, MaxActivities()));
    ShellCommandResult admitted =
        RunShellCommand(parent_busy, context,
                        {.command = "printf child-admitted",
                         .activity_kind = ActivityKind::kSubagent});
    CHECK(admitted.result.Ok());
    CHECK(admitted.result.output.find("child-admitted") != std::string::npos);
    (void)parent_busy.TakeAllForShutdown();

    // Children stop short of the ceiling so the parent keeps slots of its own.
    ActivitySupervisor children_busy;
    for (pid_t pid = 999803; pid < 999803 + MaxDelegatedActivities(); ++pid) {
      CHECK(children_busy.TryAdd(
          {.pid = pid, .cmd = "child", .kind = ActivityKind::kSubagent},
          MaxActivities()));
    }
    ShellCommandResult refused =
        RunShellCommand(children_busy, context,
                        {.command = "echo one-child-too-many",
                         .immediate = true,
                         .activity_kind = ActivityKind::kSubagent});
    CHECK(!refused.result.Ok());
    CHECK(!refused.launched);
    CHECK(refused.result.output.find("at most 6 concurrent children") !=
          std::string::npos);

    ShellCommandResult own =
        RunShellCommand(children_busy, context, {.command = "printf own-work"});
    CHECK(own.result.Ok());
    CHECK(own.result.output.find("own-work") != std::string::npos);
    (void)children_busy.TakeAllForShutdown();

    // Detached terminals outlive the session, so the pool bounds them too.
    ActivitySupervisor detached_pool;
    for (pid_t pid = 999811; pid < 999811 + MaxActivities(); ++pid) {
      CHECK(detached_pool.TryAdd(
          {.pid = pid, .cmd = "held", .kind = ActivityKind::kDetached},
          MaxActivities()));
    }
    CHECK(!detached_pool.TryAdd(
        {.pid = 999831, .cmd = "overflow", .kind = ActivityKind::kDetached},
        MaxActivities()));
    ShellCommandResult refused_detach = RunShellCommand(
        detached_pool, context,
        {.command = "sleep 21", .detach = true, .immediate = true});
    CHECK(!refused_detach.result.Ok());
    CHECK(refused_detach.result.output.find("activity limit reached (8)") !=
          std::string::npos);
    CHECK(!FindRunningDetachedActivity("sleep 21").has_value());
    // The record is written before the pool decides, so the refusal has to
    // take it back out: a stale record aims a later stop at whatever inherits
    // the reaped pid.
    CHECK(DetachedRecords().empty());
    (void)detached_pool.TakeAllForShutdown();

    // activities.maxConcurrent is read at every spawn: a lowered cap refuses
    // the next command, and children keep their headroom below it.
    ScopedEnv cap("UAGENT_MAX_ACTIVITIES", "3");
    CHECK(MaxActivities() == 3);
    CHECK(MaxDelegatedActivities() == 1);
    ActivitySupervisor lowered;
    for (pid_t pid = 999841; pid < 999844; ++pid) {
      CHECK(lowered.TryAdd({.pid = pid, .cmd = "held"}, MaxActivities()));
    }
    ShellCommandResult over_cap = RunShellCommand(
        lowered, context, {.command = "echo over-cap", .immediate = true});
    CHECK(!over_cap.result.Ok());
    CHECK(!over_cap.launched);
    CHECK(over_cap.result.output.find("activity limit reached (3)") !=
          std::string::npos);
    (void)lowered.TakeAllForShutdown();

    ActivitySupervisor one_child;
    CHECK(one_child.TryAdd(
        {.pid = 999851, .cmd = "child", .kind = ActivityKind::kSubagent},
        MaxActivities()));
    ShellCommandResult second_child =
        RunShellCommand(one_child, context,
                        {.command = "echo second-child",
                         .immediate = true,
                         .activity_kind = ActivityKind::kSubagent});
    CHECK(!second_child.result.Ok());
    CHECK(second_child.result.output.find(
              "at most 1 concurrent children of 3") != std::string::npos);
    (void)one_child.TakeAllForShutdown();
  }
}

// A delegated child is an ordinary session file whose header names its role.
// It must never surface as a session of its own, and its parent must find it.
// A killed command's grandchildren become zombies of whoever adopts them, and
// an adopter that reaps late (a container's init, a CI agent) must not make
// the stop of that command fail: nothing in such a group can run any more.
void TestAGroupOfOnlyZombiesIsFinished() {
  // A process name may hold parentheses and newlines; the fields follow the
  // last ')'. A record that does not fit says nothing.
  std::optional<ProcStat> plain = ParseProcStat("12 (sleep) Z 1 34 34 0\n");
  CHECK(plain && plain->state == 'Z' && plain->group == 34);
  std::optional<ProcStat> odd =
      ParseProcStat("12 (a) S 9 9\n(b) c) R 1 77 77 0\n");
  CHECK(odd && odd->state == 'R' && odd->group == 77);
  CHECK(!ParseProcStat(""));
  CHECK(!ParseProcStat("12 sleep Z 1 34"));
  CHECK(!ParseProcStat("12 (sleep) Z 1"));
  CHECK(!ParseProcStat("12 (sleep) Z x y"));
  CHECK(!ParseProcStat("12 (sleep) Z 1 -3"));
  // Kernel threads belong to group 0.
  std::optional<ProcStat> kernel = ParseProcStat("2 (kthreadd) S 0 0 0 0");
  CHECK(kernel && kernel->group == 0);

  // Only a /proc that lists everything may be believed about a group.
  auto mount = [](const std::string& options) {
    return "20 1 0:5 / /sys rw - sysfs sysfs rw\n"
           "21 1 0:20 / /proc rw,nosuid shared:5 - proc proc " +
           options + "\n22 1 0:21 / /dev rw - tmpfs tmpfs rw\n";
  };
  CHECK(ProcMountShowsEveryProcess(mount("rw")));
  CHECK(ProcMountShowsEveryProcess(mount("rw,hidepid=0,gid=5")));
  CHECK(ProcMountShowsEveryProcess(mount("rw,hidepid=off")));
  for (const char* hiding :
       {"rw,hidepid=1", "rw,hidepid=2", "rw,hidepid=4", "rw,hidepid=invisible",
        "rw,hidepid=noaccess", "rw,hidepid=ptraceable", "rw,subset=pid"}) {
    CHECK(!ProcMountShowsEveryProcess(mount(hiding)));
  }
  CHECK(!ProcMountShowsEveryProcess(""));
  CHECK(!ProcMountShowsEveryProcess("20 1 0:5 / /sys rw - sysfs sysfs rw\n"));
  // The mount in effect is the last one.
  CHECK(!ProcMountShowsEveryProcess(
      mount("rw") + "30 1 0:30 / /proc rw - proc proc rw,hidepid=2\n"));
  CHECK(ProcMountShowsEveryProcess(mount("rw,hidepid=2") +
                                   "30 1 0:30 / /proc rw - proc proc rw\n"));

  pid_t leader = fork();
  if (leader == 0) {
    pause();
    _exit(0);
  }
  CHECK(leader > 0);
  CHECK(setpgid(leader, leader) == 0);
  // This process is the zombie's parent and never waits for it, as a slow
  // adopter would not.
  pid_t zombie = fork();
  if (zombie == 0) _exit(setpgid(0, leader) == 0 ? 0 : 1);
  CHECK(zombie > 0);
  siginfo_t info{};
  CHECK(waitid(P_PID, static_cast<id_t>(zombie), &info, WEXITED | WNOWAIT) ==
        0);
  CHECK(info.si_status == 0);
  CHECK(ProcessGroupAlive(leader));
  CHECK(kill(leader, SIGKILL) == 0);
  int status = 0;
  CHECK(WaitPid(leader, &status) == leader);
  CHECK(!ProcessGroupAlive(leader));
  CHECK(WaitPid(zombie, &status) == zombie);
}

void TestThreadWorktreesGoOnlyWhenNothingIsLost() {
  TestWorkspace workspace("thread-worktree");
  const std::string project = CanonicalCwd();
  auto git = [](const std::string& dir, std::vector<std::string> args) {
    args.insert(args.begin(), {"git", "-C", dir, "-c", "user.email=t@t", "-c",
                               "user.name=t"});
    return CaptureProcess(args, 30).Ok();
  };
  REQUIRE(git(project, {"init", "-q"}));
  REQUIRE(git(project, {"commit", "-q", "--allow-empty", "-m", "base"}));
  const std::string tree = PlanLaunch(project, true, "thread-", "t1").cwd;
  CHECK(LaunchWorktree(tree) && !LaunchWorktree(project));
  REQUIRE(CreateWorktree(project, tree).empty());
  {
    std::ofstream(tree + "/work.txt") << "x";
  }
  CHECK(RemoveWorktree(project, tree).find("uncommitted") != std::string::npos);
  REQUIRE(git(tree, {"add", "work.txt"}));
  REQUIRE(git(tree, {"commit", "-q", "-m", "work"}));
  CHECK(RemoveWorktree(project, tree).find("no branch") != std::string::npos);
  REQUIRE(git(tree, {"branch", "keep"}));
  CHECK(RemoveWorktree(project, tree).empty());
  CHECK(!std::filesystem::exists(tree));
}

void TestPanelCountsItsOwnSpend() {
  TestWorkspace workspace("panel-spend");
  ScopedEnv limit("UAGENT_PANEL_DAILY_SPEND_USD", "1");
  const std::string folder = CanonicalCwd();
  // The day's first request sets the baseline: yesterday's spend is not
  // today's.
  RecordPanelCost(folder, 5.0);
  CHECK(PanelPause(folder).empty());
  RecordPanelCost(folder, 5.5);
  CHECK(PanelPause(folder).empty());
  RecordPanelCost(folder, 6.0);
  CHECK(PanelPause(folder).find("spend limit") != std::string::npos);
}

void TestChildSessionsStayOutOfTheCatalogue() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("child-sessions");
  const fs::path dir =
      fs::path(UagentDir(kHistoryDir)) / WorkspaceId(CanonicalCwd());
  fs::create_directories(dir);
  auto write = [&](const std::string& id, const json& extra) {
    json header = {{"format", kSessionFormat},
                   {"cwd", CanonicalCwd()},
                   {"model", "m"},
                   {"session_id", id},
                   {"turns", 1},
                   {"title", id}};
    header.update(extra);
    std::ofstream(dir / (id + ".json")) << JsonDump(header) << "\n{}\n";
  };
  write("ordinary", json::object());
  write("agent-aaaa1111",
        {{kSessionHeaderDelegation, {{"parent", "p"}, {"name", "reviewer"}}}});
  // A folder lists its panel only in the host's whole view.
  write("panel", {{kSessionHeaderKind, kSessionKindPanel}});
  CHECK(PanelPath(CanonicalCwd()) == (dir / "panel.json").string());
  auto ids = [](SessionScope scope) {
    std::vector<std::string> out;
    for (const SessionInfo& info : ListSessions(scope)) {
      out.push_back(fs::path(info.path).stem().string());
    }
    return out;
  };
  CHECK(ids(SessionScope::kWorkspace) == std::vector<std::string>{"ordinary"});
  auto all = ids(SessionScope::kAll);
  std::ranges::sort(all);
  CHECK(all == (std::vector<std::string>{"ordinary", "panel"}));
  const std::vector<SessionInfo> children =
      ListSessions(SessionScope::kChildren);
  CHECK(children.size() == 1);
  CHECK(JsonValue(children[0].delegation, "name", "") == "reviewer");
}

void TestMailbox() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("mailbox");
  auto note = [](const std::string& from, const std::string& text) {
    Mail mail;
    mail.from = from;
    mail.to = "recipient";
    mail.body = {{"text", text}};
    return mail;
  };
  auto texts = [](const std::vector<Mail>& mails) {
    std::vector<std::string> out;
    out.reserve(mails.size());
    for (const Mail& mail : mails) {
      out.push_back(JsonValue(mail.body, "text", ""));
    }
    return out;
  };
  auto all = [](const Mail&) { return true; };

  // A pending message wakes a watcher, and takes come in the order sent.
  MailboxWatch watch("recipient");
  CHECK(watch.Get() >= 0);
  CHECK(SendMail(note("a", "first")).empty());
  pollfd ready{watch.Get(), POLLIN, 0};
  CHECK(poll(&ready, 1, 1000) == 1);
  watch.Drain();
  CHECK(SendMail(note("a", "second")).empty());
  CHECK(PendingMail("recipient").size() == 2);
  std::vector<Mail> taken = TakeMail("recipient", all);
  CHECK(texts(taken) == std::vector<std::string>({"first", "second"}));
  CHECK(TakeMail("recipient", all).empty());

  // Taken but never acknowledged: a restarted runtime receives it again.
  RecoverMail("recipient");
  CHECK(texts(TakeMail("recipient", all)).size() == 2);
  AckMail("recipient", {taken[0].id, taken[1].id});
  RecoverMail("recipient");
  CHECK(PendingMail("recipient").empty());

  // A message the recipient does not accept yet stays pending.
  CHECK(SendMail(note("b", "later")).empty());
  CHECK(TakeMail("recipient", [](const Mail&) { return false; }).empty());
  CHECK(PendingMail("recipient").size() == 1);
  TakeMail("recipient", all);

  // A waiting duplicate is dropped.
  CHECK(SendMail(note("c", "same")).empty());
  CHECK(SendMail(note("c", "same")).empty());
  CHECK(texts(TakeMail("recipient", all)) ==
        std::vector<std::string>({"same"}));
  // So is a named mail sent again. Another mail that happens to say the
  // same is a message of its own: a thread that reports twice is heard twice.
  Mail named = note("c", "done");
  named.id = "first-report";
  CHECK(SendMail(named).empty());
  CHECK(SendMail(named).empty());
  CHECK(PendingMail("recipient").size() == 1);
  named.id = "second-report";
  CHECK(SendMail(named).empty());
  CHECK(texts(TakeMail("recipient", all)) ==
        std::vector<std::string>({"done", "done"}));

  // Loops, floods and oversized messages are refused with a reason.
  Mail looping = note("d", "again");
  looping.hops = kMailMaxHops + 1;
  CHECK(!SendMail(looping).empty());
  for (size_t i = 0; i < kMailSenderPerMinute; ++i) {
    CHECK(SendMail(note("e", "burst " + std::to_string(i))).empty());
  }
  CHECK(!SendMail(note("e", "one too many")).empty());
  // A thread's finished turn is heard however much it has said.
  Mail report = note("e", "finished");
  CHECK(SendMail(report, /*rate_limited=*/false).empty());
  CHECK(!SendMail(note("f", std::string(kMailBytes, 'x'))).empty());
  TakeMail("recipient", all);

  // Expired and unreadable messages are never delivered.
  Mail stale = note("g", "stale");
  stale.expires_ms = 1;
  CHECK(SendMail(stale).empty());
  const fs::path corrupt =
      fs::path(MailboxDir("recipient")) / "new" / "0000000000001-bad.json";
  std::ofstream(corrupt) << "{not json";
  CHECK(TakeMail("recipient", all).empty());
  CHECK(!fs::exists(corrupt));
  CHECK(PendingMail("recipient").empty());

  CHECK(!SendMail([&] {
           Mail escape = note("h", "x");
           escape.to = "../escape";
           return escape;
         }())
             .empty());
  CHECK(TakeMail("../escape", all).empty());
}

void TestSessionMail() {
  namespace fs = std::filesystem;
  TestWorkspace workspace("session-mail");
  const fs::path dir = workspace.workspace;
  // A saved session: a header naming its folder and, for a delegate, its role.
  auto save = [&](const fs::path& file, const json& delegation = nullptr) {
    json header = {{"format", kSessionFormat},
                   {"cwd", CanonicalCwd()},
                   {"model", ""},
                   {"session_id", ""},
                   {"turns", 0},
                   {"title", "T"}};
    if (!delegation.is_null()) header["delegation"] = delegation;
    std::ofstream(file) << header << "\n";
    return file.string();
  };
  const std::string parent = save(dir / "parent.json");
  const json role = {{"parent_session", parent}, {"parent", "owner"}};
  const std::string child = save(dir / "child.json", role);
  const std::string sibling = save(dir / "sibling.json", role);
  const std::string grandchild =
      save(dir / "grandchild.json", {{"parent_session", child}});
  const std::string stranger = save(dir / "stranger.json");

  // Parent, child and sibling, and nobody else: not a grandparent, nor
  // another session of the folder, whatever the approval mode.
  const ApprovalMode before = CurrentApprovalMode();
  SetApprovalMode(ApprovalMode::kYolo);
  const struct {
    std::string from, to;
    bool may;
  } rules[] = {
      {parent, child, true},       {child, parent, true},
      {child, sibling, true},      {grandchild, child, true},
      {parent, grandchild, false}, {grandchild, sibling, false},
      {parent, stranger, false},   {stranger, child, false},
  };
  for (const auto& rule : rules) {
    CHECK(MayMail(SessionParty(rule.from), SessionParty(rule.to)) == rule.may);
  }
  SetApprovalMode(before);
  // A parent with no session file is known by its owner id.
  CHECK(MayMail(OwnerParty("owner"), SessionParty(child)));
  CHECK(!MayMail(OwnerParty("someone"), SessionParty(child)));

  // Sending: the same rule, and the label says who wrote, from how the
  // sender stands to the recipient. A runtime's report names itself.
  auto received = [](const SessionMail& mail) {
    CHECK(SendSessionMail(mail).empty());
    const std::vector<Mail> taken =
        TakeMail(MailboxIdFor(mail.to_path), [](const Mail&) { return true; });
    return taken.size() == 1 && taken[0].sender_path == mail.from_path
               ? JsonValue(taken[0].body, "text", "")
               : std::string();
  };
  CHECK(received({.from_path = parent, .to_path = child, .text = "go on"}) ==
        "[parent guidance]\ngo on");
  CHECK(received({.from_path = sibling, .to_path = child, .text = "hello"}) ==
        "[session T (" + HashHex(sibling) + ")]\nhello");
  CHECK(received({.from_path = child,
                  .to_path = parent,
                  .text = "[event] done",
                  .report = true}) == "[event] done");
  CHECK(!SendSessionMail({.from_path = stranger, .to_path = child}).empty());

  // The session tool reaches the sessions the rule allows, by the id lists
  // show. A thread is in the catalogue the tool reads.
  const fs::path history = UagentDir(kHistoryDir);
  fs::create_directories(history);
  const std::string tool_parent = save(history / "tool-parent.json");
  const std::string tool_child =
      save(history / "tool-child.json",
           {{"parent_session", tool_parent}, {"lifetime", "thread"}});
  const std::string tool_other = save(history / "tool-other.json");
  ScopedEnv self("UAGENT_INTERNAL_SESSION_PATH", tool_parent);
  CHECK(LinkedSessionPath(HashHex(tool_child)) == tool_child);
  CHECK(LinkedSessionPath(HashHex(tool_other)).empty());
  CHECK(MessageSession(HashHex(tool_child), "hello").Ok());
  CHECK(MessageSession(HashHex(tool_other), "hello").error ==
        ToolErrorCode::kPermissionDenied);
  // Hops come from the mail that started the turn, not from the model.
  ToolContext looping;
  looping.hops = kMailMaxHops;
  CHECK(!SessionMessageTool()
             .run({{"session_id", HashHex(tool_child)}, {"prompt", "loop"}},
                  looping)
             .Ok());
  // Listing takes nothing and marks the linked session; a message needs a
  // prompt.
  CHECK(!FindToolArgumentIssue(SessionTool(), json::object()));
  CHECK(FindToolArgumentIssue(SessionMessageTool(),
                              {{"session_id", HashHex(tool_child)}})
            .has_value());
  json linked = json::object();
  for (const json& row : SessionSummaries()) {
    linked[JsonValue(row, "id", "")] = JsonValue(row, "linked", false);
  }
  CHECK(JsonValue(linked, HashHex(tool_child).c_str(), false));
  CHECK(!JsonValue(linked, HashHex(tool_other).c_str(), true));
}

}  // namespace uagent
