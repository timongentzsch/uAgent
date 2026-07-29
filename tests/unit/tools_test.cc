// Copyright 2026 Timon Gentzsch

#include <string>
#include <utility>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

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
        return ToolSuccess("done");
      },
      1);
  CHECK(id > 0);
  CHECK(side_tasks.Joinable() == 1);
  CHECK(side_tasks.Start(
            "probe", "over limit",
            [](const std::atomic<bool>&) { return ToolSuccess(""); }, 1) == 0);
  CHECK(!side_tasks.Wait(id, std::chrono::milliseconds(1)).has_value());
  auto result = ToolWaitSideTask(side_tasks, id);
  CHECK(result.output.find("[Background result: probe `quick`]") !=
        std::string::npos);
  CHECK(result.output.find("done") != std::string::npos);
  CHECK(side_tasks.Joinable() == 0);

  CHECK(side_tasks.Start(
            "probe", "detached",
            [](const std::atomic<bool>&) { return ToolSuccess("done"); }, 1,
            false) > 0);
  CHECK(side_tasks.Joinable() == 0);
  side_tasks.CancelAll();

  CHECK(side_tasks.Start(
            "probe", "cancel",
            [](const std::atomic<bool>& cancel) {
              while (!cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              return ToolCancelled("cancelled");
            },
            1) > 0);
  CHECK(side_tasks.CancelAll() == 1);
  CHECK(side_tasks.Empty());

  Tool bounded_tool =
      MakeTool("bounded", "bounded", json::object(),
               [](const json&, const ToolContext&) { return ToolSuccess(""); });
  bounded_tool.max_calls_per_turn = 2;
  Tool unbounded =
      MakeTool("unbounded", "unbounded", json::object(),
               [](const json&, const ToolContext&) { return ToolSuccess(""); });
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

  ToolResult launched =
      ToolRunBash(task_processes, "sleep 0.2; printf task-done", 3, false, base,
                  true, false, "bash", true, "task");
  CHECK(launched.output.starts_with("[backgrounded] task id "));
  std::vector<pid_t> task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 1);
  if (!task_ids.empty()) {
    int64_t task_id = task_ids[0];
    CHECK(ToolGetTaskOutput(task_processes, task_id).Ok());
    ToolContext wait_context{std::chrono::steady_clock::now() +
                             std::chrono::seconds(2)};
    ToolResult waited = ToolWaitTasks(task_processes, json::array({task_id}),
                                      true, wait_context);
    CHECK(waited.output.find("task-done") != std::string::npos);
    CHECK(!task_processes.PendingCount());
  }

  launched = ToolRunBash(task_processes, "sleep 10", 3, false, base, true,
                         false, "bash", true, "task");
  task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 1);
  if (!task_ids.empty()) {
    ToolResult killed = ToolKillTask(task_processes, task_ids[0]);
    CHECK(killed.status == CompletionStatus::kCancelled);
    CHECK(killed.output.find("cancelled") != std::string::npos);
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
    ToolResult first =
        ToolWaitTasks(task_processes, json::array({task_ids[0], task_ids[1]}),
                      false, wait_context);
    CHECK(first.output.find("first") != std::string::npos);
    CHECK(task_processes.PendingCount() == 1);
    CHECK(ToolKillTask(task_processes, task_ids[1]).output.find("cancelled") !=
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

  api.base_url = "http://127.0.0.1:8787/api/v1";
  api.openrouter_web_search = true;
  body = api.BuildChatBody(json::array(), schemas);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][1]["type"] == "openrouter:web_search");

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
  CHECK(ToolGrep(supervisor, "absent", root.string(), "").output ==
        "(no matches)");
  CHECK(ToolGrep(supervisor, "(", root.string(), "").error ==
        ToolErrorCode::kProcessFailed);
  setenv("UAGENT_MAX_BACKGROUND_JOBS", "1", 1);
  CHECK(supervisor.TryAdd({999991, "", "busy", false, false, {}, ""}, 1));
  ToolResult limited = ToolGrep(supervisor, "needle", root.string(), "");
  CHECK(limited.error == ToolErrorCode::kLimitExceeded);
  CHECK(limited.output.find("background job limit") != std::string::npos);
  CHECK(supervisor.TakeAll().size() == 1);
  unsetenv("UAGENT_MAX_BACKGROUND_JOBS");

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
            .output.starts_with("wrote "));
  CHECK(chmod(uv.c_str(), 0700) == 0);

  const char* prior_path_value = getenv("PATH");
  std::string prior_path = prior_path_value ? prior_path_value : "";
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  ProcessSupervisor supervisor;
  ToolResult result =
      ToolRunPython(supervisor, "print(6 * 7)", json::array({"numpy>=2"}), 0);
  CHECK(result.output == "42\n");

  fs::path marker = root / "injected";
  result = ToolRunPython(supervisor, "print('safe')",
                         json::array({"x; touch " + marker.string()}), 0);
  CHECK(result.output == "safe\n");
  CHECK(!fs::exists(marker));

  result = ToolRunPython(supervisor,
                         "import time; time.sleep(2); print('background-ok')",
                         json::array(), 1);
  CHECK(result.output.starts_with("[backgrounded]"));
  std::vector<pid_t> pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    CHECK(supervisor.JoinableCount() == 1);
    int64_t pid = pending.front();
    for (int attempt = 0; attempt < 3 && result.output.find("[exit code 0]") ==
                                             std::string::npos;
         ++attempt) {
      result = ToolWaitBackground(supervisor, pid);
    }
    CHECK(result.output.find("background-ok") != std::string::npos);
    CHECK(result.output.find("[exit code 0]") != std::string::npos);
  }

  result = ToolRunPython(
      supervisor,
      "import time\nprint('ready', flush=True)\ntime.sleep(2)\n"
      "print('progress', flush=True)\ntime.sleep(2)\nprint('done')",
      json::array(), 1);
  CHECK(result.output.starts_with("[backgrounded]"));
  pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    int64_t pid = pending.front();
    result = ToolWaitBackground(supervisor, pid);
    CHECK(
        result.output.starts_with("[process still running — current output]"));
    CHECK(result.output.find("ready") != std::string::npos);
    CHECK(supervisor.PendingCount() == 1);
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.output.starts_with("[process still running — new output]"));
    CHECK(result.output.find("progress") != std::string::npos);
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.output.find("done") != std::string::npos);
    if (result.output.find("[exit code 0]") == std::string::npos) {
      result = ToolWaitBackground(supervisor, pid);
    }
    CHECK(result.output.find("[exit code 0]") != std::string::npos);
  }

  result = ToolRunPython(
      supervisor, "import time\nprint('Password:', flush=True)\ntime.sleep(4)",
      json::array(), 1);
  CHECK(result.output.starts_with("[backgrounded]"));
  pending = supervisor.PendingPids();
  CHECK(pending.size() == 1);
  if (!pending.empty()) {
    int64_t pid = pending.front();
    auto started = std::chrono::steady_clock::now();
    result = ToolWaitBackground(supervisor, pid);
    CHECK(result.output.find("Password:") != std::string::npos);
    CHECK(ElapsedMs(started) < 500);
  }

  result = ToolRunPython(supervisor, "import definitely_missing_uagent_package",
                         json::array(), 0);
  CHECK(result.error == ToolErrorCode::kProcessFailed);
  CHECK(result.output.starts_with("error: Python execution failed."));
  CHECK(result.output.find("run_python.packages") != std::string::npos);

  setenv("PATH", root.c_str(), 1);  // no uv
  result = ToolRunPython(supervisor, "print('x')", json::array({"numpy"}), 0);
  CHECK(result.error == ToolErrorCode::kUnavailable);
  CHECK(result.output.find("packages require uv on PATH") != std::string::npos);
  CHECK(result.output.find("Install") != std::string::npos);

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

}  // namespace uagent
