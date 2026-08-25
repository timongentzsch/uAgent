// Copyright 2026 Timon Gentzsch

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/tools/adapt_system.h"
#include "include/tools/jobs.h"
#include "include/tools/shell.h"
#include "tests/unit/test_support.h"

namespace uagent {

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
  auto additional_issue = FindToolArgumentIssue(tool, {{"invented", true}});
  CHECK(additional_issue &&
        additional_issue->code == "schema.additional_property");
  CHECK(additional_issue && additional_issue->field == "invented");
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
  CHECK(allowed_run &&
        !allowed_run->validate({{"command", "python3 slow_analysis.py"}}));
  CHECK(allowed_run &&
        allowed_run->validate({{"command", "python3 other.py"}}));

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
  task_ids.reserve(tasks.size());
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

}  // namespace uagent
