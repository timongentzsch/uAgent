// Copyright 2026 Timon Gentzsch

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/agent/session_store.h"
#include "include/app/commands.h"
#include "include/app/config_proposal.h"
#include "include/app/launch.h"
#include "include/app/permissions.h"
#include "include/core/config_registry.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/runtime_config.h"
#include "include/core/sandbox.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/workspace.h"
#include "include/mcp/register.h"
#include "include/providers/providers.h"
#include "src/app/commands_internal.h"

namespace uagent {

// The mode is the approval setting; a conversation's own is that setting
// chosen at its scope, and "default" takes the choice back.
constexpr const char* kApprovalKey = Cfg("UAGENT_APPROVAL").EnvName();

json ApprovalState(const EffectiveConfigSnapshot& configured,
                   const json& agent) {
  // An unreadable value reads as asking.
  auto parsed = [](const std::string& text) {
    ApprovalMode value = ApprovalMode::kAsk;
    if (!ParseApprovalMode(text, value)) value = ApprovalMode::kAsk;
    return value;
  };
  std::string source = JsonValue(configured.sources, kApprovalKey, "default");
  const bool chosen = source == "conversation";
  ApprovalMode effective = parsed(configured.Get(kApprovalKey));
  ApprovalMode inherited = parsed(configured.Inherited(kApprovalKey));
  if (!agent.empty()) {
    inherited = parsed(JsonValue(agent, "approval", ""));
    if (!chosen) {
      effective = inherited;
      source = "parent";
    }
  }
  return {{"mode", chosen ? ApprovalModeName(effective) : "default"},
          {"effective", ApprovalModeName(effective)},
          {"source", source},
          {"default", ApprovalModeName(inherited)}};
}

json PermissionControl(AppContext& context, const json& request) {
  // A call refused without asking, allowed for the rest of the conversation.
  if (const json* allow = JsonObject(request, "allow")) {
    PermissionRule rule{JsonValue(*allow, "tool", ""),
                        JsonValue(*allow, "match", "")};
    if (rule.tool.empty() || rule.match.empty()) {
      return {{"error", "allow needs a tool and a match"}};
    }
    context.session_rules.Add(std::move(rule));
  }
  std::string mode = JsonValue(request, "mode", "");
  if (!mode.empty()) {
    ApprovalMode chosen = ApprovalMode::kAsk;
    if (mode != "default" && !ParseApprovalMode(mode, chosen)) {
      return {{"error", "unknown permission mode"}};
    }
    context.config_manager.ChooseForConversation(
        kApprovalKey, mode == "default" ? "" : ApprovalModeName(chosen));
  }
  json result =
      ApprovalState(context.config_manager.Read(), context.options.AgentRole());
  ApprovalMode effective = ApprovalMode::kAsk;
  ParseApprovalMode(JsonValue(result, "effective", ""), effective);
  // Planning starts afresh: a plan from before is not put to the person
  // again.
  if (!mode.empty() && effective == ApprovalMode::kPlan &&
      CurrentApprovalMode() != ApprovalMode::kPlan) {
    std::error_code ignored;
    std::filesystem::remove(PlanFile(), ignored);
  }
  SetApprovalMode(effective);
  if (JsonValue(request, "inspect", false)) {
    const json sandbox = SandboxDiagnosticJson();
    result["sandbox"] = JsonPick(sandbox, {"summary", "network"});
  }
  if (!mode.empty()) {
    Emit(Event{EventId::kConfigChanged, {{"permissions", result}}});
  }
  return result;
}

// Chooses `key` for this conversation, or takes the choice back when `value`
// is empty; the setting must allow that scope. Returns why not, or nothing.
static std::string ChooseForConversation(AppSession& session,
                                         const std::string& key,
                                         const std::string& value) {
  const ConfigDescriptor* descriptor = FindConfigDescriptor(key);
  if (!descriptor || !(descriptor->scopes & kScopeConversation)) {
    return key + " cannot be set for one conversation";
  }
  if (key == Cfg("UAGENT_APPROVAL").EnvName()) {
    return JsonValue(
        PermissionControl(session.context,
                          {{"mode", value.empty() ? "default" : value}}),
        "error", "");
  }
  ConfigManager& manager = session.context.config_manager;
  // The model: back on the configured one, or on the one named.
  const std::string wanted =
      value.empty() ? manager.Read().Inherited(key) : value;
  if (value.empty()) manager.ChooseForConversation(key, "");
  if (wanted.empty()) return "";  // nothing configured: the route stays
  const std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, wanted);
  if (selected.empty()) return "unknown model " + wanted;
  if (value.empty()) {
    ActivateRoute(session.ApiClient());
    session.ActiveAgent().RouteChanged();
  } else {
    SaveSelectedModel(session, selected, false);
  }
  return "";
}

