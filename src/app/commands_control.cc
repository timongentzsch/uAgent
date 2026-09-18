// Copyright 2026 Timon Gentzsch

#include "include/app/commands.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/prompt_control.h"
#include "include/app/self_description.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/tools/memory.h"
#include "include/agent/process.h"
#include "include/tools/subagent.h"
#include "include/ui/conversation.h"
#include "include/ui/sessions.h"

#include "src/app/commands_internal.h"
namespace uagent {

json CommandResult(const AppSession& session,
                   const ParsedSlashCommand& command) {
  switch (command.spec->id) {
    case SlashCommandId::kHelp:
      return DescribeSelf(SelfTopic::kCommands, "", DescriptionInputs(session));
    case SlashCommandId::kStatus:
      return DescribeSelf(SelfTopic::kStatus, "", DescriptionInputs(session));
    case SlashCommandId::kDebugConfig:
      return DescribeSelf(SelfTopic::kConfig, command.argument,
                          DescriptionInputs(session));
    case SlashCommandId::kTools:
      return DescribeSelf(SelfTopic::kTools, "", DescriptionInputs(session));
    case SlashCommandId::kTrace:
      return {{"trace", session.ActiveAgent().LatestToolTrace()}};
    case SlashCommandId::kContext:
      return {
          {"effective_config", session.context.config_manager.DiagnosticJson(
                                   session.Runtime().config)},
          {"capabilities", session.ApiClient().capabilities.DiagnosticJson()},
          {"model_request", session.ActiveAgent().ModelRequest()}};
    case SlashCommandId::kCost:
      return {{"routes", session.ActiveAgent().RouteUsageJson()},
              {"total", UsageJson(session.ActiveAgent().SessionUsage())},
              {"session_budget", session.ApiClient().config.session_budget}};
    case SlashCommandId::kPrompt:
    case SlashCommandId::kMemory:
    case SlashCommandId::kSkills:
    case SlashCommandId::kSchedule:
      return json::object();  // management commands return their operation
                              // result directly
    case SlashCommandId::kProcesses: {
      ToolResult activities = ToolActivityList(session.Runtime().processes);
      return {{"activities", activities.output}};
    }
    case SlashCommandId::kAgents:
      return {{"collaborators", AgentsJson(session)}};
    case SlashCommandId::kAttach: {
      json attachments = json::array();
      for (const Attachment& attachment : session.attachments) {
        attachments.push_back({{"name", attachment.name},
                               {"path", attachment.path},
                               {"mime", attachment.mime}});
      }
      return {{"attachments", std::move(attachments)}};
    }
    default:
      return json::object();
  }
}

json PermissionControl(AppContext& context, const json& request) {
  std::string mode = JsonValue(request, "mode", "");
  if (!mode.empty()) {
    if (mode != "default" && mode != "ask" && mode != "yolo") {
      return {{"error", "unknown permission mode"}};
    }
    context.permission_override.store(mode == "default" ? -1
                                      : mode == "yolo"  ? 1
                                                        : 0);
  }
  auto configured = context.config_manager.Read();
  auto value = configured.values.find("UAGENT_APPROVAL");
  bool automatic = value != configured.values.end() && value->second == "yolo";
  const std::string default_mode = automatic ? "yolo" : "ask";
  int override = context.permission_override.load();
  if (override >= 0) automatic = override == 1;
  SetApprovalAutomatic(automatic);
  json result = {{"mode", override < 0 ? "default"
                          : override   ? "yolo"
                                       : "ask"},
                 {"effective", automatic ? "yolo" : "ask"},
                 {"default", default_mode}};
  if (!mode.empty()) {
    Emit(Event{EventId::kConfigChanged, {{"permissions", result}}});
  }
  return result;
}

json SessionControl(AppSession& session, const json& request) {
  std::string kind = JsonValue(request, "kind", "");
  if (kind == "prompt") {
    return session.ActiveAgent().PromptConfiguration(request);
  }
  if (kind == "permissions") return PermissionControl(session.context, request);
  if (kind == "fork") {
    std::string error;
    if (session.session_file.empty()) {
      session.session_file = UagentDir(kHistoryDir) + "/" +
                             WorkspaceId(CanonicalCwd()) + "/" +
                             MakeSessionId() + ".json";
    }
    SaveSessionSettings(session);
    if (!session.ActiveAgent().Save(session.session_file, error)) {
      return {{"error", error}};
    }
    return SessionStore::Fork(session.session_file,
                              JsonValue(request, "title", ""), true,
                              JsonValue(request, "turn", int64_t{0}));
  }
  if (kind == "rewind") {
    const int64_t turn = JsonValue(request, "turn", int64_t{0});
    if (turn <= 0) return {{"error", "usage: /rewind [@]TURN"}};
    if (session.session_file.empty()) {
      return {{"error", "session has no file yet"}};
    }
    std::string error;
    if (!session.ActiveAgent().RewindToTurn(turn, error)) {
      return {{"error", error}};
    }
    SaveSessionSettings(session);
    if (!session.ActiveAgent().Save(session.session_file, error)) {
      return {{"error", error}};
    }
    return {{"rewound", true}, {"turns", turn - 1}};
  }
  if (kind == "share") {
    if (session.session_file.empty()) {
      return {{"error", "session has no file yet"}};
    }
    std::string error;
    SaveSessionSettings(session);
    if (!session.ActiveAgent().Save(session.session_file, error)) {
      return {{"error", error}};
    }
    return SessionStore::Share(session.session_file);
  }
  if (kind == "config") {
    return ConfigurationControl(
        request, session.context.config_manager, session.Runtime().config,
        session.context.config_manager.ProjectTrusted());
  }
  if (kind == "context") {
    json preview = session.ActiveAgent().PreviewContext();
    if (preview.contains("error")) return preview;
    return {{"exchanges", json::array({std::move(preview)})}};
  }

  if (JsonValue(request, "kind", "") == "activity") {
    if (JsonValue(request, "operation", "") != "followup") {
      return ActivityControl(session.Runtime().processes, request,
                             &session.Runtime().collaborator);
    }
    Tool tool = SubagentTool(
        session.ApiClient(), session.Runtime().processes,
        session.context.provider.routes, session.context.provider.providers,
        Debug().Enabled(), &session.Runtime().collaborator);
    ToolResult result =
        tool.run({{"operation", "followup"},
                  {"agent_id", JsonValue(request, "agent_id", "")},
                  {"prompt", JsonValue(request, "text", "")}},
                 ToolContext{});
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
  SaveSelectedModel(session, selected);
  return {{"route", RouteSelection(session.ApiClient(),
                                   session.context.provider.providers)}};
}

json ActivityControl(ProcessSupervisor& processes, const json& request,
                     CollaboratorRuntime* runtime) {
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
      result.update(InspectCollaborator(processes, agent, request, runtime));
    }
    return result.empty()
               ? json{{"error", "activity unavailable in this conversation"}}
               : result;
  }
  if (!job && runtime && !agent.empty()) {
    ToolResult control;
    if (operation == "stop") {
      control = runtime->Stop(agent);
    } else if (operation == "message" &&
               !JsonValue(request, "text", "").empty()) {
      control = MessageCollaborator(processes, runtime, agent,
                                    JsonValue(request, "text", ""));
    } else {
      return {{"error", "unsupported activity operation"}};
    }
    return control.Ok()
               ? json{{"output", control.output}, {"operation", operation}}
               : json{{"error", control.output}};
  }
  if (!job) return {{"error", "activity unavailable in this conversation"}};
  ToolResult result;
  if (operation == "stop") {
    result = ToolActivityStop(processes, id);
  } else if (operation == "message" && !job->source_id.empty() &&
             !JsonValue(request, "text", "").empty()) {
    result = MessageCollaborator(processes, runtime, job->source_id,
                                 JsonValue(request, "text", ""));
  } else {
    return {{"error", "unsupported activity operation"}};
  }
  return result.Ok() ? json{{"output", result.output}, {"operation", operation}}
                     : json{{"error", result.output}};
}

