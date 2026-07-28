// Copyright 2026 Timon Gentzsch

#include <clocale>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/cli.h"
#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/registry.h"
#include "include/tools/shell.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
#include "include/tools/tool.h"

namespace uagent {
namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
  if (condition) return;
  std::cerr << "FAIL line " << line << ": " << expression << '\n';
  ++failures;
}

#define CHECK(expression) check((expression), #expression, __LINE__)

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

void TestRegistries() {
  CHECK(std::string(kSystemPrompt).find("AGENTS.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("CLAUDE.md") != std::string::npos);
  CHECK(std::string(kSystemPrompt).find("Unicode math") != std::string::npos);

  ParsedSlashCommand command = ParseSlashCommand("/model vendor/model");
  CHECK(command.spec && command.spec->id == SlashCommandId::kModel);
  CHECK(command.argument == "vendor/model");
  CHECK(!ParseSlashCommand("/unknown").spec);
  CHECK(ValidEffort("xhigh"));
  CHECK(!ValidEffort("extreme"));

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
}

void TestFileTools() {
  namespace fs = std::filesystem;
  auto contents = [](const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  };
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  fs::path file = root / "file.txt";
  CHECK(ToolWriteFile(file.string(), "one\ntwo\n").starts_with("wrote "));
  std::string read = ToolReadFile(file.string(), 1, 1);
  CHECK(read.find("lines 1-1") != std::string::npos);
  CHECK(read.find("\none\n") != std::string::npos);
  CHECK(ToolEditFile(file.string(), "two", "three").starts_with("edited "));

  struct stat before{}, after{};
  CHECK(stat(file.c_str(), &before) == 0);
  CHECK(ToolEditFile(file.string(), "three", "three").find("makes no change") !=
        std::string::npos);
  CHECK(stat(file.c_str(), &after) == 0);
  CHECK(before.st_ino == after.st_ino);

  fs::path crlf = root / "crlf.txt";
  CHECK(ToolWriteFile(crlf.string(), "one\r\ntwo\r\n").starts_with("wrote "));
  CHECK(ToolEditFile(crlf.string(), "one\ntwo\n", "ONE\ntwo\n")
            .starts_with("edited "));
  CHECK(contents(crlf) == "ONE\r\ntwo\r\n");
  CHECK(
      ToolEditFile(crlf.string(), "two", "two\nthree").starts_with("edited "));
  CHECK(contents(crlf) == "ONE\r\ntwo\r\nthree\r\n");
  CHECK(ToolEditFile(file.string(), "one\r\nthree\r\n", "ONE\r\nthree\r\n")
            .starts_with("edited "));
  CHECK(contents(file) == "ONE\nthree\n");

  fs::path mixed = root / "mixed.txt";
  CHECK(ToolWriteFile(mixed.string(), "a\r\nb\nc\n").starts_with("wrote "));
  CHECK(ToolEditFile(mixed.string(), "c", "x\ny").starts_with("edited "));
  CHECK(contents(mixed) == "a\r\nb\nx\ny\n");

  fs::path literal_crlf = root / "literal-crlf.txt";
  CHECK(
      ToolWriteFile(literal_crlf.string(), "payload\n").starts_with("wrote "));
  CHECK(ToolEditFile(literal_crlf.string(), "payload", "a\r\nb")
            .starts_with("edited "));
  CHECK(contents(literal_crlf) == "a\r\nb\n");

  fs::path bom = root / "bom.txt";
  CHECK(ToolWriteFile(bom.string(), "\xEF\xBB\xBFone\n").starts_with("wrote "));
  CHECK(ToolEditFile(bom.string(), "one", "two").starts_with("edited "));
  CHECK(contents(bom) == "\xEF\xBB\xBFtwo\n");

  fs::path batch = root / "batch.txt";
  CHECK(ToolWriteFile(batch.string(), "alpha one alpha two\n")
            .starts_with("wrote "));
  std::string batch_result = ToolEditFile(
      batch.string(), {{"alpha", "beta", true}, {"beta two", "gamma", false}});
  CHECK(batch_result.find("3 replacements across 2 edits") !=
        std::string::npos);
  CHECK(contents(batch) == "beta one gamma\n");
  CHECK(ToolWriteFile(batch.string(), "same same\n").starts_with("wrote "));
  CHECK(ToolEditFile(batch.string(), "same", "other").find("matches 2 times") !=
        std::string::npos);
  CHECK(contents(batch) == "same same\n");
  CHECK(ToolEditFile(batch.string(), {{"same", "changed", true},
                                      {"missing", "never written", false}})
            .find("edit 2 `old` not found") != std::string::npos);
  CHECK(contents(batch) == "same same\n");

  fs::path registered = root / "registered.txt";
  CHECK(ToolWriteFile(registered.string(), "a a c\n").starts_with("wrote "));
  ProcessSupervisor supervisor;
  std::vector<Tool> tools = BuiltinTools(supervisor, root);
  const Tool* edit_tool = FindTool(tools, "edit_file");
  CHECK(edit_tool != nullptr);
  if (edit_tool) {
    CHECK(edit_tool
              ->run({{"path", registered.string()},
                     {"old", "a"},
                     {"new", "b"},
                     {"replace_all", true},
                     {"edits", json::array({{{"old", "c"}, {"new", "d"}}})}},
                    {})
              .starts_with("edited "));
    CHECK(contents(registered) == "b b d\n");
  }

  fs::path private_file = root / "private";
  CHECK(ToolWritePrivateFile(private_file.string(), "secret")
            .starts_with("wrote "));
  struct stat st{};
  CHECK(stat(private_file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(ToolEditFile(private_file.string(), "secret", "private")
            .starts_with("edited "));
  CHECK(stat(private_file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  fs::path ledger = root / "ledger";
  std::string append_error;
  CHECK(AppendPrivateLine(ledger.string(), "one", append_error));
  CHECK(AppendPrivateLine(ledger.string(), "two", append_error));
  CHECK(stat(ledger.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(ToolReadFile(ledger.string(), 1, -1).find("two") != std::string::npos);
  CHECK(PathWithin(CanonicalAccessPath(file.string()),
                   CanonicalAccessPath(root.string())));
  CHECK(!PathWithin(CanonicalAccessPath(root.parent_path().string()),
                    CanonicalAccessPath(root.string())));
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestTerminalSafety() {
  bool prior = g_tty;
  g_tty = true;
  CHECK(std::string(RST()).find("\033[49m") != std::string::npos);
  CHECK(TerminalSafe("ok\x1b]52;bad\a") == "ok\\x1b]52;bad\\x07");
  g_tty = false;
  CHECK(TerminalSafe("\x1b") == "\x1b");
  g_tty = prior;
  CHECK(DisplayWidth("ASCII") == 5);
  CHECK(DisplayWidth("界") >= 1);
  CHECK(SafeFileComponent("../../escape") == "______escape");
  CHECK(FirstLine(std::string(200, 'x')).size() == 200);
  CHECK(FirstLine("first\nsecond") == "first");
}

void TestBackgroundValidation() {
  namespace fs = std::filesystem;
  ProcessSupervisor supervisor;
  CHECK(ToolWaitBackground(supervisor, 0).starts_with("error:"));
  CHECK(ToolWaitBackground(supervisor, 999999).starts_with("error:"));
  auto add_job = [&](BgJob job) {
    supervisor.WithJobs([&](std::vector<BgJob>& jobs) { jobs.push_back(job); });
  };
  add_job({999998, "", "", false, true, {}, ""});
  CHECK(!supervisor.PendingCount());
  CHECK(supervisor.DetachedCount() == 1);
  CHECK(ToolWaitBackground(supervisor, 999998).starts_with("[detached job"));
  add_job({999997, "", "", false, false, {}, ""});
  CHECK(supervisor.PendingCount());
  CHECK(supervisor.PendingCount() == 1);
  CHECK(supervisor.PendingPids() == std::vector<pid_t>{999997});
  CHECK(!supervisor.TryAdd({999996, "", "", false, false, {}, ""}, 1));
  supervisor.WithJobs([](std::vector<BgJob>& jobs) { jobs.clear(); });
  std::vector<Tool> tools = BuiltinTools(supervisor);
  const Tool* wait = FindTool(tools, "wait_background");
  CHECK(wait && !wait->accepts_timeout);
  const Tool* edit = FindTool(tools, "edit_file");
  CHECK(edit && edit->parameters["properties"].contains("replace_all"));
  CHECK(edit && edit->parameters["properties"].contains("edits"));
  if (edit) {
    CHECK(ToolSummary(*edit, {{"path", "x"}, {"old", "a"}, {"new", "b"}}) ==
          "x (1 edit)");
    CHECK(ToolSummary(*edit, {{"path", "x"},
                              {"old", "a"},
                              {"new", "b"},
                              {"edits",
                               json::array({{{"old", "c"}, {"new", "d"}}})}}) ==
          "x (2 edits)");
    CHECK(edit->run({{"path", "x"},
                     {"old", "a"},
                     {"new", "b"},
                     {"edits", json::array({"bad"})}},
                    {})
              .find("each `edits` entry") != std::string::npos);
  }
  // read-only and independent-process tools must be able to overlap, and the
  // schema has to say so or the model has no reason to batch them
  for (const char* name :
       {"read_file", "list_dir", "grep", "run", "terminal_output"}) {
    const Tool* tool = FindTool(tools, name);
    CHECK(tool && tool->parallel_safe);
    if (tool) {
      CHECK(ToolDescription(*tool).find("Batchable") != std::string::npos);
    }
  }

  fs::path log =
      fs::temp_directory_path() /
      ("uagent-utf8-tail-" + std::to_string(static_cast<int64_t>(getpid())));
  CHECK(ToolWritePrivateFile(log.string(), "abc😀tail").starts_with("wrote "));
  std::string tail = ReadLogTail(log.string(), 7);  // begins inside 😀
  CHECK(JsonDump(json(tail)).find("\x9F") == std::string::npos);
  std::error_code ec;
  fs::remove(log, ec);
}

void TestToolExecutionPolicy() {
  Tool tool;
  tool.name = "probe";
  tool.parameters = {{"type", "object"}, {"properties", json::object()}};
  json parameters = ToolParameters(tool, 17);
  CHECK(parameters["properties"]["timeout"].value("minimum", -1) == 0);
  CHECK(parameters["properties"]["timeout"]
            .value("description", "")
            .find("default 17") != std::string::npos);
  CHECK(!tool.parameters["properties"].contains("timeout"));
  tool.timeout_s = 4;
  CHECK(ToolParameters(tool, 17)["properties"]["timeout"]
            .value("description", "")
            .find("default 4") != std::string::npos);

  ToolContext base{std::chrono::steady_clock::now() + std::chrono::seconds(30)};
  ToolContext bounded = base.WithTimeout(2);
  CHECK(bounded.timeout_s == 2);
  CHECK(bounded.deadline <= base.deadline);

  SideTaskSupervisor side_tasks;
  int64_t id = side_tasks.Start(
      "probe", "quick",
      [](const std::atomic<bool>&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        return std::string("done");
      },
      1);
  CHECK(id > 0);
  CHECK(side_tasks.Joinable() == 1);
  CHECK(side_tasks.Start(
            "probe", "over limit",
            [](const std::atomic<bool>&) { return std::string(); }, 1) == 0);
  CHECK(!side_tasks.Wait(id, std::chrono::milliseconds(1)).has_value());
  auto result = ToolWaitSideTask(side_tasks, id);
  CHECK(result.find("[Background result: probe `quick`]") != std::string::npos);
  CHECK(result.find("done") != std::string::npos);
  CHECK(side_tasks.Joinable() == 0);

  CHECK(side_tasks.Start(
            "probe", "detached",
            [](const std::atomic<bool>&) { return std::string("done"); }, 1,
            false) > 0);
  CHECK(side_tasks.Joinable() == 0);
  side_tasks.CancelAll();

  CHECK(side_tasks.Start(
            "probe", "cancel",
            [](const std::atomic<bool>& cancel) {
              while (!cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              return std::string("cancelled");
            },
            1) > 0);
  CHECK(side_tasks.CancelAll() == 1);
  CHECK(side_tasks.Empty());

  Tool bounded_tool =
      MakeTool("bounded", "bounded", json::object(),
               [](const json&, const ToolContext&) { return ""; });
  bounded_tool.max_calls_per_turn = 2;
  Tool unbounded = MakeTool("unbounded", "unbounded", json::object(),
                            [](const json&, const ToolContext&) { return ""; });
  std::vector<Tool> policies{bounded_tool, unbounded};
  json schemas = ToolSchemas(policies);
  json available = AvailableToolSchemas(policies, schemas, {{"bounded", 2}});
  CHECK(available.size() == 1);
  CHECK(available[0]["function"]["name"] == "unbounded");

  ProcessSupervisor task_processes;
  std::vector<Tool> task_tools;
  AddTaskLifecycleTools(task_tools, task_processes);
  CHECK(FindTool(task_tools, "get_task_output") != nullptr);
  CHECK(FindTool(task_tools, "wait_tasks") != nullptr);
  const Tool* get = FindTool(task_tools, "get_task_output");
  const Tool* kill = FindTool(task_tools, "kill_task");
  CHECK(get && get->summary(json{{"id", 7}}) == "task 7");
  CHECK(kill && kill->mutating);
  CHECK(kill && kill->summary(json{{"id", 7}}) == "task 7");

  std::string launched =
      ToolRunBash(task_processes, "sleep 0.2; printf task-done", 3, false, base,
                  true, false, "bash", true, "task");
  CHECK(launched.starts_with("[backgrounded] task id "));
  std::vector<pid_t> task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 1);
  if (!task_ids.empty()) {
    int64_t task_id = task_ids[0];
    CHECK(!ToolGetTaskOutput(task_processes, task_id).starts_with("error:"));
    ToolContext wait_context{std::chrono::steady_clock::now() +
                             std::chrono::seconds(2)};
    std::string waited = ToolWaitTasks(task_processes, json::array({task_id}),
                                       true, wait_context);
    CHECK(waited.find("task-done") != std::string::npos);
    CHECK(!task_processes.PendingCount());
  }

  launched = ToolRunBash(task_processes, "sleep 10", 3, false, base, true,
                         false, "bash", true, "task");
  task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 1);
  if (!task_ids.empty()) {
    CHECK(ToolKillTask(task_processes, task_ids[0]).find("cancelled") !=
          std::string::npos);
    CHECK(!task_processes.PendingCount());
  }

  ToolRunBash(task_processes, "sleep 0.1; printf first", 3, false, base, true,
              false, "bash", true, "task");
  ToolRunBash(task_processes, "sleep 10", 3, false, base, true, false, "bash",
              true, "task");
  task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 2);
  if (task_ids.size() == 2) {
    ToolContext wait_context{std::chrono::steady_clock::now() +
                             std::chrono::seconds(2)};
    std::string first =
        ToolWaitTasks(task_processes, json::array({task_ids[0], task_ids[1]}),
                      false, wait_context);
    CHECK(first.find("first") != std::string::npos);
    CHECK(task_processes.PendingCount() == 1);
    CHECK(ToolKillTask(task_processes, task_ids[1]).find("cancelled") !=
          std::string::npos);
  }
}

void TestOpenRouterServerSearch() {
  RuntimeConfig config;
  Api api(config);
  api.base_url = "https://openrouter.ai/api/v1";
  api.model = "vendor/model";
  json schemas = json::array(
      {{{"type", "function"},
        {"function", {{"name", "web_search"}, {"parameters", json::object()}}}},
       {{"type", "function"},
        {"function",
         {{"name", "read_file"}, {"parameters", json::object()}}}}});

  json body = api.BuildChatBody(json::array(), schemas);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");
  CHECK(body["tools"][1]["type"] == "openrouter:web_search");
  CHECK(body["tools"][1]["parameters"]["engine"] == "auto");
  CHECK(body["tools"][1]["parameters"]["max_results"] == 5);
  CHECK(body["tools"][1]["parameters"]["max_uses"] == 3);
  CHECK(!body["tools"][1]["parameters"].contains("search_context_size"));

  body = api.BuildChatBody(json::array(), json::array({schemas[1]}));
  CHECK(body["tools"].size() == 1);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");

  api.openrouter_web_search = false;
  body = api.BuildChatBody(json::array(), schemas);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][0]["function"]["name"] == "web_search");

  api.base_url = "http://127.0.0.1:8080/v1";
  body = api.BuildChatBody(json::array(), schemas);
  CHECK(body["tools"].size() == 1);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");

  api.base_url = "https://openrouter.ai/api/v1";
  api.openrouter_web_search = true;
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(!body.contains("tools"));  // compact/title requests stay tool-free

  Usage usage;
  usage.Add({{"prompt_tokens", 1},
             {"completion_tokens", 2},
             {"server_tool_use", {{"web_search_requests", 3}}}});
  CHECK(usage.web_searches == 3);
  CHECK(UsageFromJson(UsageJson(usage)).web_searches == 3);

  ChatResult result;
  StreamCtx stream;
  stream.res = &result;
  stream.HandleLine(
      R"(data: {"choices":[{"delta":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/a"}}]}}]})");
  stream.HandleLine(
      R"(data: {"choices":[{"message":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/b"}}]}}]})");
  std::string citations = CitationMarkdown(result.annotations);
  CHECK(citations.find("<https://example.com/a>") != std::string::npos);
  CHECK(citations.find("<https://example.com/b>") != std::string::npos);
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
  CHECK(ToolWriteFile(file.string(), "x").starts_with("wrote "));
  Attachment attachment;
  std::string error;
  CHECK(InspectAttachment(file.string(), attachment, error));
  CHECK(Base64File(attachment, 1, error, "data:text/plain;base64,") ==
        "data:text/plain;base64,eA==");
  error.clear();
  CHECK(Base64File(attachment, 0, error).empty());
  CHECK(!error.empty());

  fs::path image_path = root / "tiny.png";
  CHECK(ToolWriteFile(image_path.string(), "png").starts_with("wrote "));
  Attachment image_attachment;
  error.clear();
  CHECK(InspectAttachment(image_path.string(), image_attachment, error));
  CHECK(ImageInputError(image_attachment).empty());
  g_image_input = false;
  CHECK(ImageInputError(image_attachment).find(image_path.string()) !=
        std::string::npos);
  CHECK(ImageInputError(attachment).empty());
  g_image_input = true;

  error.clear();
  json content =
      AttachmentContent("inspect", {image_attachment, attachment}, error);
  CHECK(error.empty());
  CHECK(content[0]["text"].get<std::string>().find(image_path.string()) !=
        std::string::npos);
  json messages =
      json::array({{{"role", "user"}, {"content", std::move(content)}}});
  CHECK(StripImageContentParts(messages) == 1);
  CHECK(messages[0]["content"].size() == 2);
  CHECK(messages[0]["content"][0]["text"].get<std::string>().find(
            "1 image withheld") != std::string::npos);
  CHECK(messages[0]["content"][1].value("type", "") == "file");

  setenv("UAGENT_IMAGE_PROTOCOL", "iterm", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kIterm);
  std::string iterm = ItermImageSequence("YWJj", 3, 20, false);
  CHECK(iterm.find("\033]1337;File=inline=1") == 0);
  CHECK(iterm.find(":YWJj\a") != std::string::npos);
  std::string multipart = ItermImageSequence("YWJj", 3, 20, true);
  CHECK(multipart.find("MultipartFile=") != std::string::npos);
  CHECK(multipart.find("FilePart=YWJj") != std::string::npos);
  CHECK(multipart.find("FileEnd") != std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kItty);
  std::string kitty = KittyPngSequence("YWJj", 20);
  CHECK(kitty.find("\033_Ga=T,f=100,c=20") == 0);
  CHECK(kitty.find("YWJj\033\\") != std::string::npos);

  bool prior_tty = g_tty;
  g_tty = true;
  setenv("UAGENT_IMAGE_PROTOCOL", "none", 1);
  CHECK(std::string(TerminalImageInstruction()).find("Images unavailable") !=
        std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(std::string(TerminalImageInstruction()).find("show_image (native)") !=
        std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "ascii", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kNone);
  CHECK(std::string(TerminalImageProtocolName(DetectTerminalImageProtocol())) ==
        "none");
  CHECK(std::string(TerminalImageInstruction()).find("Images unavailable") !=
        std::string::npos);
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
            .starts_with("wrote "));
  CHECK(ToolWriteFile(ignored.string(), "needle ignored\n")
            .starts_with("wrote "));
  ProcessSupervisor supervisor;
  setenv("UAGENT_GREP_RESULTS", "2", 1);
  std::string result = ToolGrep(supervisor, "needle", root.string(), "*.cpp");
  unsetenv("UAGENT_GREP_RESULTS");
  CHECK(result.find("one.cpp") != std::string::npos);
  CHECK(result.find("two.txt") == std::string::npos);
  CHECK(result.find("more available") != std::string::npos);
  CHECK(ToolGrep(supervisor, "absent", root.string(), "") == "(no matches)");
  CHECK(ToolGrep(supervisor, "(", root.string(), "").starts_with("error:"));

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
  CHECK(result.find("[grep") == 0);
  CHECK(result.find("needle one") != std::string::npos);
  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }

  auto lean_tools = BuiltinTools(supervisor, root, false);
  auto image_tools = BuiltinTools(supervisor, root, true);
  CHECK(FindTool(lean_tools, "show_image") == nullptr);
  CHECK(FindTool(image_tools, "show_image") != nullptr);
  const Tool* run = FindTool(lean_tools, "run");
  CHECK(run != nullptr);
  if (run) {
    CHECK(run->parameters["properties"].contains("detach"));
    CHECK(run->parameters["properties"].contains("shell"));
    CHECK(run->full_terminal_output);
  }
  const Tool* python = FindTool(lean_tools, "run_python");
  CHECK(python && python->full_terminal_output);
  CHECK(FindTool(lean_tools, "terminal_output") != nullptr);
  for (const auto& registered : ToolSchemas(lean_tools, 17)) {
    bool has_timeout =
        registered["function"]["parameters"]["properties"].contains("timeout");
    CHECK(has_timeout ==
          (registered["function"].value("name", "") != "wait_background"));
  }
  fs::remove_all(root, ec);
}

