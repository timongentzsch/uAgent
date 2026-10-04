// Copyright 2026 Timon Gentzsch

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/agent/session_store.h"
#include "include/app/commands.h"
#include "include/app/config_proposal.h"
#include "include/app/launch.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/mcp/register.h"
#include "include/providers.h"
#include "include/tools/subagent.h"
#include "src/app/commands_internal.h"

namespace uagent {

json PermissionControl(AppContext& context, const json& request) {
  // The mode is the approval setting; a conversation's own is that setting
  // chosen at its scope, and "default" takes the choice back.
  const std::string key = "UAGENT_APPROVAL";
  std::string mode = JsonValue(request, "mode", "");
  if (!mode.empty()) {
    ApprovalMode chosen = ApprovalMode::kAsk;
    if (mode != "default" && !ParseApprovalMode(mode, chosen)) {
      return {{"error", "unknown permission mode"}};
    }
    context.config_manager.ChooseForConversation(
        key, mode == "default" ? "" : ApprovalModeName(chosen));
  }
  const auto configured = context.config_manager.Read();
  // An unreadable value reads as asking.
  auto parsed = [](const std::string& text) {
    ApprovalMode value = ApprovalMode::kAsk;
    if (!ParseApprovalMode(text, value)) value = ApprovalMode::kAsk;
    return value;
  };
  const auto value = configured.values.find(key);
  const ApprovalMode effective =
      parsed(value == configured.values.end() ? "" : value->second);
  SetApprovalMode(effective);
  json result = {
      {"mode", JsonValue(configured.sources, key.c_str(), "") == "conversation"
                   ? ApprovalModeName(effective)
                   : "default"},
      {"effective", ApprovalModeName(effective)},
      {"default", ApprovalModeName(parsed(configured.Inherited(key)))}};
  if (!mode.empty()) {
    Emit(Event{EventId::kConfigChanged, {{"permissions", result}}});
  }
  return result;
}

json SessionControl(AppSession& session, const json& request) {
  std::string kind = JsonValue(request, "kind", "");
  if (kind == "self_directive") {
    return session.ActiveAgent().SelfDirective(request);
  }
  if (kind == "permissions") return PermissionControl(session.context, request);
  if (kind == "tools" &&
      JsonValue(request, "operation", "").starts_with("mcp_")) {
    AppContext& app = session.context;
    json result =
        McpControl(request, app.tools, app.runtime.mcp, app.runtime.config);
    if (app.runtime.mcp.registry_changed) {
      ApplyToolPolicy(app.tools, app.tool_policy);
      app.session_approvals.clear();
    }
    return result;
  }
  if (kind == "tools") {
    json result = session.ActiveAgent().ConfigureTools(request);
    if (!result.contains("error") &&
        JsonValue(request, "operation", "catalog") != "catalog") {
      SaveSessionSettings(session);
    }
    return result;
  }
  if (kind == "fork" || kind == "share") {
    if (session.session_file.empty()) {
      if (kind != "fork") return {{"error", "session has no file yet"}};
      session.session_file = HistoryPath(CanonicalCwd(), MakeSessionId());
    }
    std::string error;
    // A folder has one coordinator, so "edit from here" rewinds it in place
    // rather than starting an ordinary conversation beside it.
    if (kind == "fork" && session.context.options.Coordinator() &&
        (JsonValue(request, "turn", int64_t{0}) > 0 ||
         !JsonValue(request, "message_id", "").empty())) {
      json rewound = session.ActiveAgent().RewindBefore(
          JsonValue(request, "turn", int64_t{0}),
          JsonValue(request, "message_id", ""));
      if (rewound.contains("error")) return rewound;
      if (!session.Save(error)) return {{"error", error}};
      rewound["rewound"] = true;
      rewound["id"] = HashHex(session.session_file);
      rewound["path"] = session.session_file;
      rewound["cwd"] = CanonicalCwd();
      return rewound;
    }
    if (!session.Save(error)) return {{"error", error}};
    if (kind == "share") return SessionStore::Share(session.session_file);
    return SessionStore::Fork(session.session_file,
                              JsonValue(request, "title", ""), true,
                              JsonValue(request, "turn", int64_t{0}),
                              JsonValue(request, "message_id", ""));
  }
  if (kind == "config") {
    return ConfigurationControl(
        request, session.context.config_manager,
        session.context.config_manager.ProjectTrusted());
  }
  if (kind == "revert") {
    return session.ActiveAgent().Revert(JsonValue(request, "turn", int64_t{0}),
                                        JsonValue(request, "path", ""));
  }
  if (kind == "context") {
    json preview = session.ActiveAgent().PreviewContext();
    if (preview.contains("error")) return preview;
    return {{"exchanges", json::array({std::move(preview)})}};
  }

  if (JsonValue(request, "kind", "") == "activity") {
    if (JsonValue(request, "operation", "") != "followup") {
      return ActivityControl(session.Runtime().processes, request);
    }
    Tool tool =
        SubagentTool(session.ApiClient(), session.Runtime().processes,
                     session.context.provider.routes,
                     session.context.provider.providers, Debug().Enabled());
    json arguments = {{"operation", "followup"},
                      {"agent_id", JsonValue(request, "agent_id", "")},
                      {"prompt", JsonValue(request, "text", "")}};
    const std::string model = JsonValue(request, "model", "");
    if (!model.empty()) arguments["model"] = model;
    ToolResult result = tool.run(arguments, ToolContext{});
    return result.Ok() ? json{{"output", result.output}}
                       : json{{"error", result.output}};
  }
  if (JsonValue(request, "operation", "") == "catalog") {
    return ModelCatalogue(session.ApiClient(), session.context.provider.routes,
                          session.context.provider.providers,
                          JsonValue(request, "query", "all"));
  }
  if (JsonValue(request, "operation", "") != "select") {
    return {{"error", "unknown model operation"}};
  }
  std::string value = JsonValue(request, "model", "");
  ModelSelection selection = ParseModelSelection(value);
  std::string variant = JsonValue(request, "variant", "default");
  std::string effort = JsonValue(request, "effort", "default");
  if (variant == "default") variant.clear();
  if (effort == "default") effort.clear();
  if (selection.base.empty() || !ValidOpenRouterVariant(variant) ||
      (!effort.empty() && !ValidEffort(effort))) {
    return {{"error", "invalid model selection"}};
  }
  value = selection.base;
  if (!variant.empty()) value += ":" + variant;
  if (!effort.empty()) value += ":" + effort;
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, value);
  if (selected.empty()) return {{"error", "unknown model"}};
  SaveSelectedModel(session, selected, JsonValue(request, "default", false));
  return {{"route", RouteSelection(session.ApiClient(),
                                   session.context.provider.providers)}};
}

