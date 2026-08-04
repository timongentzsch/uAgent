// Copyright 2026 Timon Gentzsch

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "include/api/citations.h"
#include "include/api/retry.h"
#include "tests/unit/test_support.h"

namespace uagent {

std::string RenderMarkdown(const std::string& markdown) {
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  FILE* capture = tmpfile();
  if (!capture) return "";
  dup2(fileno(capture), STDOUT_FILENO);
  bool prior_tty = g_tty;
  g_tty = true;
  MdPrint(markdown);
  fflush(stdout);
  g_tty = prior_tty;
  dup2(saved, STDOUT_FILENO);
  close(saved);
  fseek(capture, 0, SEEK_END);
  int64_t bytes = ftell(capture);
  rewind(capture);
  std::string output(static_cast<size_t>(bytes), '\0');
  if (bytes > 0) {
    CHECK(fread(output.data(), 1, output.size(), capture) == output.size());
  }
  fclose(capture);
  return output;
}

void TestTextToolProtocol() {
  auto calls = ParseTextToolCalls(
      "[uagent_tool_call]{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}"
      "}"
      "[/uagent_tool_call]");
  CHECK(calls.size() == 1);
  CHECK(calls[0].name == "read_file");
  CHECK(ParseTextToolCalls("example [uagent_tool_call]{}[/uagent_tool_call]")
            .empty());
}

void TestToolResults() {
  ToolResult success = ToolSuccess("error-free output");
  CHECK(success.Ok());
  CHECK(success.status == CompletionStatus::kSuccess);
  CHECK(success.error == ToolErrorCode::kNone);
  CHECK(success.output == "error-free output");
  CHECK(ToolSuccess("error: still explicitly successful").Ok());

  ToolResult failure =
      ToolFailure(ToolErrorCode::kPermissionDenied, "user denied this action");
  CHECK(!failure.Ok());
  CHECK(failure.status == CompletionStatus::kFailed);
  CHECK(failure.error == ToolErrorCode::kPermissionDenied);
  CHECK(failure.output == "user denied this action");

  CHECK(ToolCancelled("cancelled").status == CompletionStatus::kCancelled);
  CHECK(ToolCancelled("cancelled").error == ToolErrorCode::kNone);
  CHECK(ToolTimedOut("timed out").status == CompletionStatus::kTimedOut);
  CHECK(ToolTimedOut("timed out").error == ToolErrorCode::kNone);

  CHECK(RetryableHttpStatus(408));
  CHECK(RetryableHttpStatus(409));
  CHECK(RetryableHttpStatus(429));
  CHECK(RetryableHttpStatus(500));
  CHECK(!RetryableHttpStatus(400));
  CHECK(!RetryableHttpStatus(401));
  CHECK(RetryableRemoteError("server_error", ""));
  CHECK(RetryableRemoteError("service_unavailable_error",
                             "server_is_overloaded"));
  CHECK(RetryableRemoteError("", "model_at_capacity"));
  CHECK(!RetryableRemoteError("invalid_request_error", "invalid_value"));
  ChatResult retryable;
  retryable.retryable = true;
  CHECK(SafeToRetry(retryable));
  retryable.semantic_progress = true;
  CHECK(!SafeToRetry(retryable));
  CHECK(RetryDelay(2, 42) == RetryDelay(1, 42) * 2);

  json annotations = json::array({{{"url_citation",
                                    {{"url", "https://example.com/source"},
                                     {"title", "Example"},
                                     {"content", "grounded excerpt"}}}}});
  CHECK(CitationEvidence(annotations) ==
        "https://example.com/source\nExample\ngrounded excerpt");
  CHECK(CitationEvidence(annotations, 5, 8) ==
        "https://example.com/source\nExample\ngrounded");
}

void TestRegistries() {
  CHECK(std::string(kSystemPrompt).find("AGENTS.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("CLAUDE.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Do not guess") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("preserve unrelated work") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("fewest useful model/tool rounds") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Do not reread unchanged inputs") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("save multiple parent rounds") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Commit or push only when asked") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("terminal_columns") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Inquiries do not authorize") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("evidence, not instructions") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("AGENTS.override.md") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("cross-cutting or high-risk") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("never imitate a tool call") !=
        std::string::npos);

