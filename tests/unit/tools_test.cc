// Copyright 2026 Timon Gentzsch

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/api/retry.h"
#include "include/api/stream.h"
#include "include/core/file_watch.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/tools/adapt_system.h"
#include "include/tools/jobs.h"
#include "include/tools/output_buffer.h"
#include "include/tools/shell.h"
#include "include/tools/web_fetch.h"
#include "include/tools/web_search.h"
#include "include/ui/display.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestActivitySessions() {
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
    close(watched_fd);
    unlink(watched_path);
  }

  HeadTailBuffer buffer(10);
  buffer.Push("abcdefghij");
  buffer.Push("klmnop");
  CHECK(buffer.RetainedBytes() == 10);
  CHECK(buffer.OmittedBytes() == 6);
  CHECK(buffer.Snapshot().starts_with("abcde"));
  CHECK(buffer.Snapshot().ends_with("lmnop"));
  CHECK(buffer.Snapshot().find("6 bytes omitted") != std::string::npos);
  CHECK(!buffer.Drain().empty());
  CHECK(buffer.Drain().empty());

  ProcessSupervisor admission;
  std::optional<ActivityReservation> first_slot = admission.ReserveActivity(1);
  CHECK(first_slot.has_value());
  CHECK(!admission.ReserveActivity(1).has_value());
  CHECK(!admission.TryAdd({899999, "", "busy", false, ""}, 1));
  first_slot.reset();
  CHECK(admission.ReserveActivity(1).has_value());

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
    CHECK(completed.has_value());
    if (completed) retained.Retain(std::move(*completed));
  }
  CHECK(!retained.Find(retained_ids.front()).has_value());
  CHECK(retained.Find(retained_ids.back()).has_value());

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
    ToolResult initial = ToolActivityOutput(pty_processes, id);
    CHECK(initial.output.find("(no new output)") != std::string::npos);
    CHECK(initial.no_change);
    CHECK(ToolActivityInput(pty_processes, id, "", 0, context, 30, 100).Ok());
    ToolResult input =
        ToolActivityInput(pty_processes, id, "hello\n", 2000, context);
    CHECK(input.Ok());
    CHECK(input.output.find("got:hello") != std::string::npos);
    ToolResult completed =
        ToolActivityWait(pty_processes, {id}, "all", 2000, context);
    CHECK(completed.Ok());
    CHECK(completed.output.find("exit code 0") != std::string::npos);
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
    CHECK(rejected.output.find("tty=true") != std::string::npos);
    CHECK(ToolActivityInput(non_tty, id, "\x03", 0, context).Ok());
    (void)ToolActivityWait(non_tty, {id}, "all", 2000, context);
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