namespace {
json ForkOrShare(AppSession& session, const json& request) {
  const std::string kind = JsonValue(request, "kind", "");
  if (session.session_file.empty()) {
    if (kind != "fork") return {{"error", "session has no file yet"}};
    session.session_file = HistoryPath(MakeSessionId());
  }
  std::string error;
  if (!session.Save(error)) return {{"error", error}};
  if (kind == "share") return SessionStore::Share(session.session_file);
  return SessionStore::Fork(session.session_file,
                            JsonValue(request, "title", ""), true,
                            JsonValue(request, "turn", int64_t{0}),
                            JsonValue(request, "message_id", ""));
}

json ConfigureSession(AppSession& session, const json& request) {
  ConfigManager& manager = session.context.config_manager;
  json effects = json::array();
  // The conversation's scope is kept with it, not in a file: the choice
  // is made here and takes effect at once.
  if (JsonValue(request, "scope", "") == "conversation" &&
      JsonValue(request, "operation", "") == "apply") {
    std::vector<ConfigChange> changes;
    std::string error;
    if (!ParseConfigChanges(request, changes, error)) {
      return {{"error", error}};
    }
    for (const ConfigChange& change : changes) {
      error = ChooseForConversation(session, change.key,
                                    change.unset ? "" : change.value);
      if (!error.empty()) return {{"error", error}};
      effects.push_back({{"key", change.key},
                         {"effect", "next_turn"},
                         {"text", "applies from your next message"}});
    }
  }
  json reply = ConfigurationControl(
      effects.empty() ? request : json{{"operation", "get"}}, manager);
  if (!effects.empty()) reply["effects"] = std::move(effects);
  return reply;
}

json SelectModelControl(AppSession& session, const json& request) {
  std::string value = JsonValue(request, "model", "");
  ModelSelection selection = ParseModelSelection(value);
  std::string effort = JsonValue(request, "effort", "default");
  if (effort == "default") effort.clear();
  if (selection.base.empty() || (!effort.empty() && !ValidEffort(effort))) {
    return {{"error", "invalid model selection"}};
  }
  value = selection.base;
  if (!effort.empty()) value += ":" + effort;
  std::string selected =
      SelectModel(session.ApiClient(), session.context.provider.routes,
                  session.context.provider.providers, value);
  if (selected.empty()) return {{"error", "unknown model"}};
  SaveSelectedModel(session, selected, JsonValue(request, "default", false));
  return {{"route", RouteSelection(session.ApiClient(),
                                   session.context.provider.providers)}};
}

// A turn's change to a file as a unified diff, as git draws one between what
// the turn found and what the file holds now.
json TurnDiff(const json& texts) {
  if (texts.contains("error")) return texts;
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path folder =
      fs::temp_directory_path(ec) / ("uagent-diff-" + RandomToken(8));
  const std::string before = (folder / "before").string(),
                    after = (folder / "after").string();
  std::string error;
  CreatePrivateDirectories(folder.string());
  const bool written =
      WritePrivateFile(before, JsonValue(texts, "before", ""), error) &&
      WritePrivateFile(after, JsonValue(texts, "after", ""), error);
  const CapturedProcess diff =
      written ? HostGit(folder.string(),
                        {"diff", "--no-index", "--no-ext-diff", "--no-textconv",
                         "--no-color", "--", "before", "after"})
              : CapturedProcess{};
  fs::remove_all(folder, ec);
  // Exit code 1: the files differ.
  if (!written || diff.status < 0 || diff.status > 1) {
    return {{"error", error.empty() ? "the change could not be drawn" : error}};
  }
  const std::string path = JsonValue(texts, "path", "");
  const size_t hunk = diff.output.find("\n@@");
  return {{"path", path},
          {"text",
           "--- a/" + path + "\n+++ b/" + path +
               (hunk == std::string::npos ? "\n" : diff.output.substr(hunk))}};
}
}  // namespace

