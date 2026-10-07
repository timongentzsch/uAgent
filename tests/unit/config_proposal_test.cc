// Copyright 2026 Timon Gentzsch

#include "include/app/config_proposal.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "include/app/uagent_tool.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "include/core/settings_store.h"
#include "include/providers.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

void Write(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << bytes;
}

}  // namespace

void TestConfigProposalAndCommit() {
  TestWorkspace test("config-proposal");
  const std::filesystem::path config = test.home / ".uagent" / ".config";
  const std::string original =
      "# keep me\n"
      "# COMMENT_API_KEY=not-an-assignment\n"
      "UAGENT_MAX_TOOL_CALLS=40\n"
      "OPENROUTER_API_KEY=canary-secret\n"
      "QWEN_GPU_API_KEY=adjacent-provider-secret\n"
      "UAGENT_PROVIDERS='{\"old\":{\"base_url\":\"https://old.example/v1\","
      "\"api_key\":\"existing-provider-secret\"},\"gpu\":{\"base_url\":"
      "\"https://gpu.example/v1\",\"api_key\":\"$QWEN_GPU_API_KEY\"}}'\n";
  Write(config, original);

  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ScopedEnv no_providers("UAGENT_PROVIDERS");
  // Initialize exports file values into the process environment.
  ScopedEnv no_route_key("OPENROUTER_API_KEY");
  ConfigManager manager = ConfigManager::Capture(false, {});
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
  CHECK(choice.error.find("auto openrouter off") != std::string::npos);

  // A credential may never arrive through a tool argument.
  ConfigProposal secret =
      PrepareConfigProposal(ConfigProposalScope::kUser,
                            {{"OPENROUTER_API_KEY", "leaked", false}}, manager);
  CHECK(!secret.ok);
  CHECK(secret.error.find("credential") != std::string::npos);
  auto human_secret = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"OPENROUTER_API_KEY", "human-secret-replacement", false}}, manager,
      /*direct_user=*/true);
  CHECK(human_secret.ok);
  CHECK(human_secret.Preview().find("human-secret-replacement") ==
        std::string::npos);
  auto invalid_human_provider = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", "[]", false}}, manager,
      /*direct_user=*/true);
  CHECK(!invalid_human_provider.ok);

  // Removing a credential does not carry one through the tool arguments.
  ConfigProposal unset_secret = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"OPENROUTER_API_KEY", "", true}}, manager);
  CHECK(unset_secret.ok);
  CHECK(unset_secret.Preview().find("canary-secret") == std::string::npos);

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
  CHECK(composite.Preview().find("<redacted>") != std::string::npos);
  CHECK(composite.Preview().find("configured: {") == std::string::npos);
  CHECK(composite.Preview().find("\"wire_api\": \"responses\"") !=
        std::string::npos);
  CHECK(composite.Preview().find("-   \"gpu\":") != std::string::npos);
  CHECK(composite.Preview().find("+   \"codex-local\":") != std::string::npos);

  // The accepted reference resolves before provider selection.
  std::string error;
  CHECK(CommitConfigProposal(composite, error));
  ScopedEnv provider_key("CODEX_LOCAL_PROXY_API_KEY", "resolved-local-key");
  unsetenv("UAGENT_PROVIDERS");
  ConfigManager resolved_manager = ConfigManager::Capture(false, {});
  resolved_manager.Initialize();
  ProviderCatalog catalog = LoadProviderCatalog();
  const NamedProvider* resolved =
      FindNamedProvider(catalog.providers, "codex-local");
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
  CHECK(proposal.Preview().find("canary-secret") == std::string::npos);

  CHECK(CommitConfigProposal(proposal, error));
  SettingValues saved = ReadSettings("").all;
  CHECK(saved.at("UAGENT_MAX_TOOL_CALLS") == "120");
  CHECK(saved.at("OPENROUTER_API_KEY") == "canary-secret");  // untouched

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

  // The store hands a proposal out once, and only for the arguments it was
  // prepared from.
  ConfigApprovals store;
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

  Tool configure =
      UagentTool([](SelfTopic, const std::string&) { return json::object(); },
                 [](ConfigProposalScope, const std::vector<ConfigChange>&) {
                   ConfigProposal rejected;
                   rejected.error = "specific rejection";
                   return rejected;
                 },
                 std::make_shared<ConfigApprovals>());
  const json inspect = {{"action", "inspect"}, {"topic", "config"}};
  CHECK(RequiredApproval(configure, inspect) == ApprovalClass::kNone);
  CHECK(configure.run(inspect, {}).Ok());
  CHECK(!configure.validate(inspect));
  // What only a change takes is dropped from an inspect call before it is
  // judged: models that fill every field send it along empty.
  json filled = {{"action", "inspect"}, {"topic", "config"},
                 {"scope", ""},         {"changes", json::array()},
                 {"text", ""},          {"audience", ""}};
  CanonicalizeToolArguments(configure, filled, nullptr);
  CHECK(filled == (json{{"action", "inspect"}, {"topic", "config"}}));
  CHECK(!configure.validate(filled));
  CHECK(RequiredApproval(configure, {{"action", "configure"}}) ==
        ApprovalClass::kMandatoryHuman);
  Tool restricted =
      UagentTool([](SelfTopic, const std::string&) { return json::object(); });
  CHECK(restricted.parameters["properties"]["action"]["enum"] ==
        json::array({"inspect"}));
  CHECK(restricted.capabilities == 0);
  CHECK(restricted.validate({{"action", "configure"}}).has_value());
  auto issue = configure.validate({{"action", "configure"},
                                   {"scope", "user"},
                                   {"changes",
                                    {{{"key", "UAGENT_MAX_STEPS"},
                                      {"operation", "set"},
                                      {"value", "7"}}}}});
  CHECK(issue && issue->code == "config.rejected");
  CHECK(issue && issue->field == "changes");
  CHECK(issue && issue->message == "specific rejection");
}

