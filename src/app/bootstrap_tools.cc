// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
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
#include "include/tools/session.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
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
  // One read of the toolset selector: the three shapes it can take are one
  // decision, not three unrelated conditions.
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
  // A tool-less child remains useful for constrained internal tasks.
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
  if (!session_path.empty() && AgentDepth() == 0) {
    tools.push_back(ArtifactTool(session_path));
  }
  // Only a session someone can answer gets ask: never headless runs or
  // delegated children, which could only ever time out.
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
  if (!browser::DataDirectory().empty() && AgentDepth() == 0 &&
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
  // The default lean child is an isolation and context-efficiency boundary:
  // do not clone the parent's entire MCP fleet into every delegation. A root
  // lean session and an explicitly requested full child still get MCP. A
  // panel may call none of their tools, so it starts none of them.
  if (!context.options.Panel() && (AgentDepth() == 0 || toolset != "lean")) {
    error = McpRegister(tools, runtime.mcp, runtime.config, trusted_snapshot);
    if (!error.empty()) return {};
  }
  if (CanDelegate()) {
    tools.push_back(
        SubagentTool(api, runtime.processes, context.provider.routes,
                     context.provider.providers, context.options.debug));
    tools.push_back(
        SubagentFanoutTool(api, runtime.processes, context.provider.routes,
                           context.provider.providers, context.options.debug));
  }
  // Peer sessions are text-only and isolation-gated by links, so the session
  // tools are safe in every toolset, lean included.
  tools.push_back(SessionTool());
  tools.push_back(SessionMessageTool([](const std::string& path) {
    std::string ignored;  // the mail is queued either way
    session::Wake(JsonValue(SessionHeader(path), kSessionHeaderCwd, ""), path,
                  ignored);
  }));
  // A side fork keeps a lead agent's folder tools without becoming another
  // panel. Its saved tool ceiling still decides which of them are offered.
  bool side_panel_tools = false;
  if (!context.options.Panel() && !session_path.empty()) {
    const auto loaded = SessionStore::Inspect(session_path);
    if (loaded.record) {
      const json origin = JsonValue(
          JsonValue(loaded.record->state.display, "facts", json::object()),
          "side-origin", json::object());
      if (const json* names = JsonArray(origin, "tools")) {
        side_panel_tools = std::ranges::any_of(*names, [](const json& name) {
          return name == "thread" || name == "history" || name == "decide" ||
                 name == "plan";
        });
      }
    }
  }
  if (context.options.Panel() || side_panel_tools) {
    AddPanelTools(tools, CanonicalCwd(), [app = &context] {
      return ChosenSelection(app->runtime.api, app->provider.providers);
    });
  }
  // A side fork runs in the ordinary sandbox, so a lead agent's read-only
  // commands do not come with it.
  if (side_panel_tools && !context.options.Panel()) {
    std::erase_if(tools, [](const Tool& tool) { return tool.name == "run"; });
  }
  if (toolset == "lean") {
    KeepLeanTools(tools);
  }
  ApplyToolPolicy(tools, context.tool_policy);
  // A command in the readonly sandbox changes nothing, so it needs no
  // approval, and a child inherits that sandbox.
  const SandboxStatus& sandbox = SandboxRuntime();
  const bool read_only =
      sandbox.mode == SandboxMode::kEnforced && sandbox.policy.read_only;
  const bool commands = std::ranges::any_of(tools, [](const Tool& tool) {
    return tool.capabilities & Capability(ToolCapability::kExecute);
  });
  if (read_only) {
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
  // A child gets only the tools its parent has, and a lean one none that
  // edit: where nothing here runs commands, or only read-only ones, a lean
  // child only reads, so starting one is no change to approve, as starting a
  // thread is not.
  if (!commands || read_only) {
    for (Tool& tool : tools) {
      if (!tool.delegates) continue;
      // So a brief does not ask a child for what it cannot do.
      tool.description +=
          commands
              ? " Children have only your tools and sandbox: their commands "
                "only read, and write nothing but temporary files."
              : " Children have only your tools: none of them runs commands.";
      tool.mutating = false;
      tool.mutates = [](const json& a) {
        return JsonValue(a, "mode", "lean") == "full";
      };
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
