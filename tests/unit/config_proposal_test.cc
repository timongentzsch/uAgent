// Copyright 2026 Timon Gentzsch

#include "include/app/config_proposal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/app/self_description.h"
#include "include/app/uagent_tool.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "include/core/settings_store.h"
#include "include/mcp/server.h"
#include "include/providers/providers.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {
// What a uagent tool reads of the build, for a test that does not look.
json Describes(SelfTopic, const std::string&) { return json::object(); }
// The settings side of a tool whose test changes none.
ConfigProposal Proposes(ConfigProposalScope, const std::vector<ConfigChange>&) {
  return {};
}
}  // namespace

TEST_CASE(TestConfigProposalAndCommit) {
  TestWorkspace test("config-proposal");
  CHECK(SaveSettings(
            {{"UAGENT_MAX_TOOL_CALLS", "40"},
             {"QWEN_GPU_API_KEY", "adjacent-provider-secret"},
             {"UAGENT_PROVIDERS",
              R"({"old":{"base_url":"https://old.example/v1",)"
              R"("api_key":"existing-provider-secret"},"gpu":{"base_url":)"
              R"("https://gpu.example/v1","api_key":"$QWEN_GPU_API_KEY"}})"}})
            .empty());

  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ScopedEnv no_providers("UAGENT_PROVIDERS");
  ConfigManager manager = ConfigManager::Capture({});
  RuntimeConfig active = manager.Initialize();

  // Unknown keys are rejected before anything is read or written.
  ConfigProposal unknown =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"UAGENT_NOT_A_SETTING", "1", false}}, manager);
  CHECK(!unknown.ok);
  CHECK(unknown.error.find("unknown setting") != std::string::npos);
  // A fixed-choice setting refuses a spelling it would otherwise ignore.
  ConfigProposal choice = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_WEB_SEARCH_BACKEND", "foo", false}},
      manager);
  CHECK(!choice.ok);
  CHECK(choice.error.find("auto off") != std::string::npos);

  // A person may type a provider's key itself; no preview shows it.
  auto human_secret =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"UAGENT_PROVIDERS",
                              R"({"old":{"base_url":"https://old.example/v1",)"
                              R"("api_key":"human-secret-replacement"}})",
                              false}},
                            manager, /*direct_user=*/true);
  CHECK(human_secret.ok);
  CHECK(human_secret.Preview().find("human-secret-replacement") ==
        std::string::npos);
  auto invalid_human_provider = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", "[]", false}}, manager,
      /*direct_user=*/true);
  CHECK(!invalid_human_provider.ok);

  // A boolean takes any documented spelling and is stored in one of them.
  ConfigProposal boolean = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MEMORY", "off", false}}, manager);
  CHECK(boolean.ok);
  CHECK(boolean.written.at("UAGENT_MEMORY") == "0");
  ConfigProposal bad_boolean = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MEMORY", "maybe", false}}, manager);
  CHECK(!bad_boolean.ok);

  // Composite credentials may be named only by an exact environment reference.
  const std::string providers =
      R"({"codex-local":{"base_url":"http://127.0.0.1:8787/openai/v1","api_key":"$CODEX_LOCAL_PROXY_API_KEY","wire_api":"responses","hosted_tools":["web_search"]}})";
  ConfigProposal composite =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"UAGENT_PROVIDERS", providers, false}}, manager);
  CHECK(composite.ok);
  CHECK(composite.Preview().find("$CODEX_LOCAL_PROXY_API_KEY") !=
        std::string::npos);
  CHECK(composite.Preview().find("existing-provider-secret") ==
        std::string::npos);
  CHECK(composite.Preview().find("adjacent-provider-secret") ==
        std::string::npos);
  CHECK(composite.Preview().find("configured: {") == std::string::npos);
  CHECK(composite.Preview().find("\"wire_api\": \"responses\"") !=
        std::string::npos);
  // A tool names only the providers it changes: the others stay, with keys
  // it was never shown.
  CHECK(composite.Preview().find("-   \"gpu\":") == std::string::npos);
  CHECK(composite.Preview().find("+   \"codex-local\":") != std::string::npos);

  // The accepted reference resolves before provider selection.
  std::string error;
  CHECK(CommitConfigProposal(composite, error));
  json merged = json::parse(HeldSettings("").at("UAGENT_PROVIDERS"));
  CHECK(merged["old"]["api_key"] == "existing-provider-secret");
  CHECK(merged.contains("gpu") && merged.contains("codex-local"));
  // Null removes one.
  ConfigProposal removal = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"UAGENT_PROVIDERS", R"({"old":null})", false}}, manager);
  CHECK(removal.ok);
  CHECK(removal.Preview().find("-   \"old\":") != std::string::npos);
  CHECK(removal.Preview().find("<redacted>") != std::string::npos);
  CHECK(removal.Preview().find("existing-provider-secret") ==
        std::string::npos);
  CHECK(CommitConfigProposal(removal, error));
  merged = json::parse(HeldSettings("").at("UAGENT_PROVIDERS"));
  CHECK(!merged.contains("old") && merged.contains("gpu"));
  ScopedEnv provider_key("CODEX_LOCAL_PROXY_API_KEY", "resolved-local-key");
  unsetenv("UAGENT_PROVIDERS");
  ConfigManager resolved_manager = ConfigManager::Capture({});
  resolved_manager.Initialize();
  ProviderCatalog catalog = LoadProviderCatalog();
  const Endpoint* resolved = FindEndpoint(catalog.providers, "codex-local");
  CHECK(resolved && resolved->api_key == "resolved-local-key");

  for (const std::string credential : {"literal-secret", "Bearer secret",
                                       "prefix-$API_KEY", "$API_KEY-suffix"}) {
    const std::string unsafe =
        "{\"bad\":{\"base_url\":\"https://example.com/v1\",\"api_key\":\"" +
        credential + "\"}}";
    ConfigProposal rejected =
        PrepareConfigProposal(ConfigProposalScope::kUser,
                              {{"UAGENT_PROVIDERS", unsafe, false}}, manager);
    CHECK(!rejected.ok);
    CHECK(rejected.error.find("environment-variable reference") !=
          std::string::npos);
  }

  for (const std::string url : {"https://user:secret@example.com/v1",
                                "https://example.com/v1?api_key=secret",
                                "https://example.com/v1#secret"}) {
    const std::string unsafe =
        "{\"bad\":{\"base_url\":\"" + url + "\",\"api_key\":\"$API_KEY\"}}";
    ConfigProposal rejected =
        PrepareConfigProposal(ConfigProposalScope::kUser,
                              {{"UAGENT_PROVIDERS", unsafe, false}}, manager);
    CHECK(!rejected.ok);
    CHECK(rejected.error.find("without credentials") != std::string::npos);
  }

  // Out-of-range values are refused against the registry's own bounds.
  ConfigProposal invalid =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"UAGENT_MAX_TOOL_CALLS", "-5", false}}, manager);
  CHECK(!invalid.ok);

  ConfigProposal proposal =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"UAGENT_MAX_TOOL_CALLS", "120", false}}, manager);
  CHECK(proposal.ok);
  CHECK(proposal.effects.size() == 1);
  CHECK(proposal.effects[0].effect == ConfigEffect::kActiveNextUserTurn);
  // Preparing saves nothing.
  CHECK(ReadSettings("").all.at("UAGENT_MAX_TOOL_CALLS") == "40");
  // A secret is no part of the preview.
  CHECK(proposal.Preview().find("adjacent-provider-secret") ==
        std::string::npos);

  CHECK(CommitConfigProposal(proposal, error));
  SettingValues saved = ReadSettings("").all;
  CHECK(saved.at("UAGENT_MAX_TOOL_CALLS") == "120");
  CHECK(saved.at("QWEN_GPU_API_KEY") == "adjacent-provider-secret");

  // Replaying the same approved proposal is refused: what it replaces is no
  // longer there.
  CHECK(!CommitConfigProposal(proposal, error));
  CHECK(error.find("changed after the preview") != std::string::npos);

  // A change to another setting between preview and commit is kept beside it;
  // one to the same setting is never overwritten.
  auto set = [](const char* key, const char* value) {
    return ChangeSettings("", [&](SettingValues& scope) {
      scope[key] = value;
      return std::string();
    });
  };
  ConfigProposal beside = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "7", false}}, manager);
  CHECK(beside.ok && set("UAGENT_TOOL_TIMEOUT", "99").empty());
  CHECK(CommitConfigProposal(beside, error));
  saved = ReadSettings("").all;
  CHECK(saved.at("UAGENT_MAX_STEPS") == "7");
  CHECK(saved.at("UAGENT_TOOL_TIMEOUT") == "99");
  ConfigProposal racing = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "8", false}}, manager);
  CHECK(racing.ok && set("UAGENT_MAX_STEPS", "3").empty());
  CHECK(!CommitConfigProposal(racing, error));
  CHECK(ReadSettings("").all.at("UAGENT_MAX_STEPS") == "3");

  // An expired proposal cannot commit.
  ConfigProposal expired = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "9", false}}, manager);
  CHECK(expired.ok);
  expired.expires = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  CHECK(!CommitConfigProposal(expired, error));
  CHECK(error.find("expired") != std::string::npos);

  // A value typed into the document that its setting does not take, and
  // one spelled another way, are replaced like any other: the preview and
  // the save compare with what the document holds.
  CHECK(ChangeSettings("", [](SettingValues& scope) {
          scope["UAGENT_MAX_STEPS"] = "many";
          scope["UAGENT_SANDBOX"] = "off";
          return std::string();
        }).empty());
  ConfigProposal repair = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"UAGENT_MAX_STEPS", "11", false}, {"UAGENT_SANDBOX", "default", false}},
      manager);
  CHECK(repair.ok);
  CHECK(CommitConfigProposal(repair, error));
  CHECK(ReadSettings("").all.at("UAGENT_MAX_STEPS") == "11");
  CHECK(ReadSettings("").warning.empty());

  // The screen shows providers with their keys hidden and sends back what it
  // was shown: a key left hidden is the one held, and one with nothing
  // behind it is asked for.
  CHECK(ChangeSettings("", [](SettingValues& scope) {
          scope["UAGENT_PROVIDERS"] =
              R"({"old":{"base_url":"https://old.example/v1",)"
              R"("api_key":"existing-provider-secret"}})";
          return std::string();
        }).empty());
  const std::string shown =
      R"({"old":{"base_url":"https://new.example/v1","api_key":"<redacted>"}})";
  ConfigProposal edited = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", shown, false}}, manager,
      /*direct_user=*/true);
  CHECK(edited.ok);
  CHECK(CommitConfigProposal(edited, error));
  const json kept = json::parse(HeldSettings("").at("UAGENT_PROVIDERS"));
  CHECK(kept["old"]["api_key"] == "existing-provider-secret");
  CHECK(kept["old"]["base_url"] == "https://new.example/v1");
  // Renamed on the screen, a provider keeps its hidden key; the note naming
  // the old name is not stored.
  ConfigProposal renamed = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"UAGENT_PROVIDERS",
        R"({"lab":{"base_url":"https://new.example/v1",)"
        R"("api_key":"<redacted>","renamed_from":"old"}})",
        false}},
      manager, /*direct_user=*/true);
  CHECK(renamed.ok);
  CHECK(CommitConfigProposal(renamed, error));
  const json moved = json::parse(HeldSettings("").at("UAGENT_PROVIDERS"));
  CHECK(moved["lab"]["api_key"] == "existing-provider-secret");
  CHECK(!moved["lab"].contains("renamed_from"));
  CHECK(!moved.contains("old"));
  ConfigProposal keyless = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"UAGENT_PROVIDERS",
        R"({"new":{"base_url":"https://n.example/v1","api_key":"<redacted>"}})",
        false}},
      manager, /*direct_user=*/true);
  CHECK(!keyless.ok);
  CHECK(keyless.error.find("enter the key for provider new") !=
        std::string::npos);

  // The store hands a proposal out once, and only for the arguments it was
  // prepared from.
  ApprovedProposals<ConfigProposal> store;
  ConfigProposal stored = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "9", false}}, manager);
  CHECK(stored.ok);
  const json arguments = {{"scope", "user"}, {"key", "UAGENT_MAX_STEPS"}};
  store.Put(arguments, stored, stored.expires);
  CHECK(!store.Take(json{{"scope", "user"}}));  // different request
  store.Put(arguments, stored, stored.expires);
  CHECK(store.Take(arguments).has_value());
  CHECK(!store.Take(arguments));
  store.Put(arguments, stored,
            std::chrono::steady_clock::now() - std::chrono::seconds(1));
  CHECK(!store.Take(arguments));  // expired

  // Showing needs no one and survives restricted policies. With nobody to
  // approve, the schema offers nothing else.
  const Tool shows = UagentTool(Describes);
  const json config = {{"action", "show"}, {"topic", "config"}};
  const json reset = {{"action", "write"}, {"file", "style"}, {"text", ""}};
  CHECK(RequiredApproval(shows, config) == ApprovalClass::kNone);
  CHECK(shows.run(config, {}).Ok());
  CHECK(shows.capabilities == 0 && shows.parallel_safe);
  CHECK(FindToolArgumentIssue(shows, reset).has_value());
  CHECK(FindToolArgumentIssue(shows, {{"action", "set"}}).has_value());
  // Where a person can approve, every change needs one, whatever it asks.
  const Tool tool = UagentTool(
      Describes, [](ConfigProposalScope, const std::vector<ConfigChange>&) {
        ConfigProposal rejected;
        rejected.error = "specific rejection";
        return rejected;
      });
  CHECK(RequiredApproval(tool, config) == ApprovalClass::kNone);
  CHECK(!FindToolArgumentIssue(tool, reset) && !tool.validate(reset));
  for (const char* action : {"set", "write", "anything"}) {
    CHECK(RequiredApproval(tool, {{"action", action}}) ==
          ApprovalClass::kMandatoryHuman);
  }
  auto issue = tool.validate({{"action", "set"},
                              {"scope", "user"},
                              {"changes",
                               {{{"key", "UAGENT_MAX_STEPS"},
                                 {"operation", "set"},
                                 {"value", "7"}}}}});
  CHECK(issue && issue->code == "config.rejected");
  CHECK(issue && issue->field == "changes");
  CHECK(issue && issue->message == "specific rejection");
}