// A project's settings are saved under its folder, beside what is saved for
// all conversations, and nowhere in the project.
void TestProjectSettingsAreSavedByFolder() {
  TestWorkspace test("config-project");
  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ConfigManager manager = ConfigManager::Capture(false, {});
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
          project["UAGENT_API_KEY"] = "$TOKEN";
          return std::string();
        }).empty());
  const auto resolved = manager.Read();
  CHECK(resolved.values.at("UAGENT_API_KEY") == "project-token");
  CHECK(!resolved.values.contains("TOKEN"));
  // Such a document goes out and comes back as written.
  json exported = {{"format", 1},
                   {"all", {{"LIMIT", "7"}, {"UAGENT_MAX_STEPS", "$LIMIT"}}},
                   {"projects", json::object()}};
  CHECK(CheckSavedSettings(exported).empty());
  CHECK(exported["all"]["UAGENT_MAX_STEPS"] == "$LIMIT");
  // A value is checked and kept as it is, whatever the environment holds
  // under its name.
  {
    ScopedEnv elsewhere("UAGENT_MAX_TOOL_CALLS", "9");
    exported["all"]["UAGENT_MAX_TOOL_CALLS"] = "7";
    CHECK(CheckSavedSettings(exported).empty());
    CHECK(exported["all"]["UAGENT_MAX_TOOL_CALLS"] == "7");
  }
  // A reset is refused when a setting it removes changed since it was read.
  ConfigProposal reset =
      PrepareConfigReset(ConfigProposalScope::kProject, manager);
  CHECK(reset.ok && reset.written.contains("UAGENT_MAX_TOOL_CALLS"));
  CHECK(!reset.written.contains("UAGENT_API_KEY"));
  CHECK(ChangeSettings(test.workspace.string(), [](SettingValues& project) {
          project["UAGENT_MAX_TOOL_CALLS"] = "121";
          project["UAGENT_MAX_STEPS"] = "3";
          return std::string();
        }).empty());
  CHECK(!CommitConfigProposal(reset, error));
  CHECK(ReadSettings(test.workspace.string()).project.size() == 4);
  exported["all"]["LIMIT"] = "many";
  CHECK(!CheckSavedSettings(exported).empty());
  // Another folder is not affected.
  CHECK(ReadSettings(test.root.string()).project.empty());
  // Where no folder is named (the web host's own view) there is no project
  // to save for, and never a fall-through to all conversations.
  ConfigManager host = ConfigManager::Capture(false, {}, "");
  CHECK(!PrepareConfigProposal(ConfigProposalScope::kProject,
                               {{"UAGENT_MAX_STEPS", "5", false}}, host)
             .ok);
  CHECK(ReadSettings("").all.empty());
}

// Get reports what each scope holds; reset clears a scope's overrides in one
// save but never its secrets.
void TestConfigurationResetKeepsSecrets() {
  TestWorkspace test("config-reset");
  const std::filesystem::path config = test.home / ".uagent" / ".config";
  Write(config,
        "UAGENT_MAX_TOOL_CALLS=40\n"
        "UAGENT_WEB_SEARCH_BACKEND=off\n"
        "OPENROUTER_API_KEY=keep-me\n");
  ScopedEnv no_calls("UAGENT_MAX_TOOL_CALLS");
  ScopedEnv no_backend("UAGENT_WEB_SEARCH_BACKEND");
  ScopedEnv no_key("OPENROUTER_API_KEY");
  ConfigManager manager = ConfigManager::Capture(false, {});
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
  CHECK(find(got, "OPENROUTER_API_KEY")["set"]["user"] == true);
  CHECK(!find(got, "OPENROUTER_API_KEY").contains("effective"));
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
  CHECK(find(reset, "OPENROUTER_API_KEY")["set"]["user"] == true);
  CHECK(ReadSettings("").all ==
        (SettingValues{{"OPENROUTER_API_KEY", "keep-me"}}));
  // Nothing left to reset is not an error and writes nothing.
  json again = ConfigurationControl({{"operation", "reset"}, {"scope", "user"}},
                                    manager);
  CHECK(!again.contains("error"));
  CHECK(again["effects"].empty());
}

}  // namespace uagent
