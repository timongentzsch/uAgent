// Copyright 2026 Timon Gentzsch

#include "include/app/config_proposal.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "include/core/config_document.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/providers.h"
#include "include/tools/configure.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

void Write(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << bytes;
}

}  // namespace

void TestConfigDocumentPreservesFile() {
  const std::string original =
      "# µAgent configuration\n"
      "\n"
      "# routing\n"
      "UAGENT_MODEL=demo-model\n"
      "export UAGENT_MAX_TOOL_CALLS=40   \n"
      "UNKNOWN_KEY=kept\n"
      "\n"
      "# trailing comment\n";
  std::string error;

  ConfigDocument document = ConfigDocument::Parse(original);
  CHECK(document.Render() == original);  // parse/render is byte-identical

  CHECK(document.Set("UAGENT_MAX_TOOL_CALLS", "120", error));
  std::string updated = document.Render();
  CHECK(updated.find("export UAGENT_MAX_TOOL_CALLS=120") != std::string::npos);
  CHECK(updated.find("# µAgent configuration") != std::string::npos);
  CHECK(updated.find("# trailing comment") != std::string::npos);
  CHECK(updated.find("UNKNOWN_KEY=kept") != std::string::npos);
  CHECK(updated.find("UAGENT_MODEL=demo-model") != std::string::npos);

  // Appending keeps everything before it untouched.
  CHECK(document.Set("UAGENT_MAX_STEPS", "12", error));
  CHECK(document.Render().find("UAGENT_MAX_STEPS=12") != std::string::npos);

  CHECK(document.Unset("UNKNOWN_KEY", error));
  CHECK(document.Render().find("UNKNOWN_KEY") == std::string::npos);

  // CRLF survives a round trip.
  ConfigDocument crlf = ConfigDocument::Parse("A=1\r\nB=2\r\n");
  CHECK(crlf.Set("A", "9", error));
  CHECK(crlf.Render() == "A=9\r\nB=2\r\n");

  // A duplicated assignment is ambiguous, so a targeted edit refuses.
  ConfigDocument duplicate = ConfigDocument::Parse("A=1\nA=2\n");
  CHECK(!duplicate.Set("A", "3", error));
  CHECK(error.find("assigned 2 times") != std::string::npos);
  CHECK(duplicate.Render() == "A=1\nA=2\n");

  // Values needing quotes round-trip through the loader's own unquoting.
  std::string literal;
  CHECK(ConfigValueLiteral("plain", literal, error) && literal == "plain");
  CHECK(ConfigValueLiteral("has space", literal, error) &&
        literal == "'has space'");
  CHECK(ConfigValueLiteral("", literal, error) && literal == "''");
  CHECK(!ConfigValueLiteral("mixes '\"$", literal, error));
}

