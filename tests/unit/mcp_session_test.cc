// Copyright 2026 Timon Gentzsch

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/tools/memory.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestMcpContractHelpers() {
  json action_schema = {
      {"type", "object"},
      {"properties", {{"includeSnapshot", {{"type", "boolean"}}}}}};
  CHECK(McpSupportsPostActionSnapshot(action_schema));
  CHECK(!McpSupportsPostActionSnapshot(
      {{"type", "object"}, {"properties", json::object()}}));
  CHECK(McpCallArguments({{"uid", "7"}}, action_schema,
                         true)["includeSnapshot"] == true);
  CHECK(McpCallArguments(
            {{"uid", "7"}, {"includeSnapshot", false}, {"timeout", 9}},
            action_schema, true) ==
        json({{"uid", "7"}, {"includeSnapshot", false}, {"timeout", 9}}));
  CHECK(McpCallArguments({{"timeout", 9}}, action_schema, false) ==
        json({{"timeout", 9}}));
  json optional_path_schema = {{"type", "object"},
                               {"properties",
                                {{"filePath", {{"type", "string"}}},
                                 {"requiredPath", {{"type", "string"}}}}},
                               {"required", json::array({"requiredPath"})}};
  CHECK(McpCallArguments({{"filePath", ""}, {"requiredPath", ""}},
                         optional_path_schema,
                         false) == json({{"requiredPath", ""}}));
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
  error.clear();
  CHECK(!McpValidateServerConfig(
      "test", json{{"command", "server"}, {"roots", json::array({""})}},
      error));
  CHECK(error.find("roots") != std::string::npos);

  std::filesystem::path root_fixture =
      std::filesystem::temp_directory_path() /
      ("uagent-mcp-root-" + std::to_string(static_cast<int64_t>(getpid())));
  std::filesystem::path configured_root = root_fixture / "root with space";
  std::filesystem::create_directories(configured_root);
  McpRootSet roots;
  error.clear();
  CHECK(McpResolveRoots({{"command", "server"},
                         {"roots", json::array({"root with space"})},
                         {"__uagent_config_dir", root_fixture.string()}},
                        "", roots, error));
  CHECK(roots.entries.size() == 1);
  CHECK(roots.entries[0]["uri"].get<std::string>().find(
            "root%20with%20space") != std::string::npos);
  CHECK(roots.Contains((configured_root / "image.png").string()));
  CHECK(!roots.Contains((root_fixture / "outside.png").string()));
  McpRootSet global_roots;
  CHECK(McpResolveRoots({{"command", "server"}}, configured_root.string(),
                        global_roots, error));
  CHECK(global_roots.paths ==
        std::vector<std::filesystem::path>({configured_root}));
  CHECK(ChromeTemporaryArtifact((std::filesystem::temp_directory_path() /
                                 "chrome-devtools-mcp-test" / "screenshot.png")
                                    .string()));
  CHECK(!ChromeTemporaryArtifact(
      (std::filesystem::temp_directory_path() / "screenshot.png").string()));
  unsetenv("UAGENT_MCP_MISSING_ROOT");
  McpRootSet invalid_roots;
  CHECK(!McpResolveRoots({{"command", "server"},
                          {"roots", json::array({"$UAGENT_MCP_MISSING_ROOT"})},
                          {"__uagent_config_dir", root_fixture.string()}},
                         "", invalid_roots, error));
  CHECK(error.find("expanded to empty") != std::string::npos);
  CHECK(McpFileUri(std::filesystem::path("/tmp/\xc3\xa9")) ==
        "file:///tmp/%C3%A9");
  McpServer rooted;
  rooted.roots = roots;
  json root_reply = McpClientRequestReply(
      rooted, {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "roots/list"}});
  CHECK(root_reply["id"] == 7);
  CHECK(root_reply["result"]["roots"] == roots.entries);
  CHECK(McpClientRequestReply(
            rooted, {{"jsonrpc", "2.0"},
                     {"id", 8},
                     {"method", "unknown"}})["error"]["code"] == -32601);
  std::error_code root_cleanup_error;
  std::filesystem::remove_all(root_fixture, root_cleanup_error);

  RuntimeConfig config;
  McpRuntime diagnostic_runtime;
  auto diagnostic_server = std::make_unique<McpServer>();
  diagnostic_server->name = "diagnostic";
  diagnostic_server->config = json{{"command", "unused"}};
  diagnostic_server->last_error = "handshake failed";
  diagnostic_runtime.Add(std::move(diagnostic_server));
  std::vector<Tool> diagnostic_tools;
  McpAddControlTools(diagnostic_tools, diagnostic_runtime, config);
  const Tool* mcp_status = FindTool(diagnostic_tools, "mcp_status");
  CHECK(mcp_status != nullptr);
  ToolResult diagnostic = mcp_status->run(json::object(), {});
  CHECK(diagnostic.Ok());
  CHECK(diagnostic.output.find("diagnostic · error") != std::string::npos);
  CHECK(diagnostic.output.find("handshake failed") != std::string::npos);
  CHECK(FindTool(diagnostic_tools, "mcp_restart") != nullptr);

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
  server.roots.paths = {image_home};
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
  missing_screenshot =
      AttachChromeScreenshot(server, std::move(missing_screenshot));
  CHECK(!missing_screenshot.Ok());
  CHECK(missing_screenshot.output.find("cannot read") != std::string::npos);
  ToolResult outside_screenshot = AttachChromeScreenshot(
      server, ToolSuccess((image_home.parent_path() / "outside.png").string()));
  CHECK(!outside_screenshot.Ok());
  CHECK(outside_screenshot.error == ToolErrorCode::kPermissionDenied);
  ToolResult screenshot_text =
      AttachChromeScreenshot(server, ToolSuccess("screenshot complete"));
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
  ToolResult empty_remote_error = McpResultText(
      server, {{"result", {{"content", json::array()}, {"isError", true}}}});
  CHECK(!empty_remote_error.Ok());
  CHECK(empty_remote_error.output ==
        "error: mcp(probe) returned isError without diagnostic text");
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
  SetImageInputAvailable(false);
  agent.RouteChanged();
  CHECK(ImageInputAvailable());
  SetImageInputAvailable(false);
  agent.Reset();
  CHECK(ImageInputAvailable());
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
    // Runtime metadata remains separate from the cacheable system prefix.
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
  TestWorkspace test("trust");

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
}

