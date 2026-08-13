// Copyright 2026 Timon Gentzsch

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/api/stream.h"
#include "include/tools/adapt_system.h"
#include "include/tools/web_search.h"
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
  ToolResult launched = RunShellCommand(task_processes, base,
                                        {.command = "sleep 10",
                                         .background = true,
                                         .immediate = true,
                                         .job_kind = "task"})
                            .result;
  CHECK(launched.output.starts_with("[started] task id "));
  CHECK(task_processes.JoinableCount() == 1);
  std::vector<BgJob> tasks = task_processes.Snapshot();
  CHECK(tasks.size() == 1);
  std::vector<pid_t> task_ids;
  for (const BgJob& job : tasks) task_ids.push_back(job.pid);
  CHECK(BgCancelTasks(task_processes) == 1);
  CHECK(!task_processes.PendingCount());
  if (!task_ids.empty()) CHECK(!ProcessGroupAlive(task_ids[0]));
}

void TestOpenRouterServerSearch() {
  RuntimeConfig config;
  Api api(config);
  api.base_url = "https://openrouter.ai/api/v1";
  api.openrouter_compatible = true;
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
  CHECK(body["tools"][1]["parameters"]["max_total_results"] == 15);
  CHECK(!body["tools"][1]["parameters"].contains("max_uses"));
  CHECK(!body["tools"][1]["parameters"].contains("search_context_size"));

  body = api.BuildChatBody(json::array(), json::array({schemas[1]}));
  CHECK(body["tools"].size() == 1);
  CHECK(body["tools"][0]["function"]["name"] == "read_file");

  auto check_native_search = [&] {
    body = api.BuildChatBody(json::array(), schemas);
    CHECK(body["tools"].size() == 2);
    CHECK(body["tools"][0]["function"]["name"] == "web_search");
  };
  api.openrouter_web_search = false;
  check_native_search();

  api.base_url = "http://127.0.0.1:8080/v1";
  api.openrouter_compatible = false;
  check_native_search();

  api.base_url = "http://127.0.0.1:8787/api/v1";
  api.openrouter_compatible = true;
  api.openrouter_web_search = true;
  body = api.BuildChatBody(json::array(), schemas);
  CHECK(body["tools"].size() == 2);
  CHECK(body["tools"][1]["type"] == "openrouter:web_search");

  api.base_url = "https://openrouter.ai/api/v1";
  api.openrouter_compatible = true;
  api.openrouter_web_search = true;
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(!body.contains("tools"));  // compact/title requests stay tool-free

  Usage usage;
  usage.Add({{"prompt_tokens", 1},
             {"completion_tokens", 2},
             {"server_tool_use", {{"web_search_requests", 3}}}});
  CHECK(usage.web_searches == 3);
  CHECK(!usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).web_searches == 3);
  usage.Add({{"cost", 0.0}});
  CHECK(usage.cost_reported);
  CHECK(UsageFromJson(UsageJson(usage)).cost_reported);
  Usage responses_usage;
  responses_usage.Add({{"input_tokens", 11},
                       {"input_tokens_details", {{"cached_tokens", 3}}},
                       {"output_tokens", 7},
                       {"output_tokens_details", {{"reasoning_tokens", 2}}}});
  CHECK(responses_usage.input == 8);
  CHECK(responses_usage.cache_read == 3);
  CHECK(responses_usage.output == 5);
  CHECK(responses_usage.reasoning == 2);
  CHECK(responses_usage.GeneratedTokens() == 7);

  RuntimeConfig responses_config;
  responses_config.web_search_backend = "responses";
  responses_config.web_search_url = "https://search.example/v1/";
  responses_config.web_search_api_key = "search-key";
  responses_config.web_search_model = "search-model";
  Api responses_api(responses_config);
  responses_api.base_url = "https://inference.example/v1";
  responses_api.api_key = "inference-key";
  WebSearchRoute route = SelectWebSearchRoute(responses_api, {});
  CHECK(route.backend == WebSearchBackend::kResponses);
  CHECK(route.base_url == "https://search.example/v1");
  CHECK(route.api_key == "search-key");
  CHECK(route.model == "search-model");
  json search_body =
      WebSearchRequest(route, responses_config, "current information");
  CHECK(search_body["tools"][0]["type"] == "web_search");
  CHECK(search_body["include"][0] == "web_search_call.action.sources");
  CHECK(search_body["max_tool_calls"] == 3);
  CHECK(search_body["stream"] == false);

  WebSearchResult normalized = ParseResponsesSearch(
      {{"status", "completed"},
       {"output",
        json::array(
            {{{"type", "web_search_call"},
              {"action",
               {{"sources", json::array({{{"url", "https://example.com/source"},
                                          {"title", "Source"}}})}}}},
             {{"type", "message"},
              {"content",
               json::array(
                   {{{"type", "output_text"},
                     {"text", "grounded answer"},
                     {"annotations",
                      json::array(
                          {{{"type", "url_citation"},
                            {"url", "https://example.com/source"}}})}}})}}})}});
  CHECK(normalized.text == "grounded answer");
  CHECK(normalized.searches == 1);
  CHECK(CitationEntries(normalized.annotations).size() == 1);

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
  SetImageInputAvailable(false);
  CHECK(ImageInputError(image_attachment).find(image_path.string()) !=
        std::string::npos);
  setenv("UAGENT_IMAGE_MODEL", "vision-test", 1);
  CHECK(ImageInputError(image_attachment).empty());
  CHECK(std::string(ModelImageInputInstruction()).find("vision model") !=
        std::string::npos);
  unsetenv("UAGENT_IMAGE_MODEL");
  CHECK(ImageInputError(attachment).empty());
  SetImageInputAvailable(true);

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
  CHECK(messages[0]["content"][0]["text"].get<std::string>().find("withheld") ==
        std::string::npos);
  CHECK(messages[0]["content"][1].value("type", "") == "file");
  SetImageInputAvailable(false);
  CHECK(std::string(ModelImageInputInstruction())
            .find("Image input unavailable") != std::string::npos);
  SetImageInputAvailable(true);
  CHECK(std::string(ModelImageInputInstruction()).empty());

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
  CHECK(supervisor.TakeAll().size() == 1);
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
    activity_ids.push_back(job.pid);
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
  CHECK(FindTool(image_tools, "show_image") != nullptr);
  const Tool* run = FindTool(lean_tools, "run");
  CHECK(run != nullptr);
  if (run) {
    CHECK(run->parameters["properties"].contains("detach"));
    CHECK(run->parameters["properties"].contains("shell"));
    CHECK(run->timeout_s == 0);
    CHECK(static_cast<bool>(run->validate));
    CHECK(run->validate({{"command", "cmake --build build"}}).empty());
    CHECK(run->validate({{"command", "python -c 'print(1')"}})
              .find("run_python") != std::string::npos);
    CHECK(
        run->validate({{"command", "python3 script.py"}}).find("run_python") !=
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
                   .tool_allowlist = {"grep", "read_file", "list_dir", "run"},
                   .run_allowlist = {"python3 slow_analysis.py"},
                   .error = ""});
  const Tool* evaluator_run = FindTool(evaluator_tools, "run");
  CHECK(evaluator_run &&
        evaluator_run->validate({{"command", "python3 slow_analysis.py"}})
            .empty());
  const Tool* python = FindTool(lean_tools, "run_python");
  CHECK(python != nullptr);
  CHECK(python &&
        ToolDescription(*python).find("Never use this") != std::string::npos);
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
  CHECK(FindTool(lean_tools, "activity_output") != nullptr);
  const Tool* activity = FindTool(lean_tools, "activity_output");
  CHECK(activity && activity->parameters["properties"].contains("until"));
  CHECK(activity &&
        !activity->validate({{"id", 1}, {"wait_ms", 1000}, {"until", "ready"}})
             .size());
  CHECK(activity &&
        activity->validate({{"until", "ready"}})
                .find("requires id and wait_ms") != std::string::npos);
  const Tool* activity_wait = FindTool(lean_tools, "activity_wait");
  CHECK(activity_wait != nullptr);
  CHECK(activity_wait &&
        activity_wait->parameters["properties"]["mode"]["enum"] ==
            json::array({"any", "all"}));
  CHECK(activity_wait &&
        InvalidToolArgument(*activity_wait, {{"ids", {1, "bad"}}}) ==
            "`ids[1]` must be integer");
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
  CHECK(FindTool(tools, "run_python") != nullptr);

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
