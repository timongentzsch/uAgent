// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/app/agent_tools.h"
#include "include/app/artifact.h"
#include "include/app/asset_store.h"
#include "include/app/config_proposal.h"
#include "include/app/panel.h"
#include "include/app/session.h"
#include "include/app/uagent_tool.h"
#include "include/browser/browser.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/mcp/discover.h"
#include "include/mcp/register.h"
#include "include/providers/providers.h"
#include "include/tools/ask.h"
#include "include/tools/browser.h"
#include "include/tools/registry.h"
#include "include/tools/skill.h"
#include "include/tools/web_fetch.h"
#include "include/tools/web_search.h"
#include "src/app/bootstrap_internal.h"

namespace uagent {

// Snapshots a file into the session's committed assets, so every client can
// show it; `name` defaults to the file's own.
session::AssetStoreResult StoreSessionFile(const std::string& session_path,
                                           const std::string& path,
                                           const std::string& name,
                                           bool tool_copy) {
  session::AssetStoreResult stored;
  std::string bytes;
  if (!ReadRegularFile(path, session::kUploadBytes, bytes, stored.error)) {
    return stored;
  }
  return session::SessionAssets().Store(
      session_path, bytes,
      name.empty() ? std::filesystem::path(path).filename().string() : name,
      /*committed=*/true, tool_copy);
}

std::vector<Tool> BuildTools(AppContext& context,
                             const std::filesystem::path& workspace,
                             const json& trusted_snapshot,
                             std::vector<Skill> skills, std::string& error) {
  Api& api = context.runtime.api;
  AppRuntime& runtime = context.runtime;
  // A memory extractor gets its memory tools only; a tool-less child remains
  // useful for constrained internal tasks.
  const std::string toolset = EnvStr("UAGENT_INTERNAL_TOOLSET");
  const std::string session_path =
      context.channel ? context.channel->SessionPath() : std::string();
  std::vector<Tool> tools = BuiltinTools(runtime.processes, workspace);
  if (!runtime.config.memory_enabled) {
    std::erase_if(tools, [](const Tool& tool) { return tool.memory_store; });
  }
  if (toolset == "memory") {
    std::erase_if(tools, [](const Tool& tool) { return !tool.memory_store; });
    return tools;
  }
  if (toolset == "none") return {};
  tools.push_back(UagentTool([app = &context](SelfTopic topic,
                                              const std::string& name) {
    return DescribeSelf(
        topic, name,
        SelfDescriptionInputs{app->config_manager, app->runtime.config,
                              app->runtime.api, app->tools, app->agent.get()});
  }));
  if (InteractiveApprovalAvailable() &&
      (context.tool_policy.allowed & Capability(ToolCapability::kMutate))) {
    tools.push_back(UagentConfigureTool(
        [app = &context](ConfigProposalScope scope,
                         const std::vector<ConfigChange>& changes) {
          return PrepareConfigProposal(scope, changes, app->config_manager);
        },
        std::make_shared<ConfigApprovals>()));
    tools.push_back(UagentInstructionsTool());
  }
  if (OffersWebSearch(api, context.provider.providers)) {
    tools.push_back(
        WebSearchTool(api, runtime.side_usage, context.provider.providers));
  }
  // Reading a named URL needs no hosted route, so it does not follow search's
  // availability.
  tools.push_back(WebFetchTool(api));
  if (!session_path.empty()) tools.push_back(ArtifactTool(session_path));
  // Only a session someone can answer gets ask: never a headless run, which
  // could only ever time out.
  if (context.channel && InteractiveApprovalAvailable()) {
    // Option images are snapshotted into the session, as artifacts are, so
    // every client can show them.
    AskImage image;
    if (!session_path.empty()) {
      image = [session_path](const std::string& path, std::string& failure) {
        session::AssetStoreResult stored =
            StoreSessionFile(session_path, path, "", /*tool_copy=*/false);
        failure = stored.error;
        return stored.value;
      };
    }
    tools.push_back(AskTool(
        [](const json& questions, bool* eof) {
          return ReadInteraction(
              {.kind = "ask",
               .prompt = JsonValue(questions[0], "question", ""),
               .questions = questions},
              eof);
        },
        std::move(image)));
  }
#ifdef UAGENT_BROWSER
  // The web host runs the browser; any top-level session that reaches it may
  // use it, one at a time. A one-shot run has no session file to be named by.
  if (!browser::DataDirectory().empty() &&
      browser::Request({{"op", "ping"}}, 3000).value("ok", false)) {
    context.browser_lease =
        session_path.empty() ? RandomToken(16) : SessionIdFor(session_path);
    tools.push_back(BrowserTool(
        context.browser_lease,
        // A one-shot run has nobody to take the browser over.
        [alone = !context.channel && !context.options.prompt.empty()](
            const std::string& id, const std::string& prompt, bool* eof) {
          if (alone) {
            *eof = true;
            return std::string();
          }
          return ReadInteraction(
              {.id = id, .kind = "browser", .prompt = prompt}, eof);
        }));
  }
#endif
  // An agent's MCP servers are its first ancestor's: each agent starting
  // its own fleet would multiply them. A panel may call none of their tools,
  // so it starts none of them.
  if (!context.options.Panel() && AgentDepth() == 0) {
    error = McpRegister(tools, runtime.mcp, runtime.config, trusted_snapshot);
    if (!error.empty()) return {};
  }
  AddAgentTools(tools, api, [app = &context] {
    return ChosenSelection(app->runtime.api, app->provider.providers);
  });
  if (context.options.Panel()) AdaptPanelTools(tools);
  // A lead agent's side conversation runs in the ordinary sandbox, so a
  // lead agent's read-only commands do not come with it.
  if (!context.options.Panel() && !session_path.empty()) {
    const auto loaded = SessionStore::Inspect(session_path);
    if (loaded.record &&
        JsonValue(JsonValue(JsonValue(loaded.record->state.display, "facts",
                                      json::object()),
                            "side-origin", json::object()),
                  "parent", "") == SessionIdFor(PanelPath(CanonicalCwd()))) {
      std::erase_if(tools, [](const Tool& tool) { return tool.name == "run"; });
    }
  }
  ApplyToolPolicy(tools, context.tool_policy);
  // A command in the readonly sandbox changes nothing, so it needs no
  // approval.
  const SandboxStatus& sandbox = SandboxRuntime();
  if (sandbox.mode == SandboxMode::kEnforced && sandbox.policy.read_only) {
    for (Tool& tool : tools) {
      if (tool.name != "run") continue;
      tool.mutating = false;
      tool.mutates = nullptr;
      // A refused connection reads like a name that does not resolve.
      tool.description +=
          " Commands here only read: they write nothing but temporary "
          "files and reach no network.";
    }
  }
  std::vector<std::string> tool_names = ToolNames(tools);
  KeepSupportedSkills(skills, tool_names);
  if (!skills.empty()) {
    std::vector<Tool> skill_tool{SkillTool(skills, tool_names)};
    ApplyToolPolicy(skill_tool, context.tool_policy);
    if (!skill_tool.empty()) {
      tool_names.push_back(skill_tool.front().name);
      tools.push_back(std::move(skill_tool.front()));
    }
  }
  return tools;
}

// An MCP rescan can add or drop tools mid-session, so the policy filter has to
// run again on whatever the refresh produced.
Agent::ToolRefresher MakeToolRefresher(AppContext* app) {
  return [app](std::chrono::steady_clock::time_point deadline) {
    bool changed = McpRefreshTools(app->tools, app->runtime.mcp,
                                   app->runtime.config, deadline);
    if (changed) {
      ApplyToolPolicy(app->tools, app->tool_policy);
      app->session_approvals.clear();
    }
    return changed;
  };
}

}  // namespace uagent