void TestConfigProposalAndCommit() {
  TestWorkspace test("config-proposal");
  const std::filesystem::path config = test.home / ".uagent" / ".config";
  const std::string original =
      "# keep me\n"
      "UAGENT_MAX_TOOL_CALLS=40\n"
      "UAGENT_WEB_SEARCH_API_KEY=canary-secret\n"
      "QWEN_GPU_API_KEY=adjacent-provider-secret\n"
      "UAGENT_PROVIDERS='{\"old\":{\"base_url\":\"https://old.example/v1\","
      "\"api_key\":\"existing-provider-secret\"},\"gpu\":{\"base_url\":"
      "\"https://gpu.example/v1\",\"api_key\":\"$QWEN_GPU_API_KEY\"}}'\n";
  Write(config, original);

  ScopedEnv no_custom("UAGENT_CONFIG_FILE");
  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ScopedEnv no_providers("UAGENT_PROVIDERS");
  ConfigManager manager = ConfigManager::Capture(false, {});
  RuntimeConfig active = manager.Initialize();

  // Unknown keys are rejected before anything is read or written.
  ConfigProposal unknown = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_NOT_A_SETTING", "1", false}},
      manager, active, false);
  CHECK(!unknown.ok);
  CHECK(unknown.error.find("unknown setting") != std::string::npos);

  // A credential may never arrive through a tool argument.
  ConfigProposal secret = PrepareConfigProposal(
      ConfigProposalScope::kUser,
      {{"UAGENT_WEB_SEARCH_API_KEY", "leaked", false}}, manager, active, false);
  CHECK(!secret.ok);
  CHECK(secret.error.find("credential") != std::string::npos);

  // Removing a credential does not carry one through the tool arguments.
  ConfigProposal unset_secret = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_WEB_SEARCH_API_KEY", "", true}},
      manager, active, false);
  CHECK(unset_secret.ok);
  CHECK(unset_secret.Preview().find("canary-secret") == std::string::npos);

  // Composite credentials may be named only by an exact environment reference.
  const std::string providers =
      R"({"codex-local":{"base_url":"http://127.0.0.1:8787/openai/v1","api_key":"$CODEX_LOCAL_PROXY_API_KEY","wire_api":"responses","hosted_tools":["web_search"]}})";
  ConfigProposal composite = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", providers, false}},
      manager, active, false);
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
  CHECK(Read(config) == original);

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
    ConfigProposal rejected = PrepareConfigProposal(
        ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", unsafe, false}},
        manager, active, false);
    CHECK(!rejected.ok);
    CHECK(rejected.error.find("environment-variable reference") !=
          std::string::npos);
  }

  for (const std::string url : {"https://user:secret@example.com/v1",
                                "https://example.com/v1?api_key=secret",
                                "https://example.com/v1#secret"}) {
    const std::string unsafe =
        "{\"bad\":{\"base_url\":\"" + url + "\",\"api_key\":\"$API_KEY\"}}";
    ConfigProposal rejected = PrepareConfigProposal(
        ConfigProposalScope::kUser, {{"UAGENT_PROVIDERS", unsafe, false}},
        manager, active, false);
    CHECK(!rejected.ok);
    CHECK(rejected.error.find("without credentials") != std::string::npos);
  }

  // Out-of-range values are refused against the registry's own bounds.
  ConfigProposal invalid = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_TOOL_CALLS", "-5", false}},
      manager, active, false);
  CHECK(!invalid.ok);

  // Project scope cannot bootstrap its own trust.
  ConfigProposal untrusted = PrepareConfigProposal(
      ConfigProposalScope::kProject, {{"UAGENT_MAX_TOOL_CALLS", "60", false}},
      manager, active, /*project_trusted=*/false);
  CHECK(!untrusted.ok);
  CHECK(untrusted.error.find("not trusted") != std::string::npos);

  ConfigProposal proposal = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_TOOL_CALLS", "120", false}},
      manager, active, false);
  CHECK(proposal.ok);
  CHECK(proposal.effects.size() == 1);
  CHECK(proposal.effects[0].effect == ConfigEffect::kActiveNextUserTurn);
  // Preparing writes nothing.
  CHECK(Read(config) == composite.candidate);
  // A neighbouring secret is redacted out of the preview.
  CHECK(proposal.Preview().find("canary-secret") == std::string::npos);
  CHECK(proposal.Preview().find("adjacent-provider-secret") ==
        std::string::npos);
  CHECK(proposal.diff.find("+ UAGENT_MAX_TOOL_CALLS=120") != std::string::npos);

  CHECK(CommitConfigProposal(proposal, error));
  std::string written = Read(config);
  CHECK(written.find("UAGENT_MAX_TOOL_CALLS=120") != std::string::npos);
  CHECK(written.find("# keep me") != std::string::npos);
  CHECK(written.find("canary-secret") != std::string::npos);  // untouched

  // Replaying the same approved proposal is refused: its snapshot is stale.
  CHECK(!CommitConfigProposal(proposal, error));
  CHECK(error.find("changed after the preview") != std::string::npos);

  // An external edit between preview and commit is never merged away.
  ConfigProposal racing = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "7", false}}, manager,
      active, false);
  CHECK(racing.ok);
  Write(config, written + "UAGENT_TOOL_TIMEOUT=99\n");
  CHECK(!CommitConfigProposal(racing, error));
  CHECK(Read(config).find("UAGENT_MAX_STEPS") == std::string::npos);

  // An expired proposal cannot commit.
  ConfigProposal expired = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "7", false}}, manager,
      active, false);
  CHECK(expired.ok);
  expired.expires = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  CHECK(!CommitConfigProposal(expired, error));
  CHECK(error.find("expired") != std::string::npos);

  // The store hands a proposal out once, and only for the arguments it was
  // prepared from.
  ConfigProposalStore store;
  ConfigProposal stored = PrepareConfigProposal(
      ConfigProposalScope::kUser, {{"UAGENT_MAX_STEPS", "7", false}}, manager,
      active, false);
  CHECK(stored.ok);
  const json arguments = {{"scope", "user"}, {"key", "UAGENT_MAX_STEPS"}};
  store.Put("key", arguments, stored);
  CHECK(!store.Take("key", json{{"scope", "user"}}).ok);  // different request
  store.Put("key", arguments, stored);
  CHECK(store.Take("key", arguments).ok);
  CHECK(!store.Take("key", arguments).ok);
  CHECK(!store.Take("never-prepared", arguments).ok);

  Tool configure = ConfigureTool(
      [](ConfigProposalScope, const std::vector<ConfigChange>&) {
        ConfigProposal rejected;
        rejected.error = "specific rejection";
        return rejected;
      },
      std::make_shared<ConfigProposalStore>());
  auto issue = configure.validate({{"scope", "user"},
                                   {"changes",
                                    {{{"key", "UAGENT_MAX_STEPS"},
                                      {"operation", "set"},
                                      {"value", "7"}}}}});
  CHECK(issue && issue->code == "config.rejected");
  CHECK(issue && issue->field == "changes");
  CHECK(issue && issue->message == "specific rejection");
}

