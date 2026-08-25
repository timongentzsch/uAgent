// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "include/tools/shell.h"
#include "tests/unit/test_support.h"

namespace uagent {

namespace {

std::string ValidationMessage(const Tool& tool, const json& arguments) {
  if (!tool.validate) return {};
  auto issue = tool.validate(arguments);
  return issue ? issue->message : std::string();
}

}  // namespace

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
  // Attachments are bounded by the queue ceiling and the byte budget, not by a
  // call count that would withdraw the tool mid-turn without saying why.
  const Tool* attach = FindTool(lean_tools, "attach");
  CHECK(attach != nullptr);
  CHECK(attach && attach->max_calls_per_turn < 0);
  CHECK(attach && ToolDescription(*attach).find("Limit:") == std::string::npos);
  const Tool* run = FindTool(lean_tools, "run");
  CHECK(run != nullptr);
  if (run) {
    CHECK(run->parameters["properties"].contains("detach"));
    CHECK(run->parameters["properties"].contains("shell"));
    CHECK(run->timeout_s == 0);
    CHECK(run->command_policy);
    CHECK(static_cast<bool>(run->validate));
    CHECK(!run->validate({{"command", "cmake --build build"}}));
    CHECK(ValidationMessage(*run, {{"command", "python -c 'print(1')"}})
              .find("scratch") != std::string::npos);
    CHECK(ValidationMessage(*run, {{"command", "python3 script.py"}})
              .find("scratch") != std::string::npos);
    CHECK(ValidationMessage(*run, {{"command", "pip install reportlab"}})
              .find("PEP 723") != std::string::npos);
    CHECK(ValidationMessage(*run, {{"command", "sudo tlmgr install tcolorbox"}})
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
        !evaluator_run->validate({{"command", "python3 slow_analysis.py"}}));
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
  const Tool* activity = FindTool(lean_tools, "activity");
  CHECK(activity != nullptr);
  CHECK(activity && activity->blocking_wait_default_ms == 0);
  CHECK(activity &&
        activity->parameters["required"] == json::array({"operation"}));
  CHECK(activity &&
        activity->parameters["properties"]["operation"]["enum"] ==
            json::array({"list", "poll", "wait", "write", "resize"}));

  json materialized_poll = {
      {"operation", "poll"}, {"id", 3},          {"chars", ""},
      {"wait_ms", 0},        {"until", "ready"}, {"mode", "all"},
      {"rows", 40},          {"cols", 120},      {"max_output_chars", 0}};
  json raw_poll = materialized_poll;
  if (activity) CanonicalizeToolArguments(*activity, materialized_poll);
  CHECK(raw_poll.contains("chars") && raw_poll.contains("rows"));
  CHECK(!materialized_poll.contains("chars"));
  CHECK(!materialized_poll.contains("mode"));
  CHECK(!materialized_poll.contains("rows"));
  CHECK(!materialized_poll.contains("max_output_chars"));
  CHECK(materialized_poll.contains("until"));
  CHECK(activity && !FindToolArgumentIssue(*activity, materialized_poll));
  CHECK(activity && !activity->validate(materialized_poll));

  json materialized_write = raw_poll;
  materialized_write["operation"] = "write";
  if (activity) CanonicalizeToolArguments(*activity, materialized_write);
  CHECK(materialized_write.contains("chars"));
  CHECK(materialized_write["chars"] == "");
  CHECK(!materialized_write.contains("rows"));
  CHECK(activity && !activity->validate(materialized_write));
  CHECK(activity && !activity->mutates(materialized_poll));
  CHECK(activity && activity->mutates(materialized_write));

  auto resize_issue =
      activity ? activity->validate({{"operation", "resize"}, {"id", 3}})
               : std::nullopt;
  CHECK(resize_issue && resize_issue->code == "activity.invalid_dimensions");
  auto id_issue =
      activity ? activity->validate({{"operation", "poll"}}) : std::nullopt;
  CHECK(id_issue && id_issue->code == "activity.missing_id");
  CHECK(activity && InvalidToolArgument(
                        *activity, {{"operation", "poll"}, {"id", "bad"}}) ==
                        "`id` must be integer");

  CHECK(activity && activity->summary({{"operation", "poll"}, {"id", 3}}) ==
                        "poll activity 3");
  CHECK(activity &&
        activity->summary({{"operation", "list"}}) == "list activities");
  CHECK(
      activity &&
      activity->summary(
          {{"operation", "resize"}, {"id", 3}, {"rows", 40}, {"cols", 120}}) ==
          "resize 40×120 → activity 3");
  CHECK(
      activity &&
      activity->summary({{"operation", "write"}, {"id", 3}, {"chars", "hello"}})
              .rfind("write ", 0) == 0);
  CHECK(activity &&
        activity->summary(
                    {{"operation", "wait"}, {"mode", "all"}, {"wait_ms", 5000}})
                .find("all · all current") != std::string::npos);
  CHECK(activity && activity->summary({{"operation", "poll"},
                                       {"id", 3},
                                       {"wait_ms", 5000},
                                       {"until", "ready"}})
                            .rfind("await ready", 0) == 0);

  json overshoot = {{"operation", "poll"}, {"id", 3}, {"wait_ms", 900000}};
  if (activity) ClampToolArguments(*activity, overshoot);
  CHECK(overshoot["wait_ms"] == 300000);
  CHECK(overshoot["id"] == 3);
  CHECK(activity && InvalidToolArgument(*activity, overshoot).empty());
  const Tool* grep_tool = FindTool(lean_tools, "grep");
  json wide = {{"pattern", "x"}, {"context", 40}};
  CHECK(grep_tool != nullptr);
  if (grep_tool) ClampToolArguments(*grep_tool, wide);
  CHECK(wide["context"] == 10);
  const Tool* run_tool = FindTool(lean_tools, "run");
  json slow = {{"command", "ls"}, {"yield_ms", 60000}};
  if (run_tool) ClampToolArguments(*run_tool, slow);
  CHECK(slow["yield_ms"] == 30000);
  // A fractional value stays a type error: clamping never masks one.
  json fractional = {{"command", "ls"}, {"yield_ms", 1.5}};
  if (run_tool) ClampToolArguments(*run_tool, fractional);
  CHECK(run_tool && !InvalidToolArgument(*run_tool, fractional).empty());
  CHECK(FindTool(lean_tools, "activity_stop") != nullptr);
  for (const auto& registered : ToolSchemas(lean_tools)) {
    CHECK(registered["function"]["parameters"]["additionalProperties"] ==
          false);
    CHECK(!registered["function"]["parameters"]["properties"].contains(
        "timeout"));
  }
  fs::remove_all(root, ec);
}

}  // namespace uagent