void TestPythonTool() {
  namespace fs = std::filesystem;
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
            "  if [ \"$1\" = python ]; then shift; exec python3 \"$@\"; fi\n"
            "  shift\n"
            "done\n"
            "exit 2\n")
            .starts_with("wrote "));
  CHECK(chmod(uv.c_str(), 0700) == 0);

  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  ProcessSupervisor supervisor;
  std::string result =
      ToolRunPython(supervisor, "print(6 * 7)", json::array({"numpy>=2"}), 0);
  CHECK(result == "42\n");

  fs::path marker = root / "injected";
  result = ToolRunPython(supervisor, "print('safe')",
                         json::array({"x; touch " + marker.string()}), 0);
  CHECK(result == "safe\n");
  CHECK(!fs::exists(marker));

  result = ToolRunPython(supervisor,
                         "import time; time.sleep(2); print('background-ok')",
                         json::array(), 1);
  CHECK(result.starts_with("[backgrounded]"));
  std::vector<pid_t> pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    CHECK(supervisor.JoinableCount() == 1);
    int64_t pid = pending.front();
    for (int attempt = 0;
         attempt < 3 && result.find("[exit code 0]") == std::string::npos;
         ++attempt) {
      result = ToolWaitBackground(supervisor, pid);
    }
    CHECK(result.find("background-ok") != std::string::npos);
    CHECK(result.find("[exit code 0]") != std::string::npos);
  }

  result = ToolRunPython(
      supervisor,
      "import time\nprint('ready', flush=True)\ntime.sleep(2)\n"
      "print('progress', flush=True)\ntime.sleep(2)\nprint('done')",
      json::array(), 1);
  CHECK(result.starts_with("[backgrounded]"));
  pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    int64_t pid = pending.front();
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.starts_with("[process still running — current output]"));
    CHECK(result.find("ready") != std::string::npos);
    CHECK(supervisor.PendingCount() == 1);
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.starts_with("[process still running — new output]"));
    CHECK(result.find("progress") != std::string::npos);
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.find("done") != std::string::npos);
    if (result.find("[exit code 0]") == std::string::npos) {
      result = ToolWaitBackground(supervisor, pid);
    }
    CHECK(result.find("[exit code 0]") != std::string::npos);
  }

  result = ToolRunPython(
      supervisor, "import time\nprint('Password:', flush=True)\ntime.sleep(4)",
      json::array(), 1);
  CHECK(result.starts_with("[backgrounded]"));
  pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    int64_t pid = pending.front();
    auto started = std::chrono::steady_clock::now();
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.find("Password:") != std::string::npos);
    CHECK(ElapsedMs(started) < 500);
  }

  result = ToolRunPython(supervisor, "import definitely_missing_uagent_package",
                         json::array(), 0);
  CHECK(result.starts_with("error: Python execution failed."));
  CHECK(result.find("run_python.packages") != std::string::npos);

  setenv("PATH", root.c_str(), 1);  // no uv
  result = ToolRunPython(supervisor, "print('x')", json::array({"numpy"}), 0);
  CHECK(result.find("packages require uv on PATH") != std::string::npos);
  CHECK(result.find("Install") != std::string::npos);

  if (prior_path_value) {
    setenv("PATH", prior_path.c_str(), 1);
  } else {
    unsetenv("PATH");
  }
  auto tools = BuiltinTools(supervisor, root, false);
  CHECK(FindTool(tools, "run_python") != nullptr);

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestRuntimeOwnershipHelpers() {
  UsageAccumulator accumulator;
  accumulator.Add(json{{"prompt_tokens", 5},
                       {"completion_tokens", 2},
                       {"prompt_tokens_details", {{"cache_write_tokens", 2}}}});
  accumulator.Add(Usage{3, 4, 1, 0, 0.25});
  Usage total = accumulator.Take();
  CHECK(total.input == 8);
  CHECK(total.output == 6);
  CHECK(total.cache_read == 1);
  CHECK(total.cache_write == 2);
  CHECK(total.cost == 0.25);
  CHECK(accumulator.Take().input == 0);

  setenv("UAGENT_MAX_STEPS", "0", 1);
  setenv("UAGENT_SESSION_ARCHIVE_BYTES", "-1", 1);
  setenv("UAGENT_WEB_SEARCH_MODEL", "vendor/search", 1);
  setenv("UAGENT_WEB_SEARCH_EFFORT", "low", 1);
  setenv("UAGENT_WEB_SEARCH_ENGINE", "invalid", 1);
  setenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE", "huge", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_RESULTS", "99", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_USES", "0", 1);
  setenv("UAGENT_WEB_SEARCH_SERVER", "0", 1);
  const char* checkpoint_mode = getenv("UAGENT_CHECKPOINT_MODE");
  std::string prior_checkpoint_mode = checkpoint_mode ? checkpoint_mode : "";
  unsetenv("UAGENT_CHECKPOINT_MODE");
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 1);
  CHECK(config.session_archive_bytes == 0);
  CHECK(config.checkpoint_mode == "apply");
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_effort == "low");
  CHECK(config.web_search_engine == "auto");
  CHECK(config.web_search_context_size.empty());
  CHECK(config.web_search_max_results == 25);
  CHECK(config.web_search_max_uses == 1);
  CHECK(!config.web_search_server);
  CHECK(config.DiagnosticJson().value("max_steps", int64_t{0}) ==
        config.max_steps);
  CHECK(config.DiagnosticJson().value("web_search_model", "") ==
        config.web_search_model);
  if (checkpoint_mode) {
    setenv("UAGENT_CHECKPOINT_MODE", prior_checkpoint_mode.c_str(), 1);
  }
  unsetenv("UAGENT_MAX_STEPS");
  unsetenv("UAGENT_SESSION_ARCHIVE_BYTES");
  unsetenv("UAGENT_WEB_SEARCH_MODEL");
  unsetenv("UAGENT_WEB_SEARCH_EFFORT");
  unsetenv("UAGENT_WEB_SEARCH_ENGINE");
  unsetenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE");
  unsetenv("UAGENT_WEB_SEARCH_MAX_RESULTS");
  unsetenv("UAGENT_WEB_SEARCH_MAX_USES");
  unsetenv("UAGENT_WEB_SEARCH_SERVER");

  RuntimeConfig routed;
  routed.openrouter_provider = "streamlake";
  routed.openrouter_fallbacks = true;
  Api api(routed);
  api.base_url = "https://openrouter.ai/api/v1";
  api.model = "test";
  json body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("session_id", "") == "stable-session");
  CHECK(body["provider"]["order"][0] == "streamlake");
  CHECK(body["provider"].value("allow_fallbacks", false));
  api.reasoning_effort = "low";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body["reasoning"].value("effort", "") == "low");
  CHECK(!body.contains("reasoning_effort"));
  api.base_url = "http://127.0.0.1:8080/v1";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(!body.contains("session_id"));
  CHECK(!body.contains("provider"));

  api.base_url = "https://api.openai.com/v1";
  api.reasoning_effort = "high";
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(body.value("reasoning_effort", "") == "high");
  CHECK(body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
}