void TestProjectConfigTrustRestamp() {
  TestWorkspace test("config-trust");
  const std::filesystem::path project = test.workspace / ".uagent";
  std::filesystem::create_directories(project);
  Write(project / ".config", "UAGENT_MAX_TOOL_CALLS=40\n");
  Write(test.workspace / ".mcp.json", "{\"mcpServers\":{}}\n");

  std::string error;
  json approved_mcp;
  CHECK(TrustProjectConfig(error, &approved_mcp));
  CHECK(ProjectConfigTrusted());

  ScopedEnv no_custom("UAGENT_CONFIG_FILE");
  ScopedEnv no_override("UAGENT_MAX_TOOL_CALLS");
  ConfigManager manager = ConfigManager::Capture(true, {});
  RuntimeConfig active = manager.Initialize();

  ConfigProposal proposal = PrepareConfigProposal(
      ConfigProposalScope::kProject, {{"UAGENT_MAX_TOOL_CALLS", "120", false}},
      manager, active, /*project_trusted=*/true);
  CHECK(proposal.ok);

  std::string notice;
  CHECK(CommitConfigProposal(proposal, error, &notice));
  CHECK(notice.empty());
  // The approved edit changed the very content trust records, so without a
  // re-stamp the workspace would be treated as untrusted on the next launch.
  CHECK(ProjectConfigTrusted());

  // A .mcp.json that moved since trust was granted must never be carried over
  // by a config commit; the workspace has to be confirmed again instead.
  Write(test.workspace / ".mcp.json",
        "{\"mcpServers\":{\"x\":{\"command\":\"evil\"}}}\n");
  ConfigProposal after_swap = PrepareConfigProposal(
      ConfigProposalScope::kProject, {{"UAGENT_MAX_STEPS", "9", false}},
      manager, active, /*project_trusted=*/true);
  CHECK(after_swap.ok);
  CHECK(CommitConfigProposal(after_swap, error, &notice));
  CHECK(notice.find("trusted again") != std::string::npos);
  CHECK(!ProjectConfigTrusted());
}

}  // namespace uagent