void TestToolExecutionPolicy() {
  AdaptiveSystemState adaptive;
  Tool adapt = AdaptSystemTool(adaptive);
  CHECK(adapt.capabilities == 0);
  CHECK(adapt.description.find("exception, not a planning ritual") !=
        std::string::npos);
  CHECK(adapt.description.find("triggering observation") != std::string::npos);
  CHECK(adapt.parameters["properties"]["reason"]["description"]
            .get<std::string>()
            .find("material strategy delta") != std::string::npos);
  CHECK(!ToolMutates(adapt, {{"instructions", "x"}, {"reason", "phase"}}));
  CHECK(adapt.max_calls_per_turn < 0);
  CHECK(
      InvalidToolArgument(
          adapt, {{"instructions", std::string(kAdaptiveSystemBytes + 1, 'x')},
                  {"reason", "too long"}})
          .find("maximum length") != std::string::npos);
  ToolContext adaptive_context{std::chrono::steady_clock::now() +
                               std::chrono::seconds(30)};
  ToolResult adapted =
      adapt.run({{"instructions", "  Inspect the full lifecycle.  "},
                 {"reason", "The issue is cross-cutting."}},
                adaptive_context);
  CHECK(adapted.Ok());
  CHECK(adaptive.instructions == "Inspect the full lifecycle.");
  CHECK(adaptive.revision == 1);
  CHECK(!adapt
             .run({{"instructions", "Inspect the full lifecycle."},
                   {"reason", "same"}},
                  adaptive_context)
             .Ok());
  CHECK(adapt
            .run({{"instructions", "Validate the narrowed invariant."},
                  {"reason", "Evidence localized the failure."}},
                 adaptive_context)
            .Ok());
  CHECK(adaptive.revision == 2);
  CHECK(adapt
            .run({{"instructions", ""}, {"reason", "Specialization done."}},
                 adaptive_context)
            .Ok());
  CHECK(adaptive.instructions.empty());
  CHECK(adaptive.revision == 3);
  CHECK(
      !adapt.run({{"instructions", "new"}, {"reason", "   "}}, adaptive_context)
           .Ok());

  Tool tool;
  tool.name = "probe";
  tool.parameters = {{"type", "object"}, {"properties", json::object()}};
  CHECK(!ToolParameters(tool)["properties"].contains("timeout"));
  CHECK(ToolParameters(tool)["additionalProperties"] == false);
  CHECK(InvalidToolArgument(tool, {{"invented", true}}) ==
        "unknown argument `invented`");
  tool.parameters["properties"]["ids"] = {
      {"type", "array"},
      {"maxItems", 2},
      {"items", {{"type", "integer"}, {"minimum", 1}}}};
  CHECK(InvalidToolArgument(tool, {{"ids", {1, "bad"}}}) ==
        "`ids[1]` must be integer");
  CHECK(InvalidToolArgument(tool, {{"ids", {0}}}) ==
        "`ids[0]` is below its minimum");
  CHECK(InvalidToolArgument(tool, {{"ids", {1, 2, 3}}}) ==
        "`ids` has too many items");
  tool.parameters["properties"]["mode"] = {{"type", "string"},
                                           {"enum", {"any", "all"}}};
  CHECK(!InvalidToolArgument(tool, {{"mode", "some"}}).empty());
  tool.parameters["properties"]["label"] = {{"type", {"string", "null"}},
                                            {"maxLength", 3}};
  CHECK(InvalidToolArgument(tool, {{"label", nullptr}}).empty());
  CHECK(InvalidToolArgument(tool, {{"label", "long"}}) ==
        "`label` exceeds its maximum length");
  tool.parameters["properties"]["nested"] = {
      {"type", "object"},
      {"properties", {{"value", {{"type", "boolean"}}}}},
      {"required", {"value"}},
      {"additionalProperties", false}};
  CHECK(InvalidToolArgument(tool, {{"nested", json::object()}}) ==
        "`nested.value` is required");
  CHECK(InvalidToolArgument(tool,
                            {{"nested", {{"value", true}, {"extra", true}}}}) ==
        "unknown argument `nested.extra`");
  tool.stable_argument = "path";
  tool.parameters["properties"]["path"] = {{"type", "string"}};
  std::unordered_map<std::string, std::string> stable_arguments;
  CHECK(StableArgumentError(tool, {{"path", "one"}}, stable_arguments).empty());
  CHECK(StableArgumentError(tool, {{"path", "one"}}, stable_arguments).empty());
  CHECK(StableArgumentError(tool, {{"path", "two"}}, stable_arguments)
            .find("reuse") != std::string::npos);
  tool.parameters["properties"]["timeout"] = {
      {"type", "string"}, {"description", "provider argument"}};
  CHECK(ToolParameters(tool)["properties"]["timeout"] ==
        tool.parameters["properties"]["timeout"]);
  tool.timeout_s = 4;
  CHECK(tool.timeout_s == 4);

  ToolContext base{std::chrono::steady_clock::now() + std::chrono::seconds(30)};
  ToolContext bounded = base.WithTimeout(2);
  CHECK(bounded.timeout_s == 2);
  CHECK(bounded.deadline <= base.deadline);

  Tool bounded_tool =
      MakeTool("bounded", "bounded", json::object(),
               [](const json&, const ToolContext&) { return ToolSuccess(""); });
  bounded_tool.max_calls_per_turn = 2;
  Tool unbounded =
      MakeTool("unbounded", "unbounded", json::object(),
               [](const json&, const ToolContext&) { return ToolSuccess(""); });
  Tool implementation = unbounded;
  implementation.name = "implementation";
  implementation.available_in_lean = false;
  std::vector<Tool> lean_tools{unbounded, implementation};
  KeepLeanTools(lean_tools);
  CHECK(lean_tools.size() == 1);
  CHECK(lean_tools[0].name == "unbounded");
  std::vector<Tool> policies{bounded_tool, unbounded};
  json schemas = ToolSchemas(policies);
  json available = AvailableToolSchemas(policies, schemas, {{"bounded", 2}});
  CHECK(available.size() == 1);
  CHECK(available[0]["function"]["name"] == "unbounded");

  Tool inspect = unbounded;
  inspect.name = "inspect";
  inspect.capabilities = Capability(ToolCapability::kInspect);
  Tool mutate = unbounded;
  mutate.name = "mutate";
  mutate.capabilities = Capability(ToolCapability::kMutate);
  Tool exact_run = unbounded;
  exact_run.name = "run";
  exact_run.parameters = {{"type", "object"},
                          {"properties", {{"command", {{"type", "string"}}}}},
                          {"required", {"command"}}};
  exact_run.capabilities = Capability(ToolCapability::kExecute) |
                           Capability(ToolCapability::kMutate);
  exact_run.command_policy = true;
  std::vector<Tool> restricted{inspect, mutate, exact_run};
  ApplyToolPolicy(restricted, {.allowed = Capability(ToolCapability::kInspect),
                               .tool_allowlist = {"inspect", "run"},
                               .run_allowlist = {"python3 slow_analysis.py"},
                               .error = ""});
  CHECK(FindTool(restricted, "inspect") != nullptr);
  CHECK(FindTool(restricted, "mutate") == nullptr);
  const Tool* allowed_run = FindTool(restricted, "run");
  CHECK(allowed_run != nullptr);
  CHECK(
      allowed_run &&
      allowed_run->validate({{"command", "python3 slow_analysis.py"}}).empty());
  CHECK(allowed_run &&
        !allowed_run->validate({{"command", "python3 other.py"}}).empty());

  Tool terminal_only = unbounded;
  terminal_only.name = "terminal_only";
  terminal_only.visibility = Tool::Visibility::kDetachedTerminal;
  policies = {unbounded, terminal_only};
  schemas = ToolSchemas(policies);
  available = AvailableToolSchemas(policies, schemas, {});
  CHECK(available.size() == 1);
  available =
      AvailableToolSchemas(policies, schemas, {}, {.detached_terminal = true});
  CHECK(available.size() == 2);
  CHECK(available[1]["function"]["name"] == "terminal_only");

  namespace fs = std::filesystem;
  fs::path log_root = fs::temp_directory_path() /
                      ("uagent-log-artifact-" + std::to_string(getpid()));
  fs::create_directories(log_root);
  fs::path small_log = log_root / "small.log";
  {
    std::ofstream output(small_log);
    output << "small";
  }
  CollectedLog small_log_result = CollectCompletedLog(small_log.string(), 16);
  CHECK(small_log_result.output == "small");
  CHECK(!small_log_result.artifact);
  CHECK(!fs::exists(small_log));

  fs::path large_log = log_root / "large.log";
  {
    std::ofstream output(large_log);
    output << std::string(64, 'x');
  }
  CollectedLog large_log_result = [&] {
    ScopedEnv scoped_home("HOME", log_root.c_str());
    return CollectCompletedLog(large_log.string(), 16);
  }();
  CHECK(large_log_result.artifact.has_value());
  CHECK(large_log_result.artifact &&
        large_log_result.artifact->path != large_log.string());
  CHECK(large_log_result.artifact && large_log_result.artifact->bytes == 64);
  CHECK(!fs::exists(large_log));
  CHECK(large_log_result.artifact &&
        fs::exists(large_log_result.artifact->path));
  if (large_log_result.artifact) {
    std::ifstream input(large_log_result.artifact->path, std::ios::binary);
    std::string retained{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
    CHECK(retained == std::string(64, 'x'));
    struct stat artifact_status{};
    struct stat directory_status{};
    CHECK(stat(large_log_result.artifact->path.c_str(), &artifact_status) == 0);
    CHECK((artifact_status.st_mode & 0777) == 0600);
    std::string artifact_dir =
        fs::path(large_log_result.artifact->path).parent_path().string();
    CHECK(stat(artifact_dir.c_str(), &directory_status) == 0);
    CHECK((directory_status.st_mode & 0777) == 0700);
  }

  fs::path fallback_home = log_root / "fallback-home";
  fs::create_directories(fallback_home);
  {
    std::ofstream blocker(fallback_home / ".uagent");
    blocker << "not a directory";
  }
  fs::path fallback_log = log_root / "fallback.log";
  {
    std::ofstream output(fallback_log);
    output << std::string(64, 'y');
  }
  CollectedLog fallback_result = [&] {
    ScopedEnv scoped_home("HOME", fallback_home.c_str());
    return CollectCompletedLog(fallback_log.string(), 16);
  }();
  CHECK(fallback_result.artifact.has_value());
  CHECK(fallback_result.artifact &&
        fallback_result.artifact->path == fallback_log.string());
  CHECK(fs::exists(fallback_log));
  RemoveLog(fallback_log.string());
  fs::remove_all(log_root);

  ProcessSupervisor task_processes;
  BgJob task_header{7, "", "uagent -p 'very long delegated prompt'", false,
                    "subagent"};
  CHECK(BgResultHeader(task_header) == "[Background result: subagent id 7]");
  CHECK(BgResultHeader(task_header).find("delegated prompt") ==
        std::string::npos);
  BackgroundCompletion task_completion;
  task_completion.activity_id = 7;
  task_completion.kind = ActivityKind::kSubagent;
  task_completion.kind_label = "subagent";
  task_completion.command = "uagent -p 'very long delegated prompt'";
  CHECK(BgResultHeader(task_completion) ==
        "[Background result: subagent id 7]");
  ToolResult launched = RunShellCommand(task_processes, base,
                                        {.command = "sleep 10",
                                         .background = true,
                                         .immediate = true,
                                         .job_kind = "subagent"})
                            .result;
  CHECK(launched.output.starts_with("[started] subagent id "));
  CHECK(task_processes.JoinableCount() == 1);
  std::vector<BgJob> tasks = task_processes.Snapshot();
  CHECK(tasks.size() == 1);
  std::vector<pid_t> task_ids;
  for (const BgJob& job : tasks) task_ids.push_back(job.pid);
  CHECK(BgCancelSubagents(task_processes) == 1);
  CHECK(!task_processes.PendingCount());
  if (!task_ids.empty()) CHECK(!ProcessGroupAlive(task_ids[0]));

  {
    TestWorkspace memory_workspace("automatic-memory-write-limit");
    setenv("UAGENT_INTERNAL_MEMORY_SOURCE", "test-source", 1);
    ProcessSupervisor memory_processes;
    std::vector<Tool> memory_tools = BuiltinTools(memory_processes);
    unsetenv("UAGENT_INTERNAL_MEMORY_SOURCE");
    const Tool* memory = FindTool(memory_tools, "memory");
    CHECK(memory != nullptr);
    if (memory) {
      ToolResult first =
          memory->run({{"action", "set"},
                       {"key", "project/one"},
                       {"content", "One durable automatic lesson."}},
                      base);
      ToolResult second =
          memory->run({{"action", "set"},
                       {"key", "project/two"},
                       {"content", "A second automatic lesson."}},
                      base);
      CHECK(first.Ok());
      CHECK(!second.Ok());
      CHECK(second.output.find("already wrote one memory") !=
            std::string::npos);
    }
  }
}

void TestOpenRouterServerSearch() {
  RuntimeConfig config;
  Api api(config);
  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  api.model = "vendor/model";
  json schemas = json::array(
      {{{"type", "function"},
        {"function", {{"name", "web_search"}, {"parameters", json::object()}}}},
       {{"type", "function"},
        {"function",
         {{"name", "read_file"}, {"parameters", json::object()}}}}});

  bool web_available = false;
  json body = api.BuildChatBody(json::array(), schemas, "", &web_available);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][0]["function"]["name"] == "web_search");
  CHECK(web_available);

  web_available = true;
  body = api.BuildChatBody(json::array(), json::array({schemas[1]}), "",
                           &web_available);
  CHECK(body["tools"].size() == 1);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");
  CHECK(!web_available);

  auto check_native_search = [&] {
    body = api.BuildChatBody(json::array(), schemas);
    CHECK(body["tools"].size() == 2);
    CHECK(body["tools"][0]["function"]["name"] == "web_search");
  };
  api.base_url = "http://127.0.0.1:8080/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, api.base_url);
  check_native_search();

  api.base_url = "http://127.0.0.1:8787/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  check_native_search();

  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(!body.contains("tools"));  // compact/title requests stay tool-free

  Usage usage;
  usage.Add({{"prompt_tokens", 1},
             {"completion_tokens", 2},
             {"server_tool_use_details", {{"web_search_requests", 3}}}});
  CHECK(usage.web_searches == 3);
  usage.Add({{"server_tool_use", {{"web_search_requests", 2}}}});
  CHECK(usage.web_searches == 5);
  usage.Add({{"server_tool_use_details", {{"web_search_requests", 1}}},
             {"server_tool_use", {{"web_search_requests", 9}}}});
  CHECK(usage.web_searches == 6);
  CHECK(!usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).web_searches == 6);
  usage.Add({{"cost", 0.0}});
  CHECK(usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).cost_reported);

  // Every OpenAI-compatible spelling must land on the same invariant: `input`
  // excludes the cached part, so input + cache_read is the whole prompt, and
  // the hit percentage is the cached share of it. Chat Completions and
  // Responses report cached tokens inside the prompt total; Anthropic-style
  // reports them beside an input count that already excludes them, and
  // subtracting there would under-report fresh tokens.
  struct UsageCase {
    const char* spelling;
    json payload;
    int64_t input;
    int64_t output;
    int64_t cache_read;
    int64_t cache_write;
    int64_t reasoning;
    int64_t cache_hit_percent;
  };
  const std::vector<UsageCase> usage_cases = {
      // The percentage is defined as zero before anything is counted.
      {"nothing counted", json::object(), 0, 0, 0, 0, 0, 0},
      // Compatibility providers occasionally report detail counts larger than
      // the parent totals; the result is clamped, never negative.
      {"chat completions, inconsistent details",
       json{{"prompt_tokens", 2},
            {"prompt_tokens_details", {{"cached_tokens", 3}}},
            {"completion_tokens", 1},
            {"completion_tokens_details", {{"reasoning_tokens", 2}}}},
       0, 0, 3, 0, 2, 100},
      {"responses",
       json{{"input_tokens", 11},
            {"input_tokens_details", {{"cached_tokens", 3}}},
            {"output_tokens", 7},
            {"output_tokens_details", {{"reasoning_tokens", 2}}}},
       8, 5, 3, 0, 2, 27},
      {"anthropic",
       json{{"input_tokens", 8},
            {"output_tokens", 5},
            {"cache_read_input_tokens", 3},
            {"cache_creation_input_tokens", 7}},
       8, 5, 3, 7, 0, 27},
      {"anthropic translated to chat completions",
       json{{"prompt_tokens", 18},
            {"prompt_tokens_details",
             {{"cached_tokens", 3}, {"cache_creation_tokens", 7}}},
            {"completion_tokens", 5}},
       15, 5, 3, 7, 0, 16},
      {"fully cached prompt",
       json{{"prompt_tokens", 10},
            {"prompt_tokens_details", {{"cached_tokens", 10}}}},
       0, 0, 10, 0, 0, 100},
  };
  for (const UsageCase& usage_case : usage_cases) {
    Usage accounted;
    accounted.Add(usage_case.payload);
    CHECK(accounted.input == usage_case.input);
    CHECK(accounted.output == usage_case.output);
    CHECK(accounted.cache_read == usage_case.cache_read);
    CHECK(accounted.cache_write == usage_case.cache_write);
    CHECK(accounted.reasoning == usage_case.reasoning);
    CHECK(accounted.CacheHitPercent() == usage_case.cache_hit_percent);
    // Reasoning is billed output even though `output` excludes it.
    CHECK(accounted.GeneratedTokens() ==
          usage_case.output + usage_case.reasoning);
  }

  // The status row is one ordered list: everything fits when there is room,
  // and the least valuable segments go first when there is not.
  RuntimeConfig status_config;
  Api status_api(status_config);
  status_api.base_url = "https://openrouter.ai/api/v1";
  status_api.model = "vendor/model";
  status_api.ctx_window = 1000000;
  Usage status_usage;
  status_usage.input = 1200000;
  status_usage.output = 45300;
  status_usage.cache_read = 3100000;
  status_usage.cost = 0.31;
  StatusView status_view{.context_used = 4700,
                         .model = "openrouter/vendor/model:high",
                         .host = {},
                         .yolo = true};
  setenv("COLUMNS", "200", 1);
  std::string wide = StatusBar(status_api, status_usage, status_view);
  CHECK(wide.find("ctx 4.7K/1.0M") != std::string::npos);
  CHECK(wide.find("1.2M in · 45.3K out") != std::string::npos);
  CHECK(wide.find("cache 72%") != std::string::npos);
  CHECK(wide.find("openrouter/vendor/model:high") != std::string::npos);
  CHECK(wide.find("YOLO") != std::string::npos);
  // An unknown context window degrades to the used figure alone.
  status_api.ctx_window = 0;
  CHECK(StatusBar(status_api, status_usage, status_view).find("ctx 4.7K ") !=
        std::string::npos);
  unsetenv("COLUMNS");

  // A configured search endpoint outranks the conversation's own route.
  RuntimeConfig search_config;
  search_config.web_search_url = "https://search.example/v1/";
  search_config.web_search_api_key = "search-key";
  search_config.web_search_model = "search-model";
  Api search_api(search_config);
  search_api.base_url = "https://inference.example/v1";
  search_api.api_key = "inference-key";
  WebSearchRoute route = SelectWebSearchRoute(search_api, {});
  CHECK(route.base_url == "https://search.example/v1");
  CHECK(route.api_key == "search-key");
  CHECK(route.model == "search-model");
  // A provider-scoped selection is a route of its own: endpoint, key and model
  // all come from it, and the :effort suffix beats the session default.
  setenv("UAGENT_PROVIDERS",
         R"json({"seeker":{"base_url":"https://seek.example/v1",
                            "api_key":"seek-key",
                            "protocol":"openrouter"},
                 "plain":{"base_url":"https://plain.example/v1",
                           "api_key":"plain-key"}})json",
         1);
  RuntimeConfig scoped_config = search_config;
  scoped_config.web_search_url.clear();
  scoped_config.web_search_api_key.clear();
  scoped_config.web_search_model = "seeker/finder-model:high";
  scoped_config.web_search_effort = "low";
  Api scoped_api(scoped_config);
  scoped_api.base_url = "https://inference.example/v1";
  scoped_api.api_key = "inference-key";
  WebSearchRoute scoped = SelectWebSearchRoute(scoped_api, {});
  CHECK(scoped.base_url == "https://seek.example/v1");
  CHECK(scoped.api_key == "seek-key");
  CHECK(scoped.model == "finder-model");
  CHECK(scoped.effort == "high");
  CHECK(WebSearchRequest(scoped, scoped_config, "q")["reasoning"]["effort"] ==
        "high");
  // A selection scoped to a provider that does not speak the OpenRouter
  // protocol disables search rather than searching somewhere else.
  RuntimeConfig foreign_config = scoped_config;
  foreign_config.web_search_model = "plain/finder-model";
  Api foreign_api(foreign_config);
  foreign_api.base_url = "https://inference.example/v1";
  foreign_api.api_key = "inference-key";
  CHECK(!SelectWebSearchRoute(foreign_api, {}).Valid());
  // A bare id still only renames the model on the winning candidate.
  RuntimeConfig bare_config = search_config;
  bare_config.web_search_model = "plain-model";
  Api bare_api(bare_config);
  bare_api.base_url = "https://inference.example/v1";
  bare_api.api_key = "inference-key";
  WebSearchRoute bare = SelectWebSearchRoute(bare_api, {});
  CHECK(bare.base_url == "https://search.example/v1");
  CHECK(bare.model == "plain-model");
  unsetenv("UAGENT_PROVIDERS");
  // Without any configured search route, an OpenRouter conversation route is
  // the search route; an OpenAI-protocol one leaves search unavailable.
  RuntimeConfig inherit_config;
  Api inherit_api(inherit_config);
  inherit_api.base_url = "https://openrouter.ai/api/v1";
  inherit_api.api_key = "inference-key";
  inherit_api.model = "vendor/model";
  inherit_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, inherit_api.base_url);
  CHECK(SelectWebSearchRoute(inherit_api, {}).model == "vendor/model");
  inherit_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, inherit_api.base_url);
  CHECK(!SelectWebSearchRoute(inherit_api, {}).Valid());
  // `off` refuses every candidate.
  RuntimeConfig disabled_config = search_config;
  disabled_config.web_search_backend = "off";
  Api disabled_api(disabled_config);
  CHECK(!SelectWebSearchRoute(disabled_api, {}).Valid());

  RuntimeConfig openrouter_config;
  openrouter_config.web_search_engine = "exa";
  openrouter_config.web_search_context_size = "high";
  WebSearchRoute openrouter_route{"https://openrouter.ai/api/v1", "key",
                                  "vendor/search-model", ""};
  json openrouter_body =
      WebSearchRequest(openrouter_route, openrouter_config, "current facts");
  CHECK(openrouter_body["model"] == "vendor/search-model");
  CHECK(openrouter_body["tools"].size() == 1);
  CHECK(openrouter_body["tools"][0]["type"] == "openrouter:web_search");
  CHECK(openrouter_body["tools"][0]["parameters"]["engine"] == "exa");
  CHECK(openrouter_body["tools"][0]["parameters"]["max_uses"] == 3);
  CHECK(openrouter_body["tools"][0]["parameters"]["max_results"] == 5);
  CHECK(openrouter_body["tools"][0]["parameters"]["max_total_results"] == 15);
  CHECK(openrouter_body["tools"][0]["parameters"]["search_context_size"] ==
        "high");
  CHECK(openrouter_body["max_tool_calls"] == 3);
  UsageAccumulator side_usage;
  Tool search_tool = WebSearchTool(api, side_usage, {});
  // The configured budget bounds one attempt; the tool deadline covers all of
  // them, or a retry would be cancelled before it ran.
  CHECK(search_tool.timeout_s == config.web_search_timeout_s * kSideAttempts);
  CHECK(search_tool.parameters["properties"]["queries"]["maxItems"] == 3);
  CHECK(!search_tool.mutating);
  CHECK(search_tool.needs_approval &&
        search_tool.needs_approval(json::object()));

  WebSearchResult normalized = ParseWebSearch(
      {{"choices",
        json::array({{{"finish_reason", "length"},
                      {"message",
                       {{"content", "grounded answer"},
                        {"annotations",
                         json::array({{{"type", "url_citation"},
                                       {"url_citation",
                                        {{"url", "https://example.com/source"},
                                         {"title", "Source"}}}}})}}}}})}});
  CHECK(normalized.text == "grounded answer");
  CHECK(normalized.searches == 1);
  CHECK(normalized.truncated);
  CHECK(CitationEntries(normalized.annotations).size() == 1);
  // A body without choices is a failed search, not an empty answer.
  CHECK(ParseWebSearch({{"error", {{"message", "nope"}}}}).searches == 0);

  Tool fetch_tool = WebFetchTool(api);
  CHECK(!fetch_tool.mutating);
  CHECK(fetch_tool.needs_approval && fetch_tool.needs_approval(json::object()));
  ToolContext fetch_context;
  CHECK(fetch_tool.run({{"url", "file:///etc/passwd"}}, fetch_context).error ==
        ToolErrorCode::kInvalidArguments);
  CHECK(fetch_tool.run({{"url", "example.com"}}, fetch_context).error ==
        ToolErrorCode::kInvalidArguments);

  // Address policy is enforced on libcurl's resolved connection target, not
  // only on the URL spelling. Cover private, link-local, documentation, IPv6,
  // IPv4-mapped and NAT64 forms without depending on a listening service.
  for (std::string_view url : {
           "http://127.0.0.1/",
           "http://10.0.0.1/",
           "http://169.254.169.254/latest/meta-data/",
           "http://192.0.2.1/",
           "http://[::1]/",
           "http://[fd00::1]/",
           "http://[::ffff:127.0.0.1]/",
           "http://[64:ff9b::7f00:1]/",
       }) {
    WebResponse denied = api.GetUrl(std::string(url), 1, 1024);
    CHECK(denied.error == "refused non-public network destination");
  }

  // Markup out, reading order in: dropped elements take their content with
  // them, block edges become line breaks, and inline tags do not split words.
  CHECK(HtmlToText("<p>one</p><p>two</p>") == "one\n\ntwo");
  CHECK(HtmlToText("<style>p{color:red}</style><p>kept</p>") == "kept");
  CHECK(HtmlToText("<script>if (a < b) doc('</p>')</script>text") == "text");
  CHECK(HtmlToText("<!-- note --><b>bo</b>ld") == "bold");
  CHECK(HtmlToText("a &amp; b &lt;c&gt; &#39;d&#39;") == "a & b <c> 'd'");
  // Source layout adds nothing: a block break is one blank line at most,
  // however many newlines and tags produced it.
  CHECK(HtmlToText("<p>a</p>\n\n\n\n<p>b</p>") == "a\n\nb");
  CHECK(HtmlToText("  spaced   \t out  ") == "spaced out");
  // An unterminated tag ends the document rather than leaking markup.
  CHECK(HtmlToText("visible<div class=") == "visible");
  CHECK(HtmlToText("<style>only</style>").empty());
  // A `>` inside a quoted attribute does not end the tag. Pages carry JSON in
  // attributes, and stopping early spilled the remainder out as text.
  CHECK(HtmlToText(R"(<div data-mw='{"parts":["]}'>text</div>)") == "text");
  CHECK(HtmlToText("<a href=\"?a=1&amp;b=2\">link</a>") == "link");
  // Cells are columns, not lines: without a separator the figures either side
  // of a boundary ran together into one unreadable number.
  CHECK(HtmlToText("<tr><td>US</td><td>1,000</td><td>2,000</td></tr>") ==
        "| US | 1,000 | 2,000");
  // Headings carry their depth, so the outline survives the conversion.
  CHECK(HtmlToText("<h2>Title</h2><p>body</p>") == "## Title\n\nbody");
  CHECK(HtmlToText("<h6>deep</h6>") == "###### deep");
  // Site furniture is not what a reader came for.
  CHECK(HtmlToText("<nav>menu</nav><p>real</p><footer>legal</footer>") ==
        "real");
  // Indentation inside <pre> is the meaning: collapsing it broke every
  // Python block that came back through this tool.
  CHECK(HtmlToText("<pre>def f():\n    return 1\n</pre>") ==
        "def f():\n    return 1");
  CHECK(HtmlToText("<pre><span>if x:</span>\n    pass</pre>") ==
        "if x:\n    pass");
  CHECK(HtmlToText("<p>before</p><pre>  kept  </pre><p>after</p>") ==
        "before\n\n  kept  \n\nafter");

  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.HandleEvent(
      {"message",
       R"({"choices":[{"delta":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/a"}}]}}]})",
       ""});
  stream.HandleEvent(
      {"message",
       R"({"choices":[{"message":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/b"}}]}}]})",
       ""});
  stream.HandleEvent(
      {"message",
       R"({"error":{"message":"upstream overloaded","type":"server_error"}})",
       ""});
  CHECK(result.error == "upstream overloaded");
  stream.status = 200;
  stream.started = std::chrono::steady_clock::now();
  stream.last_byte = stream.started - std::chrono::seconds(1);
  auto prior_byte = stream.last_byte;
  CHECK(stream.Feed(": keepalive\n\n", 13) == 13);
  CHECK(stream.last_byte > prior_byte);
  CHECK(result.first_event_ms < 0);
  std::string citations = CitationMarkdown(result.annotations);
  CHECK(citations.find("<https://example.com/a>") != std::string::npos);
  CHECK(citations.find("<https://example.com/b>") != std::string::npos);
  auto entries = CitationEntries(result.annotations);
  CHECK(entries.size() == 2);
  SearchTrace trace;
  trace.Add(1, result.annotations);
  CHECK(trace.ArchiveMetadata().value("web_searches", int64_t{0}) == 1);
  CHECK(trace.ArchiveMetadata()["annotations"].size() == 2);
  trace.Reset();
  CHECK(trace.Empty());
  trace.Add(0, result.annotations);
  CHECK(!trace.Empty());
  json many = json::array();
  for (int i = 0; i < 25; ++i) {
    many.push_back({{"url", "https://example.com/" + std::to_string(i)},
                    {"content", std::string(5000, 'x')}});
  }
  trace.Reset();
  trace.Add(1, many);
  CHECK(trace.ArchiveMetadata()["annotations"].size() ==
        SearchTrace::kMaxSources);
  CHECK(trace.ArchiveMetadata()["annotations"][0]["content"]
            .get<std::string>()
            .size() <= SearchTrace::kMaxContentChars + 3);
  CHECK(CitationMarkdown(
            json::array({{{"url_citation", {{"url", "javascript:alert(1)"}}}}}))
            .empty());
}