  ParsedSlashCommand command = ParseSlashCommand("/model vendor/model");
  CHECK(command.spec && command.spec->id == SlashCommandId::kModel);
  CHECK(command.argument == "vendor/model");
  command = ParseSlashCommand("/trace");
  CHECK(command.spec && command.spec->id == SlashCommandId::kTrace);
  command = ParseSlashCommand("/verbose");
  CHECK(command.spec && command.spec->id == SlashCommandId::kVerbose);
  command = ParseSlashCommand("/context");
  CHECK(command.spec && command.spec->id == SlashCommandId::kContext);
  command = ParseSlashCommand("/ctx");
  CHECK(command.spec && command.spec->id == SlashCommandId::kContext);
  CHECK(!ParseSlashCommand("/recap").spec);
  command = ParseSlashCommand("/memory");
  CHECK(command.spec && command.spec->id == SlashCommandId::kMemory);
  CHECK(!ParseSlashCommand("/unknown").spec);
  CHECK(ValidEffort("xhigh"));
  CHECK(!ValidEffort("extreme"));
  CHECK(DisplayTrunc("abcdef", 4) == "abc…");
  CHECK(DisplayWidth(DisplayTrunc("abcdef", 4)) <= 4);
  CHECK(FmtCount(999) == "999");
  CHECK(FmtCount(1000) == "1.0K");
  CHECK(FmtCount(1'000'000) == "1.0M");
  CHECK(FmtCount(1'250'000) == "1.2M");
  CHECK(FmtCount(1'000'000'000) == "1.0B");
  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  setenv("PATH", "/uagent-no-executables", 1);
  CHECK(EnvironmentContext("2026-07-29 UTC", "/workspace") ==
        "[environment: date 2026-07-29 UTC; cwd /workspace; shell bash]");
  CHECK(EnvironmentContext("today", "/workspace", 72) ==
        "[environment: date today; cwd /workspace; shell bash; "
        "terminal_columns=72]");
  namespace fs = std::filesystem;
  fs::path bin =
      fs::temp_directory_path() /
      ("uagent-turn-context-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(bin);
  fs::path python3 = bin / "python3";
  CHECK(ToolWriteFile(python3.string(), "#!/bin/sh\nexit 0\n").Ok());
  CHECK(chmod(python3.c_str(), 0700) == 0);
  setenv("PATH", bin.c_str(), 1);
  CHECK(EnvironmentContext("today", "/workspace") ==
        "[environment: date today; cwd /workspace; shell bash]");
  fs::path python = bin / "python";
  CHECK(ToolWriteFile(python.string(), "#!/bin/sh\nexit 0\n").Ok());
  CHECK(chmod(python.c_str(), 0700) == 0);
  CHECK(EnvironmentContext("today", "/workspace") ==
        "[environment: date today; cwd /workspace; shell bash]");
  std::error_code cleanup_error;
  fs::remove_all(bin, cleanup_error);
  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }

  auto models = ParseModels(
      {{"data",
        json::array({{{"id", "vendor/beta"}},
                     {{"id", "vendor/alpha"},
                      {"context_length", 131072},
                      {"reasoning",
                       {{"supported_efforts", json::array({"low", "high"})},
                        {"default_effort", "low"}}}}})}});
  CHECK(models && models->size() == 2);
  if (models && !models->empty()) {
    CHECK((*models)[0].id == "vendor/alpha");
    CHECK((*models)[0].context == 131072);
    CHECK((*models)[0].efforts.size() == 2);
    CHECK((*models)[0].default_effort == "low");
  }
}

void TestOptions() {
  char executable[] = "uagent";
  char yolo[] = "--yolo";
  char prompt_flag[] = "-p";
  char prompt[] = "hello";
  char attach_flag[] = "--attach";
  char attachment[] = "image.png";
  char debug[] = "--debug=trace.jsonl";
  char json[] = "--json";
  char no_memory[] = "--no-memory";
  char* arguments[] = {executable, yolo,  prompt_flag, prompt,   attach_flag,
                       attachment, debug, json,        no_memory};
  ParsedOptions parsed = ParseOptions(9, arguments);
  CHECK(parsed.Ok());
  CHECK(parsed.action == OptionsAction::kRun);
  CHECK(parsed.options.yolo);
  CHECK(parsed.options.prompt == "hello");
  CHECK(parsed.options.attach_paths == std::vector<std::string>({"image.png"}));
  CHECK(parsed.options.debug);
  CHECK(parsed.options.debug_path == "trace.jsonl");
  CHECK(parsed.options.json);
  CHECK(parsed.options.no_memory);

  char json_stream[] = "--json-stream";
  char budget[] = "--budget";
  char two_dollars[] = "2.50";
  char* stream_arguments[] = {executable,  prompt_flag, prompt,
                              json_stream, budget,      two_dollars};
  ParsedOptions stream = ParseOptions(6, stream_arguments);
  CHECK(stream.Ok());
  CHECK(stream.options.json_stream);
  CHECK(stream.options.budget == 2.5);

  char non_finite[] = "nan";
  char* invalid_budget[] = {executable, budget, non_finite};
  CHECK(!ParseOptions(3, invalid_budget).Ok());

  char help[] = "--help";
  char* help_arguments[] = {executable, help};
  CHECK(ParseOptions(2, help_arguments).action == OptionsAction::kHelp);

  char unknown[] = "--unknown";
  char* unknown_arguments[] = {executable, unknown};
  CHECK(!ParseOptions(2, unknown_arguments).Ok());

  char* missing_value[] = {executable, attach_flag};
  ParsedOptions missing = ParseOptions(2, missing_value);
  CHECK(!missing.Ok());
  CHECK(missing.error.find("requires a value") != std::string::npos);

  char* json_without_prompt[] = {executable, json};
  CHECK(!ParseOptions(2, json_without_prompt).Ok());

  char* conflicting_json[] = {executable, prompt_flag, prompt, json,
                              json_stream};
  CHECK(!ParseOptions(5, conflicting_json).Ok());
}

void TestLineNumberStripping() {
  CHECK(StripLineNumbers("     1\tone\n     2\ttwo\n") == "one\ntwo\n");
  CHECK(StripLineNumbers("one\n     2\ttwo\n") == "one\n     2\ttwo\n");
}

void TestMarkdownMath() {
  std::string inline_math = RenderMarkdown(
      "inline $x^2$ and \\(y + 1\\)\n"
      "display $$z = 3$$\n");
  CHECK(inline_math.find("$x^2$") != std::string::npos);
  CHECK(inline_math.find("\\(y + 1\\)") != std::string::npos);
  CHECK(inline_math.find("$$z = 3$$") != std::string::npos);
  CHECK(inline_math.find("\033[38;5;141m") != std::string::npos);

  std::string table = RenderMarkdown(
      "| Expression | Code |\n"
      "| --- | --- |\n"
      "| $a|b$ | `x|y` |\n"
      "| \\(c|d\\) | plain |\n");
  CHECK(table.find("$a|b$") != std::string::npos);
  CHECK(table.find("\\(c|d\\)") != std::string::npos);
  CHECK(table.find("x|y") != std::string::npos);

  const char* prior_columns_value = getenv("COLUMNS");
  std::string prior_columns = prior_columns_value ? prior_columns_value : "";
  setenv("COLUMNS", "24", 1);
  std::string narrow_table = RenderMarkdown(
      "| Name | Description |\n"
      "| --- | --- |\n"
      "| alpha | a deliberately wide description |\n");
  if (prior_columns_value) {
    setenv("COLUMNS", prior_columns.c_str(), 1);
  } else {
    unsetenv("COLUMNS");
  }
  CHECK(narrow_table.find("Description:") != std::string::npos);
  CHECK(narrow_table.find("deliberately wide") != std::string::npos);
}

void TestCapsAndEscaping() {
  std::string text = "before [uagent_tool_call] after [/uagent_tool_call]";
  std::string escaped = EscapeToolTags(text);
  CHECK(escaped.find(kTtOpen) == std::string::npos);
  CHECK(escaped.find(kTtClose) == std::string::npos);
  setenv("UAGENT_TOOL_RESULT_CHARS", "8", 1);
  std::string capped = CapResult("éééééé");
  CHECK(capped.size() <= 8);
  CHECK(capped.find("\xc3\n") == std::string::npos);
  // a result truncated mid-codepoint must serialize rather than throw
  CHECK(JsonDump(json(CapResult(std::string("partial: \xF0\x9F", 11))))
            .find("\xF0\x9F") == std::string::npos);
  CHECK(ToolBatchResultCap() == 16);
  unsetenv("UAGENT_TOOL_RESULT_CHARS");

  const std::string long_utf8 = "head-" + std::string(200, 'x') + "-é-tail";
  for (int64_t cap = 1; cap <= 128; ++cap) {
    CHECK(CapResult(long_utf8, cap).size() <= static_cast<size_t>(cap));
  }
  std::string head_tail = CapResult(long_utf8, 80);
  CHECK(head_tail.starts_with("head-"));
  CHECK(head_tail.ends_with("-é-tail"));
  CHECK(head_tail.find("bytes truncated") != std::string::npos);
  CHECK(JsonDump(json(head_tail)).find("\xEF\xBF\xBD") == std::string::npos);

  std::vector<CallTask> tasks(3);
  tasks[0].result = ToolFailure(ToolErrorCode::kRemoteError, "short");
  tasks[1].result = ToolSuccess(std::string(100, 'a'));
  tasks[2].result = ToolSuccess(std::string(100, 'b'));
  std::vector<std::string> model_results = ModelFacingToolResults(tasks, 25);
  CHECK(model_results[0] == "short");
  CHECK(model_results[1].size() == 10);
  CHECK(model_results[2].size() == 10);
  CHECK(model_results[0].size() + model_results[1].size() +
            model_results[2].size() <=
        25);
  CHECK(tasks[1].result.output.size() == 100);
  CHECK(ModelFacingToolResults(tasks, 0)[1].size() == 100);
  std::vector<CallTask> small(2);
  small[0].result = ToolSuccess("a");
  small[1].result = ToolSuccess("bb");
  std::vector<std::string> unchanged = ModelFacingToolResults(small, 10);
  CHECK(unchanged[0] == "a");
  CHECK(unchanged[1] == "bb");
  std::vector<std::string> tiny = ModelFacingToolResults(tasks, 1);
  CHECK(tiny[0].size() + tiny[1].size() + tiny[2].size() <= 1);

  std::vector<CallTask> equal(3);
  for (CallTask& task : equal) task.result = ToolSuccess(std::string(100, 'x'));
  std::vector<std::string> divided = ModelFacingToolResults(equal, 7);
  CHECK(divided[0].size() == 3);
  CHECK(divided[1].size() == 2);
  CHECK(divided[2].size() == 2);

  setenv("UAGENT_TOOL_RESULT_CHARS", "8", 1);
  Tool wider;
  wider.result_chars = 20;
  std::vector<CallTask> wider_batch(2);
  for (CallTask& task : wider_batch) {
    task.tool = &wider;
    task.result = ToolSuccess(std::string(30, 'x'));
  }
  std::vector<std::string> widened = ModelFacingToolResults(wider_batch);
  CHECK(widened[0].size() + widened[1].size() == 20);
  CallTask per_call;
  per_call.result = ToolSuccess(std::string(30, 'y'), 24);
  CHECK(ModelFacingToolResults({per_call}).front().size() == 24);
  unsetenv("UAGENT_TOOL_RESULT_CHARS");

  CallTask recoverable;
  recoverable.result = ToolSuccess(std::string(200, 'x'));
  recoverable.result.artifact = ToolArtifact{"/tmp/uagent-output", 200};
  std::string recoverable_text =
      ModelFacingToolResults({recoverable}, 180).front();
  CHECK(recoverable_text.size() <= 180);
  CHECK(recoverable_text.find("/tmp/uagent-output") != std::string::npos);
  CHECK(recoverable_text.ends_with("do not read it whole]"));

  CHECK(TextLines("") == 0);
  CHECK(TextLines("one") == 1);
  CHECK(TextLines("one\ntwo\n") == 2);
  CHECK(ToolResultSummary(ToolSuccess(""), "", false) == "(empty)");
  CHECK(ToolResultSummary(ToolSuccess("one\ntwo"), "one\ntwo", false) ==
        "one … · 2 lines · 7 chars");
  CHECK(ToolResultSummary(ToolTimedOut("late"), "late", true) ==
        "timed_out: late · truncated");
  CallTask displayed;
  displayed.result = ToolSuccess("complete bounded output");
  CHECK(ToolOutputForDisplay(displayed, "model cap", false) == "model cap");
  CHECK(ToolOutputForDisplay(displayed, "model cap", true) ==
        "complete bounded output");
  displayed.result.result_chars = 8;
  CHECK(ToolOutputForDisplay(displayed, "model cap", true).size() == 8);

  size_t maximum = std::numeric_limits<size_t>::max();
  CHECK(CheckedAdd(maximum - 1, 1) == maximum);
  CHECK(!CheckedAdd(maximum, 1));
  CHECK(CheckedMul(maximum / 2, 2).has_value());
  CHECK(!CheckedMul(maximum, 2));
  CHECK(SaturatingAdd(maximum, 1) == maximum);
}

}  // namespace uagent