void TestScopedBaseAndMemory() {
  namespace fs = std::filesystem;
  TestWorkspace test("memory");
  const fs::path& root = test.root;
  const fs::path& workspace = test.workspace;
  const fs::path& home = test.home;

  // Without ./.uagent everything is global; the workspace never gets one by
  // being visited.
  CHECK(ProjectConfigFilePath() == (workspace / ".uagent/.config").string());
  CHECK(!fs::exists(workspace / ".uagent"));

  // A global memory lands in the home directory even from inside a workspace.
  CHECK(ToolMemoryAction("set", "global/prefers-tabs",
                         std::string("The user prefers tabs."))
            .output.starts_with("wrote "));
  CHECK(fs::is_regular_file(home / ".uagent/memory/global/prefers-tabs.md"));

  // Project memory is private user state; visiting a workspace never mutates
  // the repository.
  CHECK(ToolMemoryAction("set", "project/build-uses-ninja",
                         std::string("This repo builds with ninja."))
            .output.starts_with("wrote "));
  std::vector<MemoryEntry> initial_memories = ListMemories();
  auto project_entry =
      std::find_if(initial_memories.begin(), initial_memories.end(),
                   [](const MemoryEntry& memory) {
                     return memory.key == "project/build-uses-ninja";
                   });
  CHECK(project_entry != initial_memories.end());
  fs::path project_memory;
  if (project_entry != initial_memories.end()) {
    project_memory = project_entry->path;
    CHECK(fs::is_regular_file(project_memory));
    CHECK(project_memory.string().starts_with(
        (home / ".uagent/memory/projects").string()));
    CHECK((fs::status(project_memory.parent_path()).permissions() &
           fs::perms::all) == fs::perms::owner_all);
  }
  CHECK(!fs::exists(workspace / ".uagent"));
  if (!project_memory.empty()) {
    CHECK((fs::status(project_memory).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write));
  }
  CHECK(ProjectConfigFilePath() == (workspace / ".uagent/.config").string());
  CHECK(UagentDir("history") == (home / ".uagent/history").string());

  // Keys are scoped and flat, oversize content is refused, an existing key is
  // replaced, and forget is explicit.
  CHECK(ToolMemoryAction("set", "project/../escape", std::string("x"))
            .output.starts_with("error:"));
  CHECK(ToolMemoryAction("set", "global/", std::string("x"))
            .output.starts_with("error:"));
  CHECK(ToolMemoryAction("set", "elsewhere/name", std::string("x"))
            .output.starts_with("error:"));
  CHECK(ToolMemoryAction("set", "global/huge", std::string(4096, 'x'))
            .output.starts_with("error:"));
  CHECK(ToolMemoryAction("set", "project/build-uses-ninja",
                         std::string("Now it builds with make."))
            .output.starts_with("wrote "));
  CHECK(ToolMemoryAction("forget", "project/build-uses-ninja", std::nullopt)
            .output.starts_with("forgot "));
  CHECK(!fs::exists(project_memory));
  CHECK(ToolMemoryAction("forget", "project/build-uses-ninja", std::nullopt)
            .output.starts_with("error:"));

  // Both scopes reach the separate evidence rail, project after global, each
  // labeled; human workspace rules remain the only project instructions.
  CHECK(ToolWriteFile("AGENTS.md", "workspace rules")
            .output.starts_with("wrote "));
  CHECK(ToolMemoryAction("set", "project/build-uses-ninja",
                         std::string("This repo builds with ninja."))
            .output.starts_with("wrote "));
  ProjectInstructions loaded = LoadProjectInstructions(workspace, 32 * 1024);
  MemoryIndex index =
      LoadMemoryIndex(workspace, 32 * 1024 - loaded.text.size());
  loaded.memory_index = index.text;
  loaded.memory_sources = index.sources;
  loaded.truncated |= index.truncated;
  CHECK(loaded.text.find("workspace rules") != std::string::npos);
  CHECK(loaded.text.find("## memory: prefers-tabs") == std::string::npos);
  CHECK(loaded.memory_index.find("global/prefers-tabs") != std::string::npos);
  CHECK(loaded.memory_index.find("The user prefers tabs.") ==
        std::string::npos);
  CHECK(loaded.memory_index.find("project/build-uses-ninja") !=
        std::string::npos);
  CHECK(loaded.memory_index.find("global/prefers-tabs") <
        loaded.memory_index.find("project/build-uses-ninja"));
  CHECK(ToolMemoryAction("get", "global/prefers-tabs", std::nullopt)
            .output.find("The user prefers tabs.") != std::string::npos);
  CHECK(ToolMemoryAction("get", "global/prefers-tabs", std::string())
            .output.find("The user prefers tabs.") != std::string::npos);
  CHECK(ToolMemoryAction("set", "project/explicit", std::string("durable fact"))
            .output.starts_with("wrote "));
  CHECK(ToolMemoryAction("set", "project/blocked", std::string("fact"), false)
            .error == ToolErrorCode::kPermissionDenied);
  std::vector<MemoryEntry> memories = ListMemories();
  CHECK(std::any_of(memories.begin(), memories.end(), [](const MemoryEntry& m) {
    return m.key == "global/prefers-tabs";
  }));
  CHECK(std::any_of(memories.begin(), memories.end(), [](const MemoryEntry& m) {
    return m.key == "project/explicit";
  }));
  CHECK(ToolMemoryAction("forget", "project/explicit", std::nullopt)
            .output.starts_with("forgot "));
  CHECK(ToolMemoryAction("set", "project/missing", std::nullopt)
            .output.starts_with("error:"));
  CHECK(ToolMemoryAction("get", "invalid", std::nullopt)
            .output.starts_with("error:"));
  CHECK(!loaded.truncated);

  // Linked worktrees resolve through Git's common directory and therefore
  // share one project memory scope.
  fs::path main_repo = root / "main-repo";
  fs::path linked_repo = root / "linked-repo";
  fs::path linked_git_dir = main_repo / ".git/worktrees/linked-repo";
  fs::create_directories(linked_git_dir);
  fs::create_directories(linked_repo);
  CHECK(ToolWriteFile((linked_git_dir / "commondir").string(), "../..\n")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((linked_repo / ".git").string(),
                      "gitdir: " + linked_git_dir.string() + "\n")
            .output.starts_with("wrote "));
  fs::current_path(main_repo);
  CHECK(ToolMemoryAction("set", "project/shared-worktree", "shared fact")
            .output.starts_with("wrote "));
  fs::current_path(linked_repo);
  CHECK(ToolMemoryAction("get", "project/shared-worktree", std::nullopt)
            .output.find("shared fact") != std::string::npos);
  CHECK(ToolMemoryAction("forget", "project/shared-worktree", std::nullopt)
            .output.starts_with("forgot "));
  fs::current_path(workspace);

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
}

}  // namespace uagent