void TestAttachmentEncoding() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-attachment-test-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  fs::path file = root / "tiny.txt";
  CHECK(ToolWriteFile(file.string(), "x").output.starts_with("wrote "));
  Attachment attachment;
  std::string error;
  CHECK(InspectAttachment(file.string(), attachment, error));
  CHECK(Base64File(attachment, 1, error, "data:text/plain;base64,") ==
        "data:text/plain;base64,eA==");
  error.clear();
  CHECK(Base64File(attachment, 0, error).empty());
  CHECK(!error.empty());

  fs::path image_path = root / "tiny.png";
  CHECK(ToolWriteFile(image_path.string(), "png").output.starts_with("wrote "));
  Attachment image_attachment;
  error.clear();
  CHECK(InspectAttachment(image_path.string(), image_attachment, error));
  CHECK(ImageInputError(image_attachment, true, false).empty());
  CHECK(ImageInputError(image_attachment, false, false)
            .find(image_path.string()) != std::string::npos);
  CHECK(ImageInputError(image_attachment, false, true).empty());
  // A configured vision route reads like native vision: the prompt is silent
  // either way, so the model never plans around the difference.
  CHECK(std::string(ModelImageInputInstruction(false, true)).empty());
  CHECK(ImageInputError(attachment, false, false).empty());

  error.clear();
  json content =
      AttachmentContent("inspect", {image_attachment, attachment}, error);
  CHECK(error.empty());
  CHECK(content[0]["text"].get<std::string>().find(image_path.string()) !=
        std::string::npos);
  json messages =
      json::array({{{"role", "user"}, {"content", std::move(content)}}});
  CHECK(StripContentParts(messages, "image_url") == 1);

  // A route that will not read documents never gets one encoded for it: the
  // part is not built, so a large file is not read or base64'd to be stripped
  // again. The text part still names the path, which is what the model needs
  // to reach it another way.
  error.clear();
  json refused = AttachmentContent("read it", {attachment}, error,
                                   /*image_input_available=*/true,
                                   /*image_fallback_available=*/false,
                                   /*file_input_available=*/false);
  CHECK(error.empty());
  CHECK(refused.size() == 1);
  CHECK(JsonValue(refused[0], "type", "") == "text");
  CHECK(refused[0]["text"].get<std::string>().find(file.string()) !=
        std::string::npos);
  json accepted = AttachmentContent("read it", {attachment}, error);
  CHECK(accepted.size() == 2);
  CHECK(JsonValue(accepted[1], "type", "") == "file");
  CHECK(messages[0]["content"].size() == 2);
  CHECK(messages[0]["content"][0]["text"].get<std::string>().find("withheld") ==
        std::string::npos);
  CHECK(messages[0]["content"][1].value("type", "") == "file");
  CHECK(std::string(ModelImageInputInstruction(false, false))
            .find("Image input unavailable") != std::string::npos);
  CHECK(std::string(ModelImageInputInstruction(true, false)).empty());

  setenv("UAGENT_IMAGE_PROTOCOL", "iterm", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kIterm);
  std::string iterm;
  auto collect = [](std::string& out) {
    return [&out](std::string_view part) { out.append(part); };
  };
  EmitItermImage("YWJj", 3, 20, false, collect(iterm));
  CHECK(iterm.find("\033]1337;File=inline=1") == 0);
  CHECK(iterm.find(":YWJj\a") != std::string::npos);
  std::string multipart;
  EmitItermImage("YWJj", 3, 20, true, collect(multipart));
  CHECK(multipart.find("MultipartFile=") != std::string::npos);
  CHECK(multipart.find("FilePart=YWJj") != std::string::npos);
  CHECK(multipart.find("FileEnd") != std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kItty);
  std::string kitty;
  EmitKittyPng("YWJj", 20, collect(kitty));
  CHECK(kitty.find("\033_Ga=T,f=100,c=20") == 0);
  CHECK(kitty.find("YWJj\033\\") != std::string::npos);

  bool prior_tty = g_tty;
  g_tty = true;
  setenv("UAGENT_IMAGE_PROTOCOL", "none", 1);
  CHECK(std::string(TerminalImageInstruction()).empty());
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(std::string(TerminalImageInstruction())
            .find("Use show_image to display local images") !=
        std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "ascii", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kNone);
  CHECK(std::string(TerminalImageProtocolName(DetectTerminalImageProtocol())) ==
        "none");
  CHECK(std::string(TerminalImageInstruction()).empty());
  g_tty = prior_tty;

  unsetenv("UAGENT_IMAGE_PROTOCOL");
  const char* prior_term = getenv("TERM");
  std::string saved_term = prior_term ? prior_term : "";
  setenv("TERM", "xterm-ghostty", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kItty);
  if (prior_term) {
    setenv("TERM", saved_term.c_str(), 1);
  } else {
    unsetenv("TERM");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestGrepTool() {
  namespace fs = std::filesystem;
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-grep-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root / "source files");
  fs::path source = root / "source files" / "one.cpp";
  fs::path ignored = root / "source files" / "two.txt";
  CHECK(ToolWriteFile(source.string(), "needle one\nneedle two\nneedle three\n")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile(ignored.string(), "needle ignored\n")
            .output.starts_with("wrote "));
  ProcessSupervisor supervisor;
  setenv("UAGENT_GREP_RESULTS", "2", 1);
  ToolResult result = ToolGrep(supervisor, "needle", root.string(), "*.cpp");
  unsetenv("UAGENT_GREP_RESULTS");
  CHECK(result.output.find("one.cpp") != std::string::npos);
  CHECK(result.output.find("two.txt") == std::string::npos);
  CHECK(result.output.find("more available") != std::string::npos);
  ToolResult filenames =
      ToolGrep(supervisor, "one\\.cpp$", root.string(), "", 0, {}, true);
  CHECK(filenames.Ok());
  CHECK(filenames.output.find("one.cpp") != std::string::npos);
  CHECK(filenames.output.find("needle one") == std::string::npos);
  CHECK(ToolGrep(supervisor, "one", root.string(), "", 1, {}, true).error ==
        ToolErrorCode::kInvalidArguments);
  ToolResult contextual =
      ToolGrep(supervisor, "needle two", source.string(), "", 1);
  CHECK(contextual.output.find("needle one") != std::string::npos);
  CHECK(contextual.output.find("needle three") != std::string::npos);
  CHECK(ToolGrep(supervisor, "absent", root.string(), "").output ==
        "(no matches)");
  CHECK(ToolGrep(supervisor, "(", root.string(), "").error ==
        ToolErrorCode::kProcessFailed);
  setenv("UAGENT_MAX_BACKGROUND_JOBS", "1", 1);
  CHECK(supervisor.TryAdd({999991, "", "busy", false, ""}, 1));
  ToolResult limited = ToolGrep(supervisor, "needle", root.string(), "");
  CHECK(limited.error == ToolErrorCode::kLimitExceeded);
  CHECK(limited.output.find("background job limit") != std::string::npos);
  CHECK(supervisor.TakeAllForShutdown().size() == 1);
  unsetenv("UAGENT_MAX_BACKGROUND_JOBS");

  ToolContext activity_context{std::chrono::steady_clock::now() +
                               std::chrono::seconds(5)};
  CHECK(RunShellCommand(supervisor, activity_context,
                        {.command = "sleep 0.05; echo activity-one",
                         .background = true,
                         .immediate = true})
            .result.Ok());
  CHECK(RunShellCommand(supervisor, activity_context,
                        {.command = "sleep 0.10; echo activity-two",
                         .background = true,
                         .immediate = true})
            .result.Ok());
  std::vector<int64_t> activity_ids;
  for (const BgJob& job : supervisor.Snapshot()) {
    activity_ids.push_back(ActivityId(job));
  }
  CHECK(activity_ids.size() == 2);
  ToolResult waited =
      ToolActivityWait(supervisor, activity_ids, "all", 2000, activity_context);
  CHECK(waited.Ok());
  CHECK(waited.output.find("activity-one") != std::string::npos);
  CHECK(waited.output.find("activity-two") != std::string::npos);
  CHECK(supervisor.PendingCount() == 0);

  fs::path marker = root / "injected";
  result = ToolGrep(supervisor, "needle'; touch " + marker.string() + "; '",
                    root.string(), "");
  CHECK(!fs::exists(marker));

  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  fs::path fallback_bin = root / "fallback-bin";
  fs::create_directories(fallback_bin);
  std::error_code ec;
  fs::create_symlink("/usr/bin/grep", fallback_bin / "grep", ec);
  CHECK(!ec);
  ec.clear();
  fs::create_symlink("/usr/bin/head", fallback_bin / "head", ec);
  CHECK(!ec);
  setenv("PATH", fallback_bin.c_str(), 1);
  result = ToolGrep(supervisor, "needle", source.string(), "");
  CHECK(result.output.find("[grep") == 0);
  CHECK(result.output.find("needle one") != std::string::npos);
  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }

  auto lean_tools = BuiltinTools(supervisor, root, false);
  auto image_tools = BuiltinTools(supervisor, root, true);
  CHECK(FindTool(lean_tools, "show_image") == nullptr);
  const Tool* image = FindTool(image_tools, "show_image");
  CHECK(image != nullptr);
  CHECK(image && image->serial_media);
  CHECK(image && image->replay_image);
  const Tool* run = FindTool(lean_tools, "run");
  CHECK(run != nullptr);
  if (run) {
    CHECK(run->parameters["properties"].contains("detach"));
    CHECK(run->parameters["properties"].contains("shell"));
    CHECK(run->timeout_s == 0);
    CHECK(run->command_policy);
    CHECK(static_cast<bool>(run->validate));
    CHECK(run->validate({{"command", "cmake --build build"}}).empty());
    CHECK(
        run->validate({{"command", "python -c 'print(1')"}}).find("scratch") !=
        std::string::npos);
    CHECK(run->validate({{"command", "python3 script.py"}}).find("scratch") !=
          std::string::npos);
    CHECK(
        run->validate({{"command", "pip install reportlab"}}).find("PEP 723") !=
        std::string::npos);
    CHECK(run->validate({{"command", "sudo tlmgr install tcolorbox"}})
              .find("privileged commands") != std::string::npos);
  }
  auto evaluator_tools = BuiltinTools(supervisor, root, false);
  ApplyToolPolicy(evaluator_tools,
                  {.allowed = Capability(ToolCapability::kInspect),
                   .tool_allowlist = {"grep", "read_path", "run"},
                   .run_allowlist = {"python3 slow_analysis.py"},
                   .error = ""});
  const Tool* evaluator_run = FindTool(evaluator_tools, "run");
  CHECK(evaluator_run &&
        evaluator_run->validate({{"command", "python3 slow_analysis.py"}})
            .empty());
  const Tool* python = FindTool(lean_tools, "scratch");
  CHECK(python != nullptr);
  CHECK(python && ToolDescription(*python).find(
                      "never for requested project") != std::string::npos);
  CHECK(python &&
        python->parameters.value("additionalProperties", true) == false);
  CHECK(python && python->parameters["required"] ==
                      json::array({"path", "code", "packages"}));
  CHECK(python && python->parameters["properties"]["code"]["type"] ==
                      json::array({"string", "null"}));
  const Tool* memory = FindTool(lean_tools, "memory");
  CHECK(memory != nullptr);
  if (memory) {
    CHECK(
        !ToolMutates(*memory, {{"action", "get"}, {"key", "project/lesson"}}));
    CHECK(!ToolMutates(*memory, {{"action", "list"}}));
    CHECK(!ToolMutates(*memory, {{"action", "search"}, {"key", "lesson"}}));
    CHECK(ToolMutates(
        *memory,
        {{"action", "set"}, {"key", "project/lesson"}, {"content", "fact"}}));
    CHECK(ToolMutates(*memory,
                      {{"action", "forget"}, {"key", "project/lesson"}}));
    CHECK(memory->parameters["required"] == json::array({"action"}));
  }
  CHECK(python && python->timeout_s == 0);
  CHECK(python && python->stable_argument == "path");
  CHECK(FindTool(lean_tools, "wait_background") == nullptr);
  // One tool covers list, drain, write and wait; the mode is the argument set.
  const Tool* activity = FindTool(lean_tools, "activity");
  CHECK(activity != nullptr);
  CHECK(activity && activity->blocking_wait_default_ms == 0);
  CHECK(activity && activity->parameters["properties"].contains("until"));
  CHECK(activity && activity->parameters["properties"]["mode"]["enum"] ==
                        json::array({"any", "all"}));
  CHECK(activity &&
        !activity->validate({{"id", 1}, {"wait_ms", 1000}, {"until", "ready"}})
             .size());
  CHECK(activity &&
        activity->validate({{"until", "ready"}})
                .find("requires id and wait_ms") != std::string::npos);
  CHECK(activity &&
        activity->validate({{"chars", "x"}}).find("writing requires id") !=
            std::string::npos);
  CHECK(
      activity &&
      activity->validate({{"id", 1}, {"rows", 40}}).find("supplied together") !=
          std::string::npos);
  // Reading needs no approval; writing does.
  CHECK(activity && !activity->mutates({{"id", 1}}));
  CHECK(activity && activity->mutates({{"id", 1}, {"chars", "y"}}));
  CHECK(activity && InvalidToolArgument(*activity, {{"id", "bad"}}) ==
                        "`id` must be integer");
  CHECK(FindTool(lean_tools, "activity_stop") != nullptr);
  for (const auto& registered : ToolSchemas(lean_tools)) {
    CHECK(registered["function"]["parameters"]["additionalProperties"] ==
          false);
    CHECK(!registered["function"]["parameters"]["properties"].contains(
        "timeout"));
  }
  fs::remove_all(root, ec);
}