json SessionControl(AppSession& session, const json& request) {
  std::string kind = JsonValue(request, "kind", "");
  if (kind == "permissions") return PermissionControl(session.context, request);
  if (kind == "tools" &&
      JsonValue(request, "operation", "").starts_with("mcp_")) {
    AppContext& app = session.context;
    json result =
        McpControl(request, app.tools, app.runtime.mcp, app.runtime.config);
    if (app.runtime.mcp.registry_changed) {
      ApplyToolPolicy(app.tools, app.tool_policy);
      app.session_rules.Clear();
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
  if (kind == "fork" || kind == "share") return ForkOrShare(session, request);
  if (kind == "side" && JsonValue(request, "operation", "") == "detach") {
    session.ActiveAgent().DetachSide();
    return {{"detached", true}};
  }
  if (kind == "config") return ConfigureSession(session, request);
  if (kind == "revert") {
    return session.ActiveAgent().Revert(JsonValue(request, "turn", int64_t{0}),
                                        JsonValue(request, "path", ""));
  }
  if (kind == "changes") {
    return TurnDiff(
        session.ActiveAgent().TurnTexts(JsonValue(request, "turn", int64_t{0}),
                                        JsonValue(request, "path", "")));
  }
  if (kind == "context") {
    json preview = session.ActiveAgent().PreviewContext();
    if (preview.contains("error")) return preview;
    return {{"exchanges", json::array({std::move(preview)})}};
  }

  if (kind == "activity") {
    return ActivityControl(session.Runtime().processes, request);
  }
  if (JsonValue(request, "operation", "") == "catalog") {
    return ModelCatalogue(session.ApiClient(), session.context.provider.routes,
                          session.context.provider.providers);
  }
  if (JsonValue(request, "operation", "") != "select") {
    return {{"error", "unknown model operation"}};
  }
  return SelectModelControl(session, request);
}

json ActivityControl(ActivitySupervisor& processes, const json& request) {
  if (RemoteWorkspace()) {
    return WorkspaceRequest({{"op", "activity_control"}, {"request", request}});
  }
  std::string operation = JsonValue(request, "operation", "list");
  if (operation == "background") {
    return processes.RequestForegroundBackground()
               ? json{{"operation", operation}}
               : json{{"error", "no foreground process to background"}};
  }
  if (operation == "list") return {{"activities", processes.ActivityViews()}};
  int64_t id = JsonValue(request, "activity_id", int64_t{0});
  if (!processes.Find(id)) {
    return {{"error", "activity unavailable in this conversation"}};
  }
  if (operation == "inspect") return processes.InspectActivity(id);
  if (operation != "stop") {
    return {{"error", "unsupported activity operation"}};
  }
  ToolResult result = ToolActivityStop(processes, id);
  return result.Ok() ? json{{"output", result.output}, {"operation", operation}}
                     : json{{"error", result.output}};
}

std::string ActivityText(const json& result) {
  const json* rows = JsonArray(result, "activities");
  if (!rows) return JsonDump(result, 2) + "\n";
  std::string text =
      "activities (" + FmtCount(static_cast<int64_t>(rows->size())) + ")\n";
  for (const json& row : *rows) {
    if (JsonValue(row, "detached", false)) text += "[detached] activity ";
    auto id = row.find("id");
    text += (id == row.end() ? "—" : JsonText(*id)) + "  " +
            JoinDot({JsonValue(row, "status", ""), JsonValue(row, "label", ""),
                     JsonValue(row, "progress", "")}) +
            "\n";
  }
  return text;
}

json ActivityCommand(AppSession& session, const ParsedSlashCommand& command) {
  std::istringstream input(command.argument);
  std::string target, operation, text;
  input >> target >> operation;
  std::getline(input, text);
  text = Trim(text);
  if (target.empty()) {
    return {{"activities", session.Runtime().processes.ActivityViews()}};
  }
  json request = {{"kind", "activity"},
                  {"operation", operation.empty() ? "inspect" : operation},
                  {"text", text}};
  if (operation == "output") request["operation"] = "inspect";
  int64_t id = 0;
  if (!ParseInt64(target.c_str(), id)) {
    return {{"error", "invalid activity id"}};
  }
  request["activity_id"] = id;
  return SessionControl(session, request);
}

}  // namespace uagent
