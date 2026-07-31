// Copyright 2026 Timon Gentzsch

#include <string>
#include <utility>
#include <vector>

#include "tests/unit/test_support.h"

namespace uagent {

void TestMcpContractHelpers() {
  json action_schema = {
      {"type", "object"},
      {"properties", {{"includeSnapshot", {{"type", "boolean"}}}}}};
  CHECK(McpSupportsPostActionSnapshot(action_schema));
  CHECK(!McpSupportsPostActionSnapshot(
      {{"type", "object"}, {"properties", json::object()}}));
  CHECK(McpCallArguments({{"uid", "7"}}, true)["includeSnapshot"] == true);
  CHECK(McpCallArguments(
            {{"uid", "7"}, {"includeSnapshot", false}, {"timeout", 9}}, true) ==
        json({{"uid", "7"}, {"includeSnapshot", false}, {"timeout", 9}}));
  CHECK(McpCallArguments({{"timeout", 9}}, false) == json({{"timeout", 9}}));
  McpServer custom;
  custom.config = json{{"command", "custom"}};
  CHECK(!McpDefaultsPostActionSnapshot(custom, action_schema));
  custom.config["__uagent_builtin"] = "chrome-devtools";
  CHECK(McpDefaultsPostActionSnapshot(custom, action_schema));

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
  CHECK(isolated["args"].dump().find("--slim") != std::string::npos);
  CHECK(isolated["__uagent_toolset"] == "slim");
  CHECK(isolated["args"].dump().find(kChromeMcpPackage) != std::string::npos);
  json user = ChromeMcpConfig("user");
  CHECK(user["args"].dump().find("--auto-connect") != std::string::npos);
  CHECK(user["args"].dump().find("--isolated") == std::string::npos);
  CHECK(user["args"].dump().find("--slim") != std::string::npos);
  json full = ChromeMcpConfig("isolated", "full");
  CHECK(full["args"].dump().find("--slim") == std::string::npos);
  CHECK(full["__uagent_toolset"] == "full");
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
  ToolResult mcp_result = McpResultText(server, response);
  CHECK(mcp_result.Ok());
  std::string rendered = std::move(mcp_result.output);
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
  ToolResult missing_screenshot =
      ToolSuccess((image_home / "missing.png").string());
  missing_screenshot = AttachChromeScreenshot(std::move(missing_screenshot));
  CHECK(!missing_screenshot.Ok());
  CHECK(missing_screenshot.output.find("cannot read") != std::string::npos);
  ToolResult screenshot_text =
      AttachChromeScreenshot(ToolSuccess("screenshot complete"));
  CHECK(screenshot_text.Ok());
  CHECK(screenshot_text.output == "screenshot complete");
  ToolResult error_text = McpResultText(
      server,
      {{"result",
        {{"content",
          json::array({{{"type", "text"}, {"text", "error: plain content"}}})},
         {"isError", false}}}});
  CHECK(error_text.Ok());
  CHECK(error_text.output == "error: plain content");
  ToolResult remote_error = McpResultText(
      server,
      {{"result",
        {{"content",
          json::array({{{"type", "text"}, {"text", "remote rejected call"}}})},
         {"isError", true}}}});
  CHECK(!remote_error.Ok());
  CHECK(remote_error.error == ToolErrorCode::kRemoteError);
  CHECK(remote_error.output == "error: remote rejected call");
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
  UsageAccumulator usage;
  std::vector<Tool> tools;
  Agent agent(api, tools, processes, usage,
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
    CHECK(header.value("format", 0) == 3);
    CHECK(header.value("session_id", "") == session_id);
    CHECK(payload["archive"].is_array());
    CHECK(payload["message_kinds"].is_array());
    CHECK(payload["message_kinds"].size() == payload["messages"].size());
    // Stable environment metadata follows the cacheable system prefix.
    CHECK(payload["messages"][0].value("content", "").find("[environment:") ==
          std::string::npos);
    CHECK(payload.value("archive_dropped_segments", int64_t{-1}) == 0);
    CHECK(payload["checkpoint_candidates"].is_array());
    CHECK(payload["pending_checkpoint"].is_null());
    CHECK(payload["side_effects"].is_array());
    CHECK(payload.value("context_tokens", int64_t{-1}) == agent.ContextUsed());
    payload["context_tokens"] = 12345;
    CHECK(ToolWritePrivateFile(session.string(),
                               header.dump() + "\n" + payload.dump())
              .output.starts_with("wrote "));
  }
  struct stat st{};
  CHECK(stat(session.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(agent.Load(session.string(), CanonicalCwd(), error));
  CHECK(agent.SessionId() == session_id);
  CHECK(agent.ContextUsed() == 12345);
  CHECK(!agent.Load(session.string(), root.string(), error));
  CHECK(error.find("session belongs to") != std::string::npos);

  SessionLoadResult missing =
      SessionStore::Load((root / "missing.json").string(), CanonicalCwd());
  CHECK(missing.status.error == SessionStoreError::kNotFound);
  fs::path invalid_path = root / "invalid.json";
  SessionStoreStatus invalid =
      SessionStore::Save(invalid_path.string(), SessionRecord{});
  CHECK(invalid.error == SessionStoreError::kInvalid);
  CHECK(!fs::exists(invalid_path));

  std::string corrupt =
      R"({"format":1,"cwd":")" + CanonicalCwd() +
      R"(","model":"test","session_id":"old","turns":0,"title":""})"
      "\n{}";
  CHECK(ToolWritePrivateFile(session.string(), corrupt).Ok());
  SessionLoadResult incompatible =
      SessionStore::Load(session.string(), CanonicalCwd());
  CHECK(incompatible.status.error == SessionStoreError::kIncompatible);
  error.clear();
  CHECK(!agent.Load(session.string(), CanonicalCwd(), error));
  CHECK(error.find("unsupported") != std::string::npos);
  CHECK(agent.SessionId() == session_id);
  std::ifstream preserved(session);
  CHECK(std::string(std::istreambuf_iterator<char>(preserved),
                    std::istreambuf_iterator<char>()) == corrupt);

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
            .output.starts_with("wrote "));
  std::string error;
  CHECK(TrustProjectConfig(error));
  CHECK(ProjectConfigTrusted());
  CHECK(
      ToolWriteFile(".mcp.json",
                    "{\n  \"mcpServers\": {\"x\": {\"command\": \"one\"}}\n}\n")
          .output.starts_with("wrote "));
  CHECK(ProjectConfigTrusted());  // formatting-only edit
  CHECK(ToolWriteFile(".mcp.json", R"({"mcpServers":{"x":{"command":"two"}}})")
            .output.starts_with("wrote "));
  CHECK(!ProjectConfigTrusted());

  // A project config is the second surface trust covers, on its own or beside
  // .mcp.json, and a value change in either revokes it.
  fs::remove(".mcp.json");
  fs::create_directories(".uagent");
  CHECK(!ProjectMcpPresent());
  CHECK(ProjectAgentConfigPresent() == false);
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=vendor/model\n")
            .output.starts_with("wrote "));
  CHECK(ProjectAgentConfigPresent());
  CHECK(ProjectConfigPresent());
  CHECK(!ProjectConfigTrusted());
  CHECK(TrustProjectConfig(error));
  CHECK(ProjectConfigTrusted());
  CHECK(
      ToolWriteFile(".uagent/.config", "# comment\nUAGENT_MODEL=vendor/model\n")
          .output.starts_with("wrote "));
  CHECK(ProjectConfigTrusted());  // comment-only edit
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=other/model\n")
            .output.starts_with("wrote "));
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
            .output.starts_with("wrote "));
  CHECK(fs::is_regular_file(home / ".uagent/memory/prefers-tabs.md"));

  // Saving a project memory is what opts the workspace in, which then also
  // moves the config and uv locations.
  CHECK(
      ToolMemory("build-uses-ninja", "project", "This repo builds with ninja.")
          .output.starts_with("wrote "));
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
  CHECK(ToolMemory("../escape", "project", "x").output.starts_with("wrote "));
  CHECK(fs::is_regular_file(workspace / ".uagent/memory/___escape.md"));
  CHECK(ToolMemory("", "global", "x").output.starts_with("error:"));
  CHECK(ToolMemory("name", "elsewhere", "x").output.starts_with("error:"));
  CHECK(ToolMemory("huge", "global", std::string(4096, 'x'))
            .output.starts_with("error:"));
  CHECK(ToolMemory("build-uses-ninja", "project", "Now it builds with make.")
            .output.starts_with("wrote "));
  CHECK(ToolMemory("build-uses-ninja", "project", std::nullopt, true)
            .output.starts_with("forgot "));
  CHECK(!fs::exists(project_memory));
  CHECK(ToolMemory("build-uses-ninja", "project", std::nullopt, true)
            .output.starts_with("error:"));

  // Both scopes reach the separate evidence rail, project after global, each
  // labeled; human workspace rules remain the only project instructions.
  CHECK(ToolWriteFile("AGENTS.md", "workspace rules")
            .output.starts_with("wrote "));
  CHECK(
      ToolMemory("build-uses-ninja", "project", "This repo builds with ninja.")
          .output.starts_with("wrote "));
  ProjectInstructions loaded = LoadProjectInstructions(workspace, 32 * 1024);
  CHECK(loaded.text.find("workspace rules") != std::string::npos);
  CHECK(loaded.text.find("## memory: prefers-tabs") == std::string::npos);
  CHECK(loaded.memory_index.find("global/prefers-tabs") != std::string::npos);
  CHECK(loaded.memory_index.find("The user prefers tabs.") ==
        std::string::npos);
  CHECK(loaded.memory_index.find("project/build-uses-ninja") !=
        std::string::npos);
  CHECK(loaded.memory_index.find("global/prefers-tabs") <
        loaded.memory_index.find("project/build-uses-ninja"));
  CHECK(ToolMemory("prefers-tabs", "global", std::nullopt, false)
            .output.find("The user prefers tabs.") != std::string::npos);
  CHECK(!loaded.truncated);

  // A trusted project config wins key by key; the global file fills the rest.
  CHECK(ToolWriteFile(".uagent/.config", "UAGENT_MODEL=project/model\n")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((home / ".uagent/.config").string(),
                      "UAGENT_MODEL=global/model\nUAGENT_API_KEY=global-key\n")
            .output.starts_with("wrote "));
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

}  // namespace uagent
