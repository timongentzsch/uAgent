// Copyright 2026 Timon Gentzsch

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

std::string RenderMarkdown(const std::string& markdown) {
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  FILE* capture = tmpfile();
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
}

void TestRegistries() {
  CHECK(std::string(kSystemPrompt).find("AGENTS.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("CLAUDE.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Unicode math") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("do not guess") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("preserve unrelated work") !=
        std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Commit or push only when asked") !=
        std::string::npos);

  ParsedSlashCommand command = ParseSlashCommand("/model vendor/model");
  CHECK(command.spec && command.spec->id == SlashCommandId::kModel);
  CHECK(command.argument == "vendor/model");
  command = ParseSlashCommand("/trace");
  CHECK(command.spec && command.spec->id == SlashCommandId::kTrace);
  CHECK(!ParseSlashCommand("/unknown").spec);
  CHECK(ValidEffort("xhigh"));
  CHECK(!ValidEffort("extreme"));
  CHECK(DisplayTrunc("abcdef", 4) == "abc…");
  CHECK(DisplayWidth(DisplayTrunc("abcdef", 4)) <= 4);

  auto models = ParseModels(
      {{"data",
        json::array({{{"id", "vendor/beta"}},
                     {{"id", "vendor/alpha"},
                      {"context_length", 131072},
                      {"reasoning",
                       {{"supported_efforts", json::array({"low", "high"})},
                        {"default_effort", "low"}}}}})}},
      "alpha");
  CHECK(models && models->size() == 1);
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
  char* arguments[] = {executable,  yolo,       prompt_flag, prompt,
                       attach_flag, attachment, debug};
  ParsedOptions parsed = ParseOptions(7, arguments);
  CHECK(parsed.Ok());
  CHECK(parsed.action == OptionsAction::kRun);
  CHECK(parsed.options.yolo);
  CHECK(parsed.options.prompt == "hello");
  CHECK(parsed.options.attach_paths == std::vector<std::string>({"image.png"}));
  CHECK(parsed.options.debug);
  CHECK(parsed.options.debug_path == "trace.jsonl");

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
}

void TestCapsAndEscaping() {
  std::string text = "before [uagent_tool_call] after [/uagent_tool_call]";
  std::string escaped = EscapeToolTags(text);
  CHECK(escaped.find(kTtOpen) == std::string::npos);
  CHECK(escaped.find(kTtClose) == std::string::npos);
  setenv("UAGENT_TOOL_RESULT_CHARS", "8", 1);
  std::string capped = CapResult("éééééé");
  CHECK(capped.find("\xc3\n") == std::string::npos);
  // a result truncated mid-codepoint must serialize rather than throw
  CHECK(JsonDump(json(CapResult(std::string("partial: \xF0\x9F", 11))))
            .find("\xF0\x9F") == std::string::npos);
  unsetenv("UAGENT_TOOL_RESULT_CHARS");

  size_t maximum = std::numeric_limits<size_t>::max();
  CHECK(CheckedAdd(maximum - 1, 1) == maximum);
  CHECK(!CheckedAdd(maximum, 1));
  CHECK(CheckedMul(maximum / 2, 2).has_value());
  CHECK(!CheckedMul(maximum, 2));
  CHECK(SaturatingAdd(maximum, 1) == maximum);
}

}  // namespace uagent