// A write runs only as the preview a person approved, once, and never over
// a file that changed while they read it.
TEST_CASE(TestUagentWritesWhatWasApproved) {
  TestWorkspace test("uagent-write");
  const Tool tool = UagentTool(Describes, Proposes);
  const std::string skill = "---\ndescription: Release notes\n---\nBody\n";
  const std::filesystem::path config = test.home / ".uagent/config";
  const std::vector<std::pair<json, std::filesystem::path>> written = {
      {{{"file", "style"}, {"text", ":root { --accent: red; }\n"}},
       config / "style.css"},
      {{{"file", "prompt"}, {"text", R"({"replace":{"## Scratch":"None."}})"}},
       config / "prompt.json"},
      {{{"file", "mcp"}, {"text", R"({"mcpServers":{}})"}},
       test.workspace / ".mcp.json"},
      {{{"file", "skill"}, {"name", "notes"}, {"text", skill}},
       test.home / ".uagent/skills/notes/SKILL.md"},
      {{{"file", "instructions"}, {"scope", "project"}, {"text", "Brief.\n"}},
       test.workspace / "AGENTS.md"}};
  for (auto [arguments, path] : written) {
    arguments["action"] = "write";
    const std::string text = arguments["text"].get<std::string>();
    CHECK(!FindToolArgumentIssue(tool, arguments) && !tool.validate(arguments));
    CHECK(tool.run(arguments, {}).error == ToolErrorCode::kPermissionDenied);
    CHECK(!PathExists(path));
    const std::string preview = tool.approval_preview(arguments);
    CHECK(preview.find(path.filename().string() + "\n+ " +
                       text.substr(0, text.find('\n'))) != std::string::npos);
    CHECK(tool.run(arguments, {}).Ok());
    CHECK(ReadFile(path.string(), 4096) == text);
    CHECK(!tool.run(arguments, {}).Ok());
    arguments["text"] = text + "\n";
    tool.approval_preview(arguments);
    WriteFile(path, "theirs");
    CHECK(!tool.run(arguments, {}).Ok());
    CHECK(ReadFile(path.string(), 4096) == "theirs");
  }
  // New, the folder's server file is private: an entry may carry a key.
  CHECK((std::filesystem::status(test.workspace / ".mcp.json").permissions() &
         (std::filesystem::perms::group_all |
          std::filesystem::perms::others_all)) == std::filesystem::perms::none);

  // A file is named by kind and a skill by one folder name, never a path;
  // text that cannot be saved is refused before anyone is asked.
  const std::vector<json> refused = {
      {{"file", "instructions"}, {"text", "no scope"}},
      {{"file", "style"}},
      {{"file", "style"}, {"text", std::string(kUserStyleBytes + 1, 'x')}},
      {{"file", "prompt"}, {"text", R"({"replace":{"## Nope":"x"}})"}},
      {{"file", "prompt"}, {"text", "not json"}},
      {{"file", "mcp"}, {"text", "[]"}},
      {{"file", "skill"}, {"text", skill}},
      {{"file", "skill"}, {"name", "../notes"}, {"text", skill}},
      {{"file", "skill"}, {"name", "a.b"}, {"text", skill}}};
  for (json arguments : refused) {
    arguments["action"] = "write";
    const auto issue = tool.validate(arguments);
    CHECK(issue && issue->code == "file.rejected");
  }
  // What makes a skill is the Library's to say, when it saves one.
  const json plain = {{"action", "write"},
                      {"file", "skill"},
                      {"name", "plain"},
                      {"text", "no front matter"}};
  tool.approval_preview(plain);
  CHECK(!tool.run(plain, {}).Ok());
  CHECK(!PathExists(test.home / ".uagent/skills/plain"));
}

// A command is queued only as the line a person approved, and once; a line
// the host does not run is refused before anyone is asked; and without a
// queue, as in an agent, the tool offers none.
TEST_CASE(TestUagentQueuesTheApprovedCommand) {
  std::vector<std::string> queued;
  const Tool tool = UagentTool(
      Describes, Proposes,
      [&queued](const std::string& line) { queued.push_back(line); });
  const auto command = [](const std::string& line) {
    return json{{"action", "command"}, {"line", line}};
  };
  const Tool agent = UagentTool(Describes, Proposes);
  CHECK(FindToolArgumentIssue(agent, command("/compact")).has_value());
  CHECK(!agent.parameters["properties"].contains("line"));
  CHECK(FindToolArgumentIssue(tool, {{"action", "command"}}).has_value());
  for (const char* line : {"/nope", "compact", "/fork later", "/quit", "/init",
                           "/review src", "/compact\n/yolo"}) {
    const auto issue = tool.validate(command(line));
    CHECK(issue && issue->code == "command.rejected" && issue->field == "line");
    CHECK(issue &&
          issue->message.find("show topic commands") != std::string::npos);
    // Put to a person all the same, it is not recorded as approved.
    CHECK(tool.approval_preview(command(line)).starts_with("cannot apply: "));
    CHECK(!tool.run(command(line), {}).Ok() && queued.empty());
  }
  const ApprovalMode before = CurrentApprovalMode();
  SetApprovalMode(ApprovalMode::kAsk);
  const std::string moves = " what the agent may do without asking.";
  const std::vector<std::pair<std::string, std::string>> previews = {
      {"/model sonnet", "/model sonnet"},
      {" /compact\n", "/compact"},
      {"/permissions yolo", "/permissions yolo\nThis widens" + moves},
      {"/permissions plan", "/permissions plan\nThis narrows" + moves},
      {"/yolo", "/yolo\nThis changes" + moves},
      {"/plan", "/plan\nThis changes" + moves}};
  for (const auto& [line, preview] : previews) {
    const json arguments = command(line);
    CHECK(!FindToolArgumentIssue(tool, arguments) && !tool.validate(arguments));
    CHECK(RequiredApproval(tool, arguments) == ApprovalClass::kMandatoryHuman);
    // Nothing is queued before a person saw the line, nor twice after.
    CHECK(tool.run(arguments, {}).error == ToolErrorCode::kPermissionDenied);
    CHECK(tool.approval_preview(arguments) == preview);
    CHECK(queued.empty());
    const ToolResult result = tool.run(arguments, {});
    CHECK(result.Ok() && result.output.find("not to you") != std::string::npos);
    CHECK(queued ==
          std::vector<std::string>{preview.substr(0, preview.find('\n'))});
    CHECK(!tool.run(arguments, {}).Ok());
    queued.clear();
  }
  // What runs is the line approved, not another asked for meanwhile.
  tool.approval_preview(command("/compact"));
  CHECK(!tool.run(command("/yolo"), {}).Ok() && queued.empty());
  SetApprovalMode(before);
}

// The servers, rules and skills a person manages are shown from the same
// controls, and what starts a server stays out: it can carry a credential.
TEST_CASE(TestUagentShowsServersRulesAndSkills) {
  TestWorkspace test("uagent-show");
  WriteFile(test.home / ".uagent/skills/notes/SKILL.md",
            "---\ndescription: Release notes\n---\n");
  ConfigManager manager = ConfigManager::Capture({});
  const RuntimeConfig active = manager.Initialize();
  const Api api(active);
  const std::vector<Tool> tools;
  McpRuntime mcp;
  auto server = std::make_unique<McpServer>();
  server->name = "tracker";
  server->config = {{"command", "npx"},
                    {"args", {"tracker", "--token", "canary-token"}}};
  server->error = "exited: bad canary-token";
  mcp.Add(std::move(server));
  const Tool tool = UagentTool([&](SelfTopic topic, const std::string& name) {
    return DescribeSelf(topic, name, {manager, active, api, tools, mcp});
  });
  const auto show = [&tool](const char* topic) {
    const ToolResult result =
        tool.run({{"action", "show"}, {"topic", topic}}, {});
    CHECK(result.Ok());
    return json::parse(result.output, nullptr, false);
  };
  const json servers = show("mcp")["servers"];
  CHECK(servers.size() == 1 && servers[0]["name"] == "tracker");
  CHECK(servers[0]["state"] == "failed" && servers[0]["tools"] == 0);
  CHECK(JsonDump(servers).find("canary") == std::string::npos);
  const json skills = show("skills");
  CHECK(std::ranges::any_of(skills, [](const json& skill) {
    return skill["name"] == "notes" && skill["status"] == "available";
  }));
  CHECK(show("permissions")["rules"].empty());
}

// A project's settings are saved under its folder, beside what is saved for
// all conversations, and nowhere in the project.
TEST_CASE(TestProjectSettingsAreSavedByFolder) {
  TestWorkspace test("config-project");
  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ConfigManager manager = ConfigManager::Capture({});
  ConfigProposal proposal =
      PrepareConfigProposal(ConfigProposalScope::kProject,
                            {{"UAGENT_MAX_TOOL_CALLS", "120", false}}, manager);
  CHECK(proposal.ok);
  std::string error;
  CHECK(CommitConfigProposal(proposal, error));
  const SavedSettings saved = ReadSettings(test.workspace.string());
  CHECK(saved.all.empty());
  CHECK(saved.project.at("UAGENT_MAX_TOOL_CALLS") == "120");
  CHECK(manager.Read().sources["UAGENT_MAX_TOOL_CALLS"] == "project");
  CHECK(!PathExists((test.workspace / ".uagent").string()));
  // A value may refer to a name saved beside it, and each project has its
  // own.
  CHECK(ChangeSettings(test.workspace.string(), [](SettingValues& project) {
          project["TOKEN"] = "project-token";
          project["UAGENT_PROVIDERS"] = "$TOKEN";
          return std::string();
        }).empty());
  const auto resolved = manager.Read();
  CHECK(resolved.values.at("UAGENT_PROVIDERS") == "project-token");
  CHECK(!resolved.values.contains("TOKEN"));
  // A reset is refused when a setting it removes changed since it was read.
  ConfigProposal reset =
      PrepareConfigReset(ConfigProposalScope::kProject, manager);
  CHECK(reset.ok && reset.written.contains("UAGENT_MAX_TOOL_CALLS"));
  CHECK(!reset.written.contains("UAGENT_PROVIDERS"));
  CHECK(ChangeSettings(test.workspace.string(), [](SettingValues& project) {
          project["UAGENT_MAX_TOOL_CALLS"] = "121";
          project["UAGENT_MAX_STEPS"] = "3";
          return std::string();
        }).empty());
  CHECK(!CommitConfigProposal(reset, error));
  CHECK(ReadSettings(test.workspace.string()).project.size() == 4);
  // Another folder is not affected.
  CHECK(ReadSettings(test.root.string()).project.empty());
  // Where no folder is named (the web host's own view) there is no project
  // to save for, and never a fall-through to all conversations.
  ConfigManager host = ConfigManager::Capture({}, "");
  CHECK(!PrepareConfigProposal(ConfigProposalScope::kProject,
                               {{"UAGENT_MAX_STEPS", "5", false}}, host)
             .ok);
  CHECK(ReadSettings("").all.empty());
  // A change may name a setting as the settings file does.
  std::vector<ConfigChange> named;
  std::string why;
  CHECK(ParseConfigChanges(
      {{"changes", {{{"key", "limits.maxSteps"}, {"value", "5"}}}}}, named,
      why));
  CHECK(named.size() == 1 && named[0].key == "UAGENT_MAX_STEPS");
}

// Get reports what each scope holds; reset clears a scope's overrides in one
// save but never its secrets.
TEST_CASE(TestConfigurationResetKeepsSecrets) {
  TestWorkspace test("config-reset");
  const std::string providers =
      R"({"p":{"base_url":"https://p.example/v1","api_key":"keep-me"}})";
  CHECK(SaveSettings({{"UAGENT_MAX_TOOL_CALLS", "40"},
                      {"UAGENT_WEB_SEARCH_BACKEND", "off"},
                      {"UAGENT_PROVIDERS", providers}})
            .empty());
  ScopedEnv no_calls("UAGENT_MAX_TOOL_CALLS");
  ScopedEnv no_backend("UAGENT_WEB_SEARCH_BACKEND");
  ScopedEnv no_providers("UAGENT_PROVIDERS");
  ConfigManager manager = ConfigManager::Capture({});
  RuntimeConfig active = manager.Read().config;
  auto find = [](const json& result, const std::string& name) {
    for (const json& setting : result["settings"]) {
      if (setting["name"] == name) return setting;
    }
    return json();
  };
  json got = ConfigurationControl({{"operation", "get"}}, manager);
  // Facts, typed like the default: what the user file sets, what applies and
  // where that comes from. A secret says only that it is set.
  const json calls = find(got, "UAGENT_MAX_TOOL_CALLS");
  CHECK(calls["set"]["user"] == 40 && calls["effective"] == 40);
  CHECK(calls["scopes"].size() == 2 && calls["scopes"][1] == "project");
  CHECK(calls["source"] == "user" && calls["locked"] == false);
  CHECK(find(got, "UAGENT_PROVIDERS")["set"]["user"].dump().find("keep-me") ==
        std::string::npos);
  const json steps = find(got, "UAGENT_MAX_STEPS");
  CHECK(!steps.contains("set") && steps["effective"] == steps["default"]);
  CHECK(steps["source"] == "default");
  // A model role names the setting it follows and takes its value.
  CHECK(find(got, "UAGENT_MEMORY_MODEL")["follows"] == "UAGENT_MODEL");

  json reset = ConfigurationControl({{"operation", "reset"}, {"scope", "user"}},
                                    manager);
  CHECK(!reset.contains("error"));
  CHECK(reset["effects"].size() == 2);
  CHECK(reset["effects"][0]["effect"] == "next_turn");
  CHECK(!find(reset, "UAGENT_MAX_TOOL_CALLS").contains("set"));
  const SettingValues kept = ReadSettings("").all;
  CHECK(kept.size() == 1 &&
        json::parse(kept.at("UAGENT_PROVIDERS")) == json::parse(providers));
  // Nothing left to reset is not an error and writes nothing.
  json again = ConfigurationControl({{"operation", "reset"}, {"scope", "user"}},
                                    manager);
  CHECK(!again.contains("error"));
  CHECK(again["effects"].empty());
}

}  // namespace uagent