json ActivityControl(ProcessSupervisor& processes, const json& request) {
  std::string operation = JsonValue(request, "operation", "list");
  if (operation == "background") {
    return processes.RequestForegroundBackground()
               ? json{{"operation", operation}}
               : json{{"error", "no foreground process to background"}};
  }
  if (operation == "list") return {{"activities", processes.ActivityViews()}};
  int64_t id = JsonValue(request, "activity_id", int64_t{0});
  auto job = processes.Find(id);
  std::string agent = job ? job->source_id : JsonValue(request, "agent_id", "");
  if (operation == "inspect") {
    json result = job ? processes.InspectActivity(id) : json::object();
    if (!agent.empty()) {
      result.update(InspectAgent(processes, agent, request));
    }
    return result.empty()
               ? json{{"error", "activity unavailable in this conversation"}}
               : result;
  }
  ToolResult result;
  if (operation == "message" && !agent.empty() &&
      !JsonValue(request, "text", "").empty()) {
    // An idle child is messaged by id; ownership is its header's to decide.
    result = MessageAgent(processes, agent, JsonValue(request, "text", ""));
  } else if (!job) {
    return {{"error", "activity unavailable in this conversation"}};
  } else if (operation == "stop") {
    result = ToolActivityStop(processes, id);
  } else {
    return {{"error", "unsupported activity operation"}};
  }
  return result.Ok() ? json{{"output", result.output}, {"operation", operation}}
                     : json{{"error", result.output}};
}

std::string ActivityText(const json& result) {
  for (const char* key : {"activities", "agents"}) {
    if (const json* rows = JsonArray(result, key)) {
      const bool agents = std::string_view(key) == "agents";
      std::string text = std::string(agents ? "subagents" : "background work") +
                         " (" + FmtCount(static_cast<int64_t>(rows->size())) +
                         ")\n";
      for (const json& row : *rows) {
        if (JsonValue(row, "detached", false)) text += "[detached] activity ";
        auto id = row.find("id");
        const std::string id_text = id == row.end()   ? "—"
                                    : id->is_string() ? id->get<std::string>()
                                                      : JsonDump(*id);
        if (agents) {
          // Agents are named teammates, not tasks: name first, id for reuse.
          const std::string name = JsonValue(row, "name", "");
          text += name.empty() ? id_text : name + " (" + id_text + ")";
        } else {
          text += id_text;
        }
        text += "  " +
                JoinDot({JsonValue(row, "status", ""),
                         JsonValue(row, "mode", JsonValue(row, "label", "")),
                         JsonValue(row, "model", ""),
                         JsonValue(row, "progress", "")}) +
                "\n";
        if (agents) {
          const std::string about =
              Utf8Trunc(JsonValue(row, "description", ""), 120);
          if (!about.empty()) text += "  " + about + "\n";
        }
      }
      return text;
    }
  }
  return JsonDump(result, 2) + "\n";
}

json ActivityCommand(AppSession& session, const ParsedSlashCommand& command) {
  std::istringstream input(command.argument);
  std::string target, operation, text;
  input >> target >> operation;
  std::getline(input, text);
  text = Trim(text);
  auto& processes = session.Runtime().processes;
  if (target.empty()) {
    return command.spec->id == SlashCommandId::kAgents
               ? json{{"agents", AgentsJson(session)}}
               : json{{"activities", processes.ActivityViews()}};
  }
  json request = {{"kind", "activity"},
                  {"operation", operation.empty() ? "inspect" : operation},
                  {"text", text}};
  if (operation == "output") request["operation"] = "inspect";
  if (command.spec->id == SlashCommandId::kAgents) {
    request["agent_id"] = target;
    for (const json& row : processes.ActivityViews()) {
      if (JsonValue(row, "agent_id", "") == target) {
        request["activity_id"] = row["id"];
      }
    }
  } else {
    int64_t id = 0;
    if (!ParseInt64(target.c_str(), id)) {
      return {{"error", "invalid activity id"}};
    }
    request["activity_id"] = id;
  }
  return SessionControl(session, request);
}

}  // namespace uagent