void TestAgentConfigAllowlist() {
  namespace fs = std::filesystem;
  CHECK(AgentConfigKey("UAGENT_MODEL"));
  CHECK(AgentConfigKey("OPENROUTER_API_KEY"));
  CHECK(AgentConfigKey("OPENROUTER_MODEL"));
  CHECK(AgentConfigKey("OPENROUTER_EFFORT"));
  CHECK(!AgentConfigKey("OPENAI_API_KEY"));

  fs::path root =
      fs::temp_directory_path() /
      ("uagent-config-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root / ".uagent");
  CHECK(ToolWriteFile((root / ".uagent/.config").string(),
                      "secret=test-key\n"
                      "OPENROUTER_API_KEY=$secret\n"
                      "OPENROUTER_MODEL=vendor/model\n"
                      "OPENROUTER_EFFORT=high\n")
            .starts_with("wrote "));

  const char* inherited_home = getenv("HOME");
  const char* inherited_config = getenv("UAGENT_CONFIG_FILE");
  std::string prior_home = inherited_home ? inherited_home : "";
  std::string prior_config = inherited_config ? inherited_config : "";
  setenv("HOME", root.c_str(), 1);
  unsetenv("UAGENT_CONFIG_FILE");
  unsetenv("OPENROUTER_API_KEY");
  unsetenv("OPENROUTER_MODEL");
  unsetenv("OPENROUTER_EFFORT");
  LoadConfigFile(/*trust_project=*/false);
  CHECK(EnvStr("OPENROUTER_API_KEY") == "test-key");
  CHECK(EnvStr("OPENROUTER_MODEL") == "vendor/model");
  CHECK(EnvStr("OPENROUTER_EFFORT") == "high");

  unsetenv("OPENROUTER_API_KEY");
  unsetenv("OPENROUTER_MODEL");
  unsetenv("OPENROUTER_EFFORT");
  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  if (inherited_config) {
    setenv("UAGENT_CONFIG_FILE", prior_config.c_str(), 1);
  } else {
    unsetenv("UAGENT_CONFIG_FILE");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestModelPreference() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-model-preference-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  const char* inherited_home = getenv("HOME");
  std::string prior_home = inherited_home ? inherited_home : "";
  setenv("HOME", root.c_str(), 1);

  std::string error;
  CHECK(SaveModelPreference({"provider/fast", "https://example.test/v1", true},
                            error));
  ModelPreference saved = LoadModelPreference();
  CHECK(saved.selection == "provider/fast");
  CHECK(saved.base_url == "https://example.test/v1");
  CHECK(saved.route);
  struct stat st{};
  CHECK(stat(ModelPreferencePath().c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(!SaveModelPreference({"bad\nmodel", "https://example.test/v1", false},
                             error));

  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestProviderTemplates() {
  const ProviderTemplate* openrouter = FindProviderTemplate("openrouter");
  CHECK(openrouter != nullptr);
  CHECK(openrouter && openrouter->matches_url("https://openrouter.ai/api/v1"));
  CHECK(FindProviderTemplateForUrl("https://edge.openrouter.ai/v1") ==
        openrouter);

  constexpr ProviderTemplate kTestProvider = {
      "test",
      "https://provider.test/v1",
      "UAGENT_TEST_PROVIDER_KEY",
      "UAGENT_TEST_PROVIDER_MODEL",
      "UAGENT_TEST_PROVIDER_EFFORT",
      "default-model",
      +[](std::string url) { return url == "https://provider.test/v1"; },
  };
  const char* inherited_effort = getenv("UAGENT_REASONING_EFFORT");
  std::string prior_effort = inherited_effort ? inherited_effort : "";
  setenv(kTestProvider.api_key_env, "test-key", 1);
  setenv(kTestProvider.model_env, "selected-model", 1);
  setenv(kTestProvider.effort_env, "low", 1);
  unsetenv("UAGENT_REASONING_EFFORT");
  RuntimeConfig config;
  Api api(config);
  CHECK(ApplyProviderTemplate(api, kTestProvider));
  CHECK(api.base_url == kTestProvider.base_url);
  CHECK(api.api_key == "test-key");
  CHECK(api.model == "selected-model");
  CHECK(api.reasoning_effort == "low");
  unsetenv(kTestProvider.api_key_env);
  unsetenv(kTestProvider.model_env);
  unsetenv(kTestProvider.effort_env);
  if (inherited_effort) {
    setenv("UAGENT_REASONING_EFFORT", prior_effort.c_str(), 1);
  } else {
    unsetenv("UAGENT_REASONING_EFFORT");
  }
}

void TestSafeJsonValues() {
  json values = {{"boolean", "true"},
                 {"integer", "12"},
                 {"number", json::array()},
                 {"string", 7},
                 {"object", "wrong"}};
  CHECK(!JsonValue(values, "boolean", false));
  CHECK(JsonValue(values, "integer", int64_t{9}) == 9);
  CHECK(JsonValue(values, "number", 1.5) == 1.5);
  CHECK(JsonValue(values, "string", "fallback") == "fallback");
  CHECK(JsonValue(values, "object", json::object()).is_string());

  Usage usage;
  usage.Add({{"prompt_tokens", "bad"},
             {"completion_tokens", json::array()},
             {"cost", "bad"}});
  CHECK(usage.input == 0);
  CHECK(usage.output == 0);
  CHECK(usage.cost == 0);
}

void TestProjectInstructionDiscovery() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-instructions-test-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::path home = root / "home";
  fs::path repo = root / "repo";
  fs::path child = repo / "child";
  fs::path empty = child / "empty";
  fs::create_directories(home / ".uagent");
  fs::create_directories(repo / ".git");
  fs::create_directories(empty);
  CHECK(ToolWriteFile((home / ".uagent/AGENTS.md").string(), "global")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((repo / "AGENTS.md").string(), "root-agent")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((repo / "CLAUDE.md").string(), "root-claude")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((child / "AGENTS.md").string(), "ignored-agent")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((child / "AGENTS.override.md").string(), "child-override")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((child / "CLAUDE.md").string(), "child-claude")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((empty / "AGENTS.override.md").string(), " \n")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((empty / "AGENTS.md").string(), "must-not-load")
            .starts_with("wrote "));

  const char* inherited_home = getenv("HOME");
  std::string prior_home = inherited_home ? inherited_home : "";
  setenv("HOME", home.c_str(), 1);

  ProjectInstructions loaded = LoadProjectInstructions(child, 32 * 1024);
  CHECK(loaded.sources.size() == 3);  // one file per directory
  CHECK(!loaded.truncated);
  CHECK(loaded.text.find("ignored-agent") == std::string::npos);
  // CLAUDE.md is a fallback, so it must not load beside an AGENTS file
  CHECK(loaded.text.find("root-claude") == std::string::npos);
  CHECK(loaded.text.find("child-claude") == std::string::npos);
  size_t global = loaded.text.find("global");
  size_t root_agent = loaded.text.find("root-agent");
  size_t child_override = loaded.text.find("child-override");
  CHECK(global < root_agent && root_agent < child_override);

  // ...but it is used when the directory has no AGENTS file
  fs::path only_claude = repo / "claude-only";
  fs::create_directories(only_claude);
  CHECK(ToolWriteFile((only_claude / "CLAUDE.md").string(), "claude-fallback")
            .starts_with("wrote "));
  CHECK(LoadProjectInstructions(only_claude, 32 * 1024)
            .text.find("claude-fallback") != std::string::npos);

  ProjectInstructions shadowed = LoadProjectInstructions(empty, 32 * 1024);
  CHECK(shadowed.text.find("must-not-load") == std::string::npos);
  ProjectInstructions capped = LoadProjectInstructions(child, 4);
  CHECK(capped.truncated);
  CHECK(capped.text == "glob");

  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestMcpContractHelpers() {
  std::string decoded;
  CHECK(Base64Decode("aW1hZ2U=", decoded, 5));
  CHECK(decoded == "image");
  CHECK(!Base64Decode("aW1h=Z2U", decoded, 16));
  CHECK(!Base64Decode("aW1hZ2U=", decoded, 4));

  setenv("UAGENT_MCP_TEST_VALUE", "expanded", 1);
  CHECK(ExpandProcessEnv("pre-${UAGENT_MCP_TEST_VALUE}-$$") ==
        "pre-expanded-$");
  unsetenv("UAGENT_MCP_TEST_VALUE");

  json isolated = ChromeMcpConfig();
  CHECK(isolated["args"].dump().find("--isolated") != std::string::npos);
  CHECK(isolated["args"].dump().find(kChromeMcpPackage) != std::string::npos);
  json user = ChromeMcpConfig("user");
  CHECK(user["args"].dump().find("--auto-connect") != std::string::npos);
  CHECK(user["args"].dump().find("--isolated") == std::string::npos);
  setenv("UAGENT_CHROME_BROWSER_URL", "http://127.0.0.1:9222", 1);
  user = ChromeMcpConfig("user");
  CHECK(user["args"].dump().find("--browser-url") != std::string::npos);
  CHECK(user["args"].dump().find("http://127.0.0.1:9222") != std::string::npos);
  CHECK(user["args"].dump().find("--auto-connect") == std::string::npos);
  unsetenv("UAGENT_CHROME_BROWSER_URL");

  std::string error;
  CHECK(McpValidateServerConfig("test",
                                json{{"command", "server"},
                                     {"args", json::array({"one"})},
                                     {"env", {{"TOKEN", "$TOKEN"}}},
                                     {"cwd", "relative"}},
                                error));
  error.clear();
  CHECK(!McpValidateServerConfig(
      "test", json{{"command", "server"}, {"args", json::array({1})}}, error));
  CHECK(error.find("args") != std::string::npos);

  RuntimeConfig config;
  McpServer server;
  server.name = "probe";
  server.config = json{{"command", "unused"}};
  json input_schema = {
      {"$schema", "https://json-schema.org/draft/2020-12/schema"},
      {"title", "Exact"},
      {"type", "object"},
      {"properties", {{"value", {{"type", "string"}}}}},
      {"additionalProperties", false}};
  json output_schema = {{"type", "object"}, {"required", json::array({"ok"})}};
  json listed = json::array({{{"name", "echo"},
                              {"inputSchema", input_schema},
                              {"outputSchema", output_schema}},
                             {{"name", "async"},
                              {"inputSchema", json{{"type", "object"}}},
                              {"execution", {{"taskSupport", "required"}}}}});
  std::vector<Tool> tools;
  CHECK(McpReplaceServerTools(tools, server, config, listed));
  CHECK(tools.size() == 1);
  CHECK(tools[0].parameters == input_schema);
  CHECK(tools[0].output_schema == output_schema);
  CHECK(tools[0].provider == "mcp:probe");

  const char* inherited_home = getenv("HOME");
  std::string prior_home = inherited_home ? inherited_home : "";
  namespace fs = std::filesystem;
  fs::path image_home =
      fs::temp_directory_path() /
      ("uagent-mcp-image-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(image_home);
  setenv("HOME", image_home.c_str(), 1);
  json response = {
      {"result",
       {{"content", json::array({{{"type", "text"}, {"text", "plain"}},
                                 {{"type", "image"},
                                  {"data", "aW1hZ2U="},
                                  {"mimeType", "image/png"}},
                                 {{"type", "resource_link"},
                                  {"uri", "file:///tmp/value"},
                                  {"name", "value"}}})},
        {"structuredContent", {{"ok", true}}}}}};
  std::string rendered = McpResultText(server, response);
  CHECK(rendered.find("plain") != std::string::npos);
  CHECK(rendered.find("aW1hZ2U=") == std::string::npos);
  CHECK(rendered.find("[mcp image saved: ") != std::string::npos);
  size_t image_start = rendered.find("[mcp image saved: ") + 18;
  size_t image_end = rendered.find(';', image_start);
  fs::path saved_image = rendered.substr(image_start, image_end - image_start);
  CHECK(fs::exists(saved_image));
  std::ifstream image(saved_image, std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(image),
                    std::istreambuf_iterator<char>()) == "image");
  CHECK(rendered.find("file:///tmp/value") != std::string::npos);
  CHECK(rendered.find("structuredContent") != std::string::npos);
  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code remove_error;
  fs::remove_all(image_home, remove_error);
}

void TestWorkspaceScopedSession() {
  namespace fs = std::filesystem;
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-session-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  fs::path session = root / "session.json";

  RuntimeConfig config;
  Api api(config);
  api.model = "test";
  ProcessSupervisor processes;
  SideTaskSupervisor side_tasks;
  UsageAccumulator usage;
  std::vector<Tool> tools;
  Agent agent(api, tools, processes, side_tasks, usage,
              [](const Tool&, const json&) { return false; });
  g_image_input = false;
  agent.RouteChanged();
  CHECK(g_image_input.load());
  g_image_input = false;
  agent.Reset();
  CHECK(g_image_input.load());
  std::string session_id = agent.SessionId();
  std::string error;
  CHECK(agent.Save(session.string(), error));
  {
    std::ifstream saved(session);
    std::string header_line, payload_line;
    CHECK(static_cast<bool>(std::getline(saved, header_line)));
    CHECK(static_cast<bool>(std::getline(saved, payload_line)));
    json header = json::parse(header_line);
    json payload = json::parse(payload_line);
    CHECK(header.value("format", 0) == 2);
    CHECK(header.value("session_id", "") == session_id);
    CHECK(payload["archive"].is_array());
    // the clock rides on the turn, never in the cacheable prefix
    CHECK(payload["messages"][0].value("content", "").find("[now ") ==
          std::string::npos);
    CHECK(payload.value("archive_dropped_segments", int64_t{-1}) == 0);
    CHECK(payload["checkpoint_candidates"].is_array());
    CHECK(payload["pending_checkpoint"].is_null());
    CHECK(payload["side_effects"].is_array());
    CHECK(payload.value("context_tokens", int64_t{-1}) == agent.ContextUsed());
    payload["context_tokens"] = 12345;
    CHECK(ToolWritePrivateFile(session.string(),
                               header.dump() + "\n" + payload.dump())
              .starts_with("wrote "));
  }
  struct stat st{};
  CHECK(stat(session.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(agent.Load(session.string(), CanonicalCwd(), error));
  CHECK(agent.SessionId() == session_id);
  CHECK(agent.ContextUsed() == 12345);
  CHECK(!agent.Load(session.string(), root.string(), error));
  CHECK(error.find("session belongs to") != std::string::npos);

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestProjectTrustTracksSemanticConfig() {
  namespace fs = std::filesystem;
  fs::path original = fs::current_path();
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-trust-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path workspace = root / "workspace";
  fs::path home = root / "home";
  fs::create_directories(workspace);
  fs::create_directories(home);
  const char* prior_home_value = getenv("HOME");
  std::string prior_home = prior_home_value ? prior_home_value : "";
  bool had_home = prior_home_value != nullptr;
  setenv("HOME", home.c_str(), 1);
  fs::current_path(workspace);

  CHECK(ToolWriteFile(".mcp.json", R"({"mcpServers":{"x":{"command":"one"}}})")
            .starts_with("wrote "));
  std::string error;
  CHECK(TrustProjectConfig(error));
  CHECK(ProjectConfigTrusted());
  CHECK(
      ToolWriteFile(".mcp.json",
                    "{\n  \"mcpServers\": {\"x\": {\"command\": \"one\"}}\n}\n")
          .starts_with("wrote "));
  CHECK(ProjectConfigTrusted());  // formatting-only edit
  CHECK(ToolWriteFile(".mcp.json", R"({"mcpServers":{"x":{"command":"two"}}})")
            .starts_with("wrote "));
  CHECK(!ProjectConfigTrusted());

  // A project config is the second surface trust covers, on its own or beside
  // .mcp.json, and a value change in either revokes it.
  fs::remove(".mcp.json");
  fs::create_directories(".uagent");
  CHECK(!ProjectMcpPresent());
  CHECK(ProjectAgentConfigPresent() == false);
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=vendor/model\n")
            .starts_with("wrote "));
  CHECK(ProjectAgentConfigPresent());
  CHECK(ProjectConfigPresent());
  CHECK(!ProjectConfigTrusted());
  CHECK(TrustProjectConfig(error));
  CHECK(ProjectConfigTrusted());
  CHECK(
      ToolWriteFile(".uagent/.config", "# comment\nUAGENT_MODEL=vendor/model\n")
          .starts_with("wrote "));
  CHECK(ProjectConfigTrusted());  // comment-only edit
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=other/model\n")
            .starts_with("wrote "));
  CHECK(!ProjectConfigTrusted());

  fs::current_path(original);
  if (had_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestScopedBaseAndMemory() {
  namespace fs = std::filesystem;
  fs::path original = fs::current_path();
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-memory-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path workspace = root / "workspace";
  fs::path home = root / "home";
  fs::create_directories(workspace);
  fs::create_directories(home);
  const char* prior_home_value = getenv("HOME");
  std::string prior_home = prior_home_value ? prior_home_value : "";
  bool had_home = prior_home_value != nullptr;
  setenv("HOME", home.c_str(), 1);
  fs::current_path(workspace);
  workspace = fs::current_path();  // macOS resolves /var to /private/var

  // Without ./.uagent everything is global; the workspace never gets one by
  // being visited.
  CHECK(UagentBase() == GlobalBase());
  CHECK(ProjectConfigFilePath().empty());
  CHECK(!fs::exists(workspace / ".uagent"));

  // A global memory lands in the home directory even from inside a workspace.
  CHECK(ToolMemory("prefers-tabs", "global", "The user prefers tabs.")
            .starts_with("wrote "));
  CHECK(fs::is_regular_file(home / ".uagent/memory/prefers-tabs.md"));

  // Saving a project memory is what opts the workspace in, which then also
  // moves the config and uv locations.
  CHECK(
      ToolMemory("build-uses-ninja", "project", "This repo builds with ninja.")
          .starts_with("wrote "));
  fs::path project_memory = workspace / ".uagent/memory/build-uses-ninja.md";
  CHECK(fs::is_regular_file(project_memory));
  CHECK((fs::status(project_memory).permissions() & fs::perms::all) ==
        (fs::perms::owner_read | fs::perms::owner_write));
  CHECK(UagentBase() == (workspace / ".uagent").string());
  CHECK(ProjectConfigFilePath() == (workspace / ".uagent/.config").string());
  CHECK(UagentScopedDir("uv") == (workspace / ".uagent/uv").string());
  CHECK(UagentDir("history") == (home / ".uagent/history").string());

  // Names become safe file components, oversize content is refused, an existing
  // name is replaced, and empty content forgets.
  CHECK(ToolMemory("../escape", "project", "x").starts_with("wrote "));
  CHECK(fs::is_regular_file(workspace / ".uagent/memory/___escape.md"));
  CHECK(ToolMemory("", "global", "x").starts_with("error:"));
  CHECK(ToolMemory("name", "elsewhere", "x").starts_with("error:"));
  CHECK(ToolMemory("huge", "global", std::string(4096, 'x'))
            .starts_with("error:"));
  CHECK(ToolMemory("build-uses-ninja", "project", "Now it builds with make.")
            .starts_with("wrote "));
  CHECK(ToolMemory("build-uses-ninja", "project", "").starts_with("forgot "));
  CHECK(!fs::exists(project_memory));
  CHECK(ToolMemory("build-uses-ninja", "project", "").starts_with("error:"));

  // Both scopes reach the instruction rail, project after global, each labeled.
  CHECK(ToolWriteFile("AGENTS.md", "workspace rules").starts_with("wrote "));
  CHECK(
      ToolMemory("build-uses-ninja", "project", "This repo builds with ninja.")
          .starts_with("wrote "));
  ProjectInstructions loaded = LoadProjectInstructions(workspace, 32 * 1024);
  CHECK(loaded.text.find("workspace rules") != std::string::npos);
  CHECK(loaded.text.find("## memory: prefers-tabs") != std::string::npos);
  CHECK(loaded.text.find("The user prefers tabs.") != std::string::npos);
  CHECK(loaded.text.find("## memory: build-uses-ninja") != std::string::npos);
  CHECK(loaded.text.find("workspace rules") <
        loaded.text.find("## memory: prefers-tabs"));
  CHECK(loaded.text.find("## memory: prefers-tabs") <
        loaded.text.find("## memory: build-uses-ninja"));
  CHECK(!loaded.truncated);

  // A trusted project config wins key by key; the global file fills the rest.
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=project/model\n")
            .starts_with("wrote "));
  CHECK(ToolWriteFile((home / ".uagent/.config").string(),
                      "UAGENT_MODEL=global/model\nUAGENT_API_KEY=global-key\n")
            .starts_with("wrote "));
  const char* prior_config = getenv("UAGENT_CONFIG_FILE");
  std::string prior_config_value = prior_config ? prior_config : "";
  unsetenv("UAGENT_CONFIG_FILE");
  unsetenv("UAGENT_MODEL");
  unsetenv("UAGENT_API_KEY");
  LoadConfigFile(/*trust_project=*/true);
  CHECK(EnvStr("UAGENT_MODEL") == "project/model");
  CHECK(EnvStr("UAGENT_API_KEY") == "global-key");

  // Untrusted, the project file is skipped entirely.
  unsetenv("UAGENT_MODEL");
  unsetenv("UAGENT_API_KEY");
  LoadConfigFile(/*trust_project=*/false);
  CHECK(EnvStr("UAGENT_MODEL") == "global/model");

  unsetenv("UAGENT_MODEL");
  unsetenv("UAGENT_API_KEY");
  if (prior_config) setenv("UAGENT_CONFIG_FILE", prior_config_value.c_str(), 1);
  fs::current_path(original);
  if (had_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestSkillDiscovery() {
  namespace fs = std::filesystem;
  fs::path original = fs::current_path();
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-skill-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::path workspace = root / "workspace";
  fs::path home = root / "home";
  fs::create_directories(workspace);
  fs::create_directories(home);
  const char* prior_home_value = getenv("HOME");
  std::string prior_home = prior_home_value ? prior_home_value : "";
  bool had_home = prior_home_value != nullptr;
  setenv("HOME", home.c_str(), 1);
  fs::current_path(workspace);
  workspace = fs::current_path();

  auto write_skill = [](const fs::path& dir, const std::string& text) {
    std::filesystem::create_directories(dir);
    CHECK(
        ToolWriteFile((dir / "SKILL.md").string(), text).starts_with("wrote "));
  };

  CHECK(LoadSkills(workspace).empty());

  write_skill(home / ".uagent/skills/release",
              "---\nname: release\ndescription: how to cut a release\n---\n\n"
              "Run the checks, then tag.\n");
  // No front matter, so nothing to advertise: skipped rather than guessed at.
  write_skill(home / ".uagent/skills/broken", "no front matter here\n");
  std::vector<Skill> skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "release");
  CHECK(skills[0].description == "how to cut a release");
  CHECK(skills[0].dir == (home / ".uagent/skills/release").string());

  // The body arrives without its front matter and names its own directory, so
  // relative references inside it resolve.
  std::string body = ReadSkillBody(skills[0]);
  CHECK(body.find("Run the checks, then tag.") != std::string::npos);
  CHECK(body.find("description:") == std::string::npos);
  CHECK(body.find(skills[0].dir) != std::string::npos);

  // A workspace skill of the same name shadows the global one.
  write_skill(
      workspace / ".uagent/skills/release",
      "---\nname: whatever\ndescription: this repo's release steps\n---\n\n"
      "Use the makefile.\n");
  write_skill(workspace / ".uagent/skills/lint",
              "---\ndescription: how this repo lints\n---\n\nRun ruff.\n");
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 2);
  const Skill* release = nullptr;
  const Skill* lint = nullptr;
  for (const Skill& s : skills) {
    if (s.name == "release") release = &s;
    if (s.name == "lint") lint = &s;
  }
  CHECK(release != nullptr);
  CHECK(lint != nullptr);
  if (release) {
    CHECK(release->description == "this repo's release steps");
    CHECK(release->dir == (workspace / ".uagent/skills/release").string());
  }
  // The directory name is authoritative even when front matter disagrees or
  // omits it, so a skill can never claim another skill's name.
  if (lint) CHECK(lint->description == "how this repo lints");

  // Skills installed for another agent are already on the machine and use the
  // same format, so they are found too; ours wins a name collision.
  write_skill(home / ".claude/skills/vendor-only",
              "---\ndescription: from claude code\n---\n\nVendor body.\n");
  write_skill(home / ".codex/skills/release",
              "---\ndescription: codex's release steps\n---\n\nCodex body.\n");
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 3);
  for (const Skill& s : skills) {
    // The workspace copy still outranks both the user's and the vendors'.
    if (s.name == "release") {
      CHECK(s.description == "this repo's release steps");
    }
  }

  // An explicit path replaces the defaults outright, so a user can narrow the
  // catalogue to exactly what they want to pay for.
  setenv("UAGENT_SKILL_PATH", (home / ".claude/skills").c_str(), 1);
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "vendor-only");
  unsetenv("UAGENT_SKILL_PATH");

  // The cap keeps the highest-precedence entries rather than filling up on
  // user/vendor skills before the workspace is scanned.
  setenv("UAGENT_SKILLS", "1", 1);
  skills = LoadSkills(workspace);
  CHECK(skills.size() == 1);
  CHECK(skills[0].name == "release");
  CHECK(skills[0].dir == (workspace / ".uagent/skills/release").string());
  unsetenv("UAGENT_SKILLS");

  // Descriptions ride every request, so an over-long one is truncated rather
  // than dropping the skill. The cap bounds the text; the ellipsis marking the
  // cut is allowed on top of it.
  setenv("UAGENT_SKILL_DESC_BYTES", "16", 1);
  skills = LoadSkills(workspace);
  int64_t marked = 0;
  for (const Skill& s : skills) {
    CHECK(s.description.size() <= 16 + strlen("…"));
    marked += s.description.ends_with("…");
  }
  CHECK(marked > 0);  // the over-long ones say so; short ones are left alone
  unsetenv("UAGENT_SKILL_DESC_BYTES");

  // The tool advertises every skill by name and returns the one asked for.
  skills = LoadSkills(workspace);
  Tool tool = SkillTool(skills);
  CHECK(tool.name == "skill");
  CHECK(!tool.mutating);
  json names = tool.parameters["properties"]["name"]["enum"];
  CHECK(names.size() == 3);
  CHECK(tool.run({{"name", "lint"}}, {}).find("Run ruff.") !=
        std::string::npos);
  CHECK(tool.run({{"name", "nope"}}, {}).starts_with("error:"));

  fs::current_path(original);
  if (had_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace

int RunTests() {
  std::setlocale(LC_CTYPE, "");
  curl_global_init(CURL_GLOBAL_DEFAULT);
  TestTextToolProtocol();
  TestRegistries();
  TestLineNumberStripping();
  TestMarkdownMath();
  TestCapsAndEscaping();
  TestFileTools();
  TestTerminalSafety();
  TestBackgroundValidation();
  TestToolExecutionPolicy();
  TestOpenRouterServerSearch();
  TestAttachmentEncoding();
  TestGrepTool();
  TestPythonTool();
  TestRuntimeOwnershipHelpers();
  TestAgentConfigAllowlist();
  TestModelPreference();
  TestProviderTemplates();
  TestSafeJsonValues();
  TestProjectInstructionDiscovery();
  TestMcpContractHelpers();
  TestWorkspaceScopedSession();
  TestProjectTrustTracksSemanticConfig();
  TestScopedBaseAndMemory();
  TestSkillDiscovery();
  curl_global_cleanup();
  if (failures) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "all core tests passed\n";
  return 0;
}

}  // namespace uagent

int main() { return uagent::RunTests(); }