void TestPythonTool() {
  namespace fs = std::filesystem;
  CHECK(!PythonScriptHasDependencies(
      "# /// script\n# dependencies = [\n# ]\n# ///\n"));
  CHECK(PythonScriptHasDependencies(
      "# /// script\n# dependencies = [\n#   \"numpy\",\n# ]\n# ///\n"));
  CHECK(
      PythonScriptHasDependencies("# /// script\n# dependencies = [\n# ///\n"));
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-python-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path bin = root / "bin";
  fs::create_directories(bin);
  fs::path uv = bin / "uv";
  CHECK(ToolWriteFile(
            uv.string(),
            "#!/bin/sh\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  if [ \"$1\" = --script ]; then shift; exec python3 \"$1\"; fi\n"
            "  shift\n"
            "done\n"
            "exit 2\n")
            .output.starts_with("wrote "));
  CHECK(chmod(uv.c_str(), 0700) == 0);

  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  ProcessSupervisor supervisor;
  std::vector<Tool> python_tools = BuiltinTools(supervisor, root, false);
  const Tool* run = FindTool(python_tools, "run");
  CHECK(run && ToolDescription(*run).find("omit cd") != std::string::npos);
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  ToolResult result = ToolRunPython(supervisor, root, "math.py", "print(6 * 7)",
                                    json::array({"numpy>=2"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/math.py · wrote · executed]\n42\n");
  fs::path script = root / ".uagent/scratch/math.py";
  CHECK(fs::is_regular_file(script));
  CHECK(fs::is_regular_file(root / ".uagent/scratch/.gitignore"));
  result = ToolRunPython(supervisor, root, ".uagent/scratch/prefixed.py",
                         "print('normalized')", json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/prefixed.py · wrote · executed]\n"
        "normalized\n");
  CHECK(fs::is_regular_file(root / ".uagent/scratch/prefixed.py"));
  std::ifstream script_input(script);
  std::string script_source{std::istreambuf_iterator<char>(script_input),
                            std::istreambuf_iterator<char>()};
  CHECK(script_source.find("# /// script") == 0);
  CHECK(script_source.find("\"numpy>=2\"") != std::string::npos);

  result = ToolRunPython(supervisor, root, "math.py", "print(6 * 7)",
                         json::array({"numpy>=2"}));
  CHECK(result.error == ToolErrorCode::kInvalidArguments);
  CHECK(result.output.find("code is identical") != std::string::npos);
  CHECK(result.output.find("code=null") != std::string::npos);

  result = ToolRunPython(supervisor, root, "math.py", "print(7 * 7)",
                         json::array({"numpy>=2"}));
  CHECK(result.output ==
        "[script: .uagent/scratch/math.py · overwrote · executed]\n49\n");

  CHECK(ToolEditFile(script.string(), {{"7 * 7", "8 * 8", false}}).Ok());
  result = ToolRunPython(supervisor, root, "math.py", nullptr, nullptr);
  CHECK(result.output == "[script: .uagent/scratch/math.py · executed]\n64\n");

  fs::path marker = root / "injected";
  result = ToolRunPython(supervisor, root, "safe.py", "print('safe')",
                         json::array({"x; touch " + marker.string()}));
  CHECK(result.output ==
        "[script: .uagent/scratch/safe.py · wrote · executed]\nsafe\n");
  CHECK(!fs::exists(marker));

  result = ToolRunPython(supervisor, root, "slow.py",
                         "import time; time.sleep(.2); print('slow-ok')",
                         json::array());
  CHECK(result.output ==
        "[script: .uagent/scratch/slow.py · wrote · executed]\nslow-ok\n");
  CHECK(supervisor.PendingCount() == 0);

  result =
      ToolRunPython(supervisor, root, "missing.py",
                    "import definitely_missing_uagent_package", json::array());
  CHECK(result.error == ToolErrorCode::kProcessFailed);
  CHECK(result.output.find("error: Python execution failed.") !=
        std::string::npos);
  CHECK(result.output.find("PEP 723 header") != std::string::npos);

  result = ToolRunPython(supervisor, root, "../escape.py", "print('x')",
                         json::array());
  CHECK(result.error == ToolErrorCode::kPermissionDenied);
  result = ToolRunPython(supervisor, root, "math.py", nullptr, json::array());
  CHECK(result.error == ToolErrorCode::kInvalidArguments);

  setenv("PATH", root.c_str(), 1);  // no uv
  result = ToolRunPython(supervisor, root, "dependency.py", "print('x')",
                         json::array({"numpy"}));
  CHECK(result.error == ToolErrorCode::kUnavailable);
  CHECK(result.output.find("declares third-party dependencies") !=
        std::string::npos);

  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }
  auto tools = BuiltinTools(supervisor, root, false);
  CHECK(FindTool(tools, "scratch") != nullptr);

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
