#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "agent.hpp"
#include "cli.hpp"
#include "mcp.hpp"
#include "media.hpp"
#include "providers.hpp"
#include "tools.hpp"
#include "util.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_text_tool_protocol() {
    auto calls = parse_text_tool_calls(
        "[uagent_tool_call]{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}}"
        "[/uagent_tool_call]");
    CHECK(calls.size() == 1);
    CHECK(calls[0].name == "read_file");
    CHECK(parse_text_tool_calls("example [uagent_tool_call]{}[/uagent_tool_call]").empty());
}

void test_registries() {
    ParsedSlashCommand command = parse_slash_command("/model vendor/model");
    CHECK(command.spec && command.spec->id == SlashCommandId::model);
    CHECK(command.argument == "vendor/model");
    CHECK(!parse_slash_command("/unknown").spec);
    CHECK(valid_effort("xhigh"));
    CHECK(!valid_effort("extreme"));

    auto models = parse_models(
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

void test_line_number_stripping() {
    CHECK(strip_line_numbers("     1\tone\n     2\ttwo\n") == "one\ntwo\n");
    CHECK(strip_line_numbers("one\n     2\ttwo\n") == "one\n     2\ttwo\n");
}

void test_caps_and_escaping() {
    std::string text = "before [uagent_tool_call] after [/uagent_tool_call]";
    std::string escaped = escape_tool_tags(text);
    CHECK(escaped.find(TT_OPEN) == std::string::npos);
    CHECK(escaped.find(TT_CLOSE) == std::string::npos);
    setenv("UAGENT_TOOL_RESULT_CHARS", "8", 1);
    std::string capped = cap_result("éééééé");
    CHECK(capped.find("\xc3\n") == std::string::npos);
    unsetenv("UAGENT_TOOL_RESULT_CHARS");
}

void test_file_tools() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() /
                    ("uagent-test-" + std::to_string(static_cast<long>(getpid())));
    fs::create_directories(root);
    fs::path file = root / "file.txt";
    CHECK(tool_write_file(file.string(), "one\ntwo\n").rfind("wrote ", 0) == 0);
    CHECK(tool_read_file(file.string(), 1, 1).find("     1\tone") != std::string::npos);
    CHECK(tool_edit_file(file.string(), "two", "three").rfind("edited ", 0) == 0);
    fs::path private_file = root / "private";
    CHECK(tool_write_private_file(private_file.string(), "secret").rfind("wrote ", 0) == 0);
    struct stat st {};
    CHECK(stat(private_file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    fs::path ledger = root / "ledger";
    std::string append_error;
    CHECK(append_private_line(ledger.string(), "one", append_error));
    CHECK(append_private_line(ledger.string(), "two", append_error));
    CHECK(stat(ledger.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(tool_read_file(ledger.string(), 1, -1).find("two") != std::string::npos);
    CHECK(path_within(canonical_access_path(file.string()), canonical_access_path(root.string())));
    CHECK(!path_within(canonical_access_path(root.parent_path().string()),
                       canonical_access_path(root.string())));
    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_terminal_safety() {
    bool prior = g_tty;
    g_tty = true;
    CHECK(terminal_safe("ok\x1b]52;bad\a") == "ok\\x1b]52;bad\\x07");
    g_tty = false;
    CHECK(terminal_safe("\x1b") == "\x1b");
    g_tty = prior;
    CHECK(display_width("ASCII") == 5);
    CHECK(display_width("界") >= 1);
    CHECK(safe_file_component("../../escape") == "______escape");
}

void test_background_validation() {
    ProcessSupervisor supervisor;
    CHECK(tool_wait_background(supervisor, 0, 1).rfind("error:", 0) == 0);
    CHECK(tool_wait_background(supervisor, 999999, 1).rfind("error:", 0) == 0);
}

void test_tool_execution_policy() {
    Tool tool;
    tool.name = "probe";
    tool.parameters = {{"type", "object"}, {"properties", json::object()}};
    json parameters = tool_parameters(tool, 17);
    CHECK(parameters["properties"]["timeout"].value("minimum", -1) == 0);
    CHECK(parameters["properties"]["timeout"].value("description", "")
              .find("default 17") != std::string::npos);
    CHECK(!tool.parameters["properties"].contains("timeout"));
    tool.timeout_s = 4;
    CHECK(tool_parameters(tool, 17)["properties"]["timeout"]
              .value("description", "")
              .find("default 4") != std::string::npos);

    ToolContext base{std::chrono::steady_clock::now() + std::chrono::seconds(30)};
    ToolContext bounded = base.with_timeout(2);
    CHECK(bounded.timeout_s == 2);
    CHECK(bounded.deadline <= base.deadline);

    SideTaskSupervisor side_tasks;
    long id = side_tasks.start(
        "probe", "quick",
        [](const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return std::string("done");
        },
        1);
    CHECK(id > 0);
    CHECK(side_tasks.joinable() == 1);
    CHECK(side_tasks.start("probe", "over limit",
                           [](const std::atomic<bool>&) { return std::string(); }, 1) == 0);
    CHECK(!side_tasks.wait(id, std::chrono::milliseconds(1)).has_value());
    auto result = side_tasks.wait(id, std::chrono::milliseconds(250));
    CHECK(result.has_value());
    if (result) CHECK(result->output == "done");
    CHECK(side_tasks.joinable() == 0);

    CHECK(side_tasks.start("probe", "detached",
                           [](const std::atomic<bool>&) { return std::string("done"); },
                           1, false) > 0);
    CHECK(side_tasks.joinable() == 0);
    side_tasks.cancel_all();

    CHECK(side_tasks.start(
              "probe", "cancel",
              [](const std::atomic<bool>& cancel) {
                  while (!cancel.load())
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  return std::string("cancelled");
              },
              1) > 0);
    CHECK(side_tasks.cancel_all() == 1);
    CHECK(side_tasks.empty());
}

void test_attachment_encoding() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() /
                    ("uagent-attachment-test-" +
                     std::to_string(static_cast<long>(getpid())));
    fs::create_directories(root);
    fs::path file = root / "tiny.txt";
    CHECK(tool_write_file(file.string(), "x").rfind("wrote ", 0) == 0);
    Attachment attachment;
    std::string error;
    CHECK(inspect_attachment(file.string(), attachment, error));
    CHECK(base64_file(attachment, 1, error, "data:text/plain;base64,") ==
          "data:text/plain;base64,eA==");
    error.clear();
    CHECK(base64_file(attachment, 0, error).empty());
    CHECK(!error.empty());

    setenv("UAGENT_IMAGE_PROTOCOL", "iterm", 1);
    CHECK(terminal_image_protocol() == TerminalImageProtocol::iterm);
    std::string iterm = iterm_image_sequence("YWJj", 3, 20, false);
    CHECK(iterm.find("\033]1337;File=inline=1") == 0);
    CHECK(iterm.find(":YWJj\a") != std::string::npos);
    std::string multipart = iterm_image_sequence("YWJj", 3, 20, true);
    CHECK(multipart.find("MultipartFile=") != std::string::npos);
    CHECK(multipart.find("FilePart=YWJj") != std::string::npos);
    CHECK(multipart.find("FileEnd") != std::string::npos);
    setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
    CHECK(terminal_image_protocol() == TerminalImageProtocol::kitty);
    std::string kitty = kitty_png_sequence("YWJj", 20);
    CHECK(kitty.find("\033_Ga=T,f=100,c=20") == 0);
    CHECK(kitty.find("YWJj\033\\") != std::string::npos);
    unsetenv("UAGENT_IMAGE_PROTOCOL");

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_grep_tool() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() /
                    ("uagent-grep-test-" +
                     std::to_string(static_cast<long>(getpid())));
    fs::create_directories(root / "source files");
    fs::path source = root / "source files" / "one.cpp";
    fs::path ignored = root / "source files" / "two.txt";
    CHECK(tool_write_file(source.string(), "needle one\nneedle two\nneedle three\n")
              .rfind("wrote ", 0) == 0);
    CHECK(tool_write_file(ignored.string(), "needle ignored\n").rfind("wrote ", 0) == 0);
    ProcessSupervisor supervisor;
    setenv("UAGENT_GREP_RESULTS", "2", 1);
    std::string result =
        tool_grep(supervisor, "needle", root.string(), "*.cpp");
    unsetenv("UAGENT_GREP_RESULTS");
    CHECK(result.find("one.cpp") != std::string::npos);
    CHECK(result.find("two.txt") == std::string::npos);
    CHECK(result.find("more available") != std::string::npos);
    CHECK(tool_grep(supervisor, "absent", root.string(), "") == "(no matches)");
    CHECK(tool_grep(supervisor, "(", root.string(), "").rfind("error:", 0) == 0);

    fs::path marker = root / "injected";
    result = tool_grep(supervisor, "needle'; touch " + marker.string() + "; '",
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
    result = tool_grep(supervisor, "needle", source.string(), "");
    CHECK(result.find("[grep") == 0);
    CHECK(result.find("needle one") != std::string::npos);
    if (prior_path_value) setenv("PATH", prior_path.c_str(), 1);
    else unsetenv("PATH");

    auto lean_tools = builtin_tools(supervisor, root, false);
    auto image_tools = builtin_tools(supervisor, root, true);
    CHECK(find_tool(lean_tools, "view_image") == nullptr);
    CHECK(find_tool(image_tools, "view_image") != nullptr);
    for (const auto& registered : tool_schemas(lean_tools, 17))
        CHECK(registered["function"]["parameters"]["properties"]
                  .contains("timeout"));
    fs::remove_all(root, ec);
}

void test_python_tool() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() /
                    ("uagent-python-test-" +
                     std::to_string(static_cast<long>(getpid())));
    fs::path bin = root / "bin";
    fs::create_directories(bin);
    fs::path uv = bin / "uv";
    CHECK(tool_write_file(
              uv.string(),
              "#!/bin/sh\n"
              "while [ \"$#\" -gt 0 ]; do\n"
              "  if [ \"$1\" = python ]; then shift; exec python3 \"$@\"; fi\n"
              "  shift\n"
              "done\n"
              "exit 2\n")
              .rfind("wrote ", 0) == 0);
    CHECK(chmod(uv.c_str(), 0700) == 0);

    const char* prior_path_value = getenv("PATH");
    std::string prior_path = prior_path_value ? prior_path_value : "";
    setenv("PATH", (bin.string() + ":" + prior_path).c_str(), 1);

    ProcessSupervisor supervisor;
    std::string result = tool_run_python(
        supervisor, "print(6 * 7)", json::array({"numpy>=2"}), 0);
    CHECK(result == "42\n");

    fs::path marker = root / "injected";
    result = tool_run_python(
        supervisor, "print('safe')",
        json::array({"x; touch " + marker.string()}), 0);
    CHECK(result == "safe\n");
    CHECK(!fs::exists(marker));

    result = tool_run_python(
        supervisor,
        "import time; time.sleep(2); print('background-ok')",
        json::array(), 1);
    CHECK(result.rfind("[backgrounded]", 0) == 0);
    CHECK(supervisor.jobs().size() == 1);
    if (!supervisor.jobs().empty()) {
        CHECK(supervisor.jobs().front().join_before_final);
        long pid = supervisor.jobs().front().pid;
        result = tool_wait_background(supervisor, pid, 5);
        CHECK(result.find("background-ok") != std::string::npos);
        CHECK(result.find("[exit code 0]") != std::string::npos);
    }

    result = tool_run_python(supervisor, "import definitely_missing_uagent_package",
                             json::array(), 0);
    CHECK(result.rfind("error: Python execution failed.", 0) == 0);
    CHECK(result.find("run_python.packages") != std::string::npos);

    setenv("PATH", root.c_str(), 1);
    result = tool_run_python(
        supervisor, "print('unreachable')", json::array(), 0);
    CHECK(result.find("uv is not on PATH") != std::string::npos);
    CHECK(result.find("Install") != std::string::npos);

    if (prior_path_value) setenv("PATH", prior_path.c_str(), 1);
    else unsetenv("PATH");
    auto tools = builtin_tools(supervisor, root, false);
    CHECK(find_tool(tools, "run_python") != nullptr);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_runtime_ownership_helpers() {
    UsageAccumulator accumulator;
    accumulator.add(
        json{{"prompt_tokens", 5},
             {"completion_tokens", 2},
             {"prompt_tokens_details", {{"cache_write_tokens", 2}}}});
    accumulator.add(Usage{3, 4, 1, 0, 0.25});
    Usage total = accumulator.take();
    CHECK(total.input == 8);
    CHECK(total.output == 6);
    CHECK(total.cache_read == 1);
    CHECK(total.cache_write == 2);
    CHECK(total.cost == 0.25);
    CHECK(accumulator.take().input == 0);

    setenv("UAGENT_MAX_STEPS", "0", 1);
    setenv("UAGENT_SESSION_ARCHIVE_BYTES", "-1", 1);
    const char* checkpoint_mode = getenv("UAGENT_CHECKPOINT_MODE");
    std::string prior_checkpoint_mode = checkpoint_mode ? checkpoint_mode : "";
    unsetenv("UAGENT_CHECKPOINT_MODE");
    RuntimeConfig config = RuntimeConfig::from_environment();
    CHECK(config.max_steps == 1);
    CHECK(config.session_archive_bytes == 0);
    CHECK(config.checkpoint_mode == "apply");
    CHECK(config.diagnostic_json().value("max_steps", 0L) == config.max_steps);
    if (checkpoint_mode)
        setenv("UAGENT_CHECKPOINT_MODE", prior_checkpoint_mode.c_str(), 1);
    unsetenv("UAGENT_MAX_STEPS");
    unsetenv("UAGENT_SESSION_ARCHIVE_BYTES");

    RuntimeConfig routed;
    routed.openrouter_provider = "streamlake";
    routed.openrouter_fallbacks = true;
    Api api(routed);
    api.base_url = "https://openrouter.ai/api/v1";
    api.model = "test";
    json body =
        api.build_chat_body(json::array(), json::array(), "stable-session");
    CHECK(body.value("session_id", "") == "stable-session");
    CHECK(body["provider"]["order"][0] == "streamlake");
    CHECK(body["provider"].value("allow_fallbacks", false));
    api.base_url = "http://127.0.0.1:8080/v1";
    body = api.build_chat_body(json::array(), json::array(), "stable-session");
    CHECK(!body.contains("session_id"));
    CHECK(!body.contains("provider"));

    api.base_url = "https://api.openai.com/v1";
    api.reasoning_effort = "high";
    body = api.build_chat_body(json::array(), json::array());
    CHECK(body.value("reasoning_effort", "") == "high");
    CHECK(body.contains("max_completion_tokens"));
    CHECK(!body.contains("max_tokens"));
}

void test_agent_config_allowlist() {
    namespace fs = std::filesystem;
    CHECK(agent_config_key("UAGENT_MODEL"));
    CHECK(agent_config_key("OPENROUTER_API_KEY"));
    CHECK(agent_config_key("OPENROUTER_MODEL"));
    CHECK(agent_config_key("OPENROUTER_EFFORT"));
    CHECK(!agent_config_key("OPENAI_API_KEY"));

    fs::path root = fs::temp_directory_path() /
                    ("uagent-config-test-" +
                     std::to_string(static_cast<long>(getpid())));
    fs::create_directories(root / ".uagent");
    CHECK(tool_write_file((root / ".uagent/.config").string(),
                          "secret=test-key\n"
                          "OPENROUTER_API_KEY=$secret\n"
                          "OPENROUTER_MODEL=vendor/model\n"
                          "OPENROUTER_EFFORT=high\n")
              .rfind("wrote ", 0) == 0);

    const char* inherited_home = getenv("HOME");
    const char* inherited_config = getenv("UAGENT_CONFIG_FILE");
    std::string prior_home = inherited_home ? inherited_home : "";
    std::string prior_config = inherited_config ? inherited_config : "";
    setenv("HOME", root.c_str(), 1);
    unsetenv("UAGENT_CONFIG_FILE");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OPENROUTER_MODEL");
    unsetenv("OPENROUTER_EFFORT");
    load_config_file();
    CHECK(env_str("OPENROUTER_API_KEY") == "test-key");
    CHECK(env_str("OPENROUTER_MODEL") == "vendor/model");
    CHECK(env_str("OPENROUTER_EFFORT") == "high");

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("OPENROUTER_MODEL");
    unsetenv("OPENROUTER_EFFORT");
    if (inherited_home) setenv("HOME", prior_home.c_str(), 1);
    else unsetenv("HOME");
    if (inherited_config) setenv("UAGENT_CONFIG_FILE", prior_config.c_str(), 1);
    else unsetenv("UAGENT_CONFIG_FILE");
    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_mcp_contract_helpers() {
    setenv("UAGENT_MCP_TEST_VALUE", "expanded", 1);
    CHECK(expand_process_env("pre-${UAGENT_MCP_TEST_VALUE}-$$") ==
          "pre-expanded-$");
    unsetenv("UAGENT_MCP_TEST_VALUE");

    json isolated = chrome_mcp_config();
    CHECK(isolated["args"].dump().find("--isolated") != std::string::npos);
    CHECK(isolated["args"].dump().find(CHROME_MCP_PACKAGE) != std::string::npos);
    json user = chrome_mcp_config("user");
    CHECK(user["args"].dump().find("--auto-connect") != std::string::npos);
    CHECK(user["args"].dump().find("--isolated") == std::string::npos);

    std::string error;
    CHECK(mcp_validate_server_config(
        "test",
        json{{"command", "server"},
             {"args", json::array({"one"})},
             {"env", {{"TOKEN", "$TOKEN"}}},
             {"cwd", "relative"}},
        error));
    error.clear();
    CHECK(!mcp_validate_server_config(
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
    json output_schema = {
        {"type", "object"}, {"required", json::array({"ok"})}};
    json listed = json::array(
        {{{"name", "echo"},
          {"inputSchema", input_schema},
          {"outputSchema", output_schema}},
         {{"name", "async"},
          {"inputSchema", json{{"type", "object"}}},
          {"execution", {{"taskSupport", "required"}}}}});
    std::vector<Tool> tools;
    CHECK(mcp_replace_server_tools(tools, server, config, listed));
    CHECK(tools.size() == 1);
    CHECK(tools[0].parameters == input_schema);
    CHECK(tools[0].output_schema == output_schema);
    CHECK(tools[0].provider == "mcp:probe");

    json response = {
        {"result",
         {{"content",
           json::array({{{"type", "text"}, {"text", "plain"}},
                        {{"type", "image"},
                         {"data", "aW1hZ2U="},
                         {"mimeType", "image/png"}},
                        {{"type", "resource_link"},
                         {"uri", "file:///tmp/value"},
                         {"name", "value"}}})},
          {"structuredContent", {{"ok", true}}}}}};
    std::string rendered = mcp_result_text(server, response);
    CHECK(rendered.find("plain") != std::string::npos);
    CHECK(rendered.find("aW1hZ2U=") != std::string::npos);
    CHECK(rendered.find("file:///tmp/value") != std::string::npos);
    CHECK(rendered.find("structuredContent") != std::string::npos);
}

void test_workspace_scoped_session() {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() /
                    ("uagent-session-test-" + std::to_string(static_cast<long>(getpid())));
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
    std::string session_id = agent.session_id();
    std::string error;
    CHECK(agent.save(session.string(), error));
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
        CHECK(payload["messages"][0].value("content", "")
                  .find("Current local date and time:") != std::string::npos);
        CHECK(payload.value("archive_dropped_segments", -1L) == 0);
        CHECK(payload["checkpoint_candidates"].is_array());
        CHECK(payload["pending_checkpoint"].is_null());
        CHECK(payload["side_effects"].is_array());
        CHECK(payload.value("context_tokens", -1L) == agent.context_used());
        payload["context_tokens"] = 12345;
        CHECK(tool_write_private_file(
                  session.string(), header.dump() + "\n" + payload.dump())
                  .rfind("wrote ", 0) == 0);
    }
    struct stat st {};
    CHECK(stat(session.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(agent.load(session.string(), canonical_cwd(), error));
    CHECK(agent.session_id() == session_id);
    CHECK(agent.context_used() == 12345);
    CHECK(!agent.load(session.string(), root.string(), error));
    CHECK(error.find("session belongs to") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_project_trust_tracks_semantic_config() {
    namespace fs = std::filesystem;
    fs::path original = fs::current_path();
    fs::path root = fs::temp_directory_path() /
                    ("uagent-trust-test-" + std::to_string(static_cast<long>(getpid())));
    fs::path workspace = root / "workspace";
    fs::path home = root / "home";
    fs::create_directories(workspace);
    fs::create_directories(home);
    const char* prior_home_value = getenv("HOME");
    std::string prior_home = prior_home_value ? prior_home_value : "";
    bool had_home = prior_home_value != nullptr;
    setenv("HOME", home.c_str(), 1);
    fs::current_path(workspace);

    CHECK(tool_write_file(".mcp.json", R"({"mcpServers":{"x":{"command":"one"}}})")
              .rfind("wrote ", 0) == 0);
    std::string error;
    CHECK(trust_project_config(error));
    CHECK(project_config_trusted());
    CHECK(tool_write_file(
              ".mcp.json",
              "{\n  \"mcpServers\": {\"x\": {\"command\": \"one\"}}\n}\n")
              .rfind("wrote ", 0) == 0);
    CHECK(project_config_trusted());  // formatting-only edit
    CHECK(tool_write_file(".mcp.json", R"({"mcpServers":{"x":{"command":"two"}}})")
              .rfind("wrote ", 0) == 0);
    CHECK(!project_config_trusted());

    fs::current_path(original);
    if (had_home) setenv("HOME", prior_home.c_str(), 1);
    else unsetenv("HOME");
    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    std::setlocale(LC_CTYPE, "");
    curl_global_init(CURL_GLOBAL_DEFAULT);
    test_text_tool_protocol();
    test_registries();
    test_line_number_stripping();
    test_caps_and_escaping();
    test_file_tools();
    test_terminal_safety();
    test_background_validation();
    test_tool_execution_policy();
    test_attachment_encoding();
    test_grep_tool();
    test_python_tool();
    test_runtime_ownership_helpers();
    test_agent_config_allowlist();
    test_mcp_contract_helpers();
    test_workspace_scoped_session();
    test_project_trust_tracks_semantic_config();
    curl_global_cleanup();
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all core tests passed\n";
}