std::string ActivityText(const json& result) {
  for (const char* key : {"activities", "collaborators"}) {
    if (const json* rows = JsonArray(result, key)) {
      const bool agents = std::string_view(key) == "collaborators";
      std::string text = std::string(agents ? "subagents" : "background work") +
                         " (" + FmtCount(static_cast<int64_t>(rows->size())) + ")\n";
      for (const json& row : *rows) {
        if (JsonValue(row, "detached", false)) text += "[detached] activity ";
        auto id = row.find("id");
        const std::string id_text =
            id == row.end()   ? "—"
            : id->is_string() ? id->get<std::string>()
                              : JsonDump(*id);
        if (agents) {
          // Agents are named teammates, not tasks: name first, id for reuse.
          const std::string name = JsonValue(row, "name", "");
          text += name.empty() ? id_text : name + " (" + id_text + ")";
        } else {
          text += id_text;
        }
        text += "  " + JsonValue(row, "status", "") + " · " +
                JsonValue(row, "mode", JsonValue(row, "label", ""));
        for (const char* field : {"model", "progress"}) {
          const std::string value = JsonValue(row, field, "");
          if (!value.empty()) text += " · " + value;
        }
        if (agents && JsonValue(row, "persistent", false)) text += " · persistent";
        text += "\n";
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
               ? json{{"collaborators", AgentsJson(session)}}
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
