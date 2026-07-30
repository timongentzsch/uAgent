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
  CHECK(!ToolParameters(tool)["properties"].contains("timeout"));
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

  Tool checkpoint_only = unbounded;
  checkpoint_only.name = "checkpoint_only";
  checkpoint_only.visibility = Tool::Visibility::kCheckpointHint;
  Tool terminal_only = unbounded;
  terminal_only.name = "terminal_only";
  terminal_only.visibility = Tool::Visibility::kDetachedTerminal;
  policies = {unbounded, checkpoint_only, terminal_only};
  schemas = ToolSchemas(policies);
  available = AvailableToolSchemas(policies, schemas, {});
  CHECK(available.size() == 1);
  available =
      AvailableToolSchemas(policies, schemas, {}, {.checkpoint_hint = true});
  CHECK(available.size() == 2);
  CHECK(available[1]["function"]["name"] == "checkpoint_only");
  available = AvailableToolSchemas(
      policies, schemas, {},
      {.checkpoint_hint = false, .detached_terminal = true});
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
  const char* current_home = getenv("HOME");
  std::optional<std::string> saved_home =
      current_home ? std::optional<std::string>(current_home) : std::nullopt;
  setenv("HOME", log_root.c_str(), 1);
  CollectedLog large_log_result = CollectCompletedLog(large_log.string(), 16);
  if (saved_home) {
    setenv("HOME", saved_home->c_str(), 1);
  } else {
    unsetenv("HOME");
  }
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
  setenv("HOME", fallback_home.c_str(), 1);
  CollectedLog fallback_result = CollectCompletedLog(fallback_log.string(), 16);
  if (saved_home) {
    setenv("HOME", saved_home->c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  CHECK(fallback_result.artifact.has_value());
  CHECK(fallback_result.artifact &&
        fallback_result.artifact->path == fallback_log.string());
  CHECK(fs::exists(fallback_log));
  RemoveLog(fallback_log.string());
  fs::remove_all(log_root);

  ProcessSupervisor task_processes;
  BgJob task_header{7, "", "uagent -p 'very long delegated prompt'", false,
                    "task"};
  CHECK(BgResultHeader(task_header) == "[Background result: task id 7]");
  CHECK(BgResultHeader(task_header).find("delegated prompt") ==
        std::string::npos);
  ToolResult launched = ToolRunBash(task_processes, "sleep 10", base, true,
                                    false, "bash", true, "task");
  CHECK(launched.output.starts_with("[started] task id "));
  CHECK(task_processes.JoinableCount() == 1);
  std::vector<pid_t> task_ids = task_processes.PendingPids();
  CHECK(task_ids.size() == 1);
  CHECK(BgCancelTasks(task_processes) == 1);
  CHECK(!task_processes.PendingCount());
  if (!task_ids.empty()) CHECK(!ProcessAlive(task_ids[0]));
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
  CHECK(body["tools"][0]["function"]["name"] == "web_search");
  api.server_tools_authorized = true;
  body = api.BuildChatBody(json::array(), schemas);
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
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][0]["function"]["name"] == "web_search");

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
  CHECK(supervisor.TryAdd({999991, "", "busy", false, ""}, 1));
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
    CHECK(run->timeout_s == 0);
  }
  const Tool* python = FindTool(lean_tools, "run_python");
  CHECK(python != nullptr);
  CHECK(python && python->timeout_s == 0);
  CHECK(FindTool(lean_tools, "wait_background") == nullptr);
  CHECK(FindTool(lean_tools, "terminal_output") != nullptr);
  for (const auto& registered : ToolSchemas(lean_tools)) {
    CHECK(!registered["function"]["parameters"]["properties"].contains(
        "timeout"));
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
  ProcessSupervisor supervisor;
  std::vector<Tool> python_tools = BuiltinTools(supervisor, root, false);
  const Tool* run = FindTool(python_tools, "run");
  CHECK(run && ToolDescription(*run).find("omit cd") != std::string::npos);
  setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

  ToolResult result =
      ToolRunPython(supervisor, "print(6 * 7)", json::array({"numpy>=2"}));
  CHECK(result.output == "42\n");

  fs::path marker = root / "injected";
  result = ToolRunPython(supervisor, "print('safe')",
                         json::array({"x; touch " + marker.string()}));
  CHECK(result.output == "safe\n");
  CHECK(!fs::exists(marker));

  result =
      ToolRunPython(supervisor, "import time; time.sleep(.2); print('slow-ok')",
                    json::array());
  CHECK(result.output == "slow-ok\n");
  CHECK(supervisor.PendingCount() == 0);

  result = ToolRunPython(supervisor, "import definitely_missing_uagent_package",
                         json::array());
  CHECK(result.error == ToolErrorCode::kProcessFailed);
  CHECK(result.output.starts_with("error: Python execution failed."));
  CHECK(result.output.find("run_python.packages") != std::string::npos);

  setenv("PATH", root.c_str(), 1);  // no uv
  result = ToolRunPython(supervisor, "print('x')", json::array({"numpy"}));
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
