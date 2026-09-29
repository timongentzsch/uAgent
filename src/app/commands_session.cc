// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/core/project.h"
#include "include/cli.h"
#include "include/app/commands.h"
#include "include/app/permissions.h"
#include "include/app/self_description.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/providers.h"
#include "include/tools/subagent.h"
#include "include/ui/display.h"
#include "src/app/commands_internal.h"

namespace uagent {

void SaveSessionSettings(AppSession& session) {
  session.ActiveAgent().SessionSettings(
      {{"route", RouteSelection(session.ApiClient(),
                                session.context.provider.providers)},
       {"permissions",
        PermissionOverrideName(session.context.permission_override.load())},
       {"tools", session.ActiveAgent().ToolSelectionSettings()}});
}

bool AppSession::Save(std::string& error, bool force) {
  SaveSessionSettings(*this);
  if ((force || ActiveAgent().Revision() != saved_revision ||
       !PathExists(session_file)) &&
      !ActiveAgent().Save(session_file, error)) {
    return false;
  }
  if (!context.observability.Journal().Flush(session_file + ".events.jsonl",
                                             error)) {
    return false;
  }
  saved_revision = ActiveAgent().Revision();
  return true;
}

void HandleAttach(AppSession& session, const std::string& argument,
                  CommandReply& reply) {
  if (argument.empty()) {
    if (session.attachments.empty()) {
      reply.Note(Tone::kNeutral, "no pending attachments");
    } else {
      for (const Attachment& attachment : session.attachments) {
        reply.Note(Tone::kNeutral, TerminalSafe(attachment.path) + " (" +
                                       attachment.mime + ")");
      }
    }
    return;
  }
  if (argument == "clear") {
    session.attachments.clear();
    reply.Note(Tone::kNeutral, "attachments cleared");
    return;
  }
  Attachment attachment;
  std::string error;
  // Dropped or pasted paths often arrive quoted; accept one pair.
  if (!InspectAttachment(Unquote(argument), attachment, error)) {
    reply.Note(Tone::kError, TerminalSafe(error));
    return;
  }
  session.attachments.push_back(std::move(attachment));
  reply.Note(Tone::kNeutral, "attached " + session.attachments.back().name +
                                 " for the next message");
}

void HandleCost(const AppSession& session, CommandReply& reply) {
  reply.result = {
      {"routes", session.ActiveAgent().RouteUsageJson()},
      {"total", UsageJson(session.ActiveAgent().SessionUsage())},
      {"session_budget", session.ApiClient().config.session_budget}};
  const json& routes = reply.result["routes"];
  if (routes.empty()) {
    reply.Note(Tone::kNeutral, "no session spend yet");
    return;
  }
  for (const auto& [route, usage] : routes.items()) {
    std::string cost = JsonValue(usage, "cost_reported", false)
                           ? FmtCost(JsonValue(usage, "cost", 0.0))
                           : "cost unavailable";
    // Tokens beside the cost say *why* a route is expensive — a large
    // evidence block reads very differently from many small turns.
    Usage spent;
    spent.input = JsonValue(usage, "input", int64_t{0});
    spent.output = JsonValue(usage, "output", int64_t{0});
    spent.cache_read = JsonValue(usage, "cache_read", int64_t{0});
    reply.Note(Tone::kNeutral,
               JoinDot({TerminalSafe(route), TokenSummary(spent),
                        CacheSummary(spent), cost}));
  }
  const Usage& spent = session.ActiveAgent().SessionUsage();
  std::string cost =
      spent.cost_reported ? FmtCost(spent.cost) : "cost unavailable";
  if (session.ApiClient().config.session_budget > 0) {
    cost += " / " + FmtCost(session.ApiClient().config.session_budget);
  }
  reply.Note(Tone::kNeutral, JoinDot({"total", TokenSummary(spent),
                                      CacheSummary(spent), cost}));
}

// What the session actually resolved to: the effective configuration with the
// source of each setting, then the request the model would see.
void HandleContext(AppSession& session, CommandReply& reply) {
  reply.result = {
      {"effective_config",
       session.context.config_manager.DiagnosticJson(session.Runtime().config)},
      {"capabilities", session.ApiClient().capabilities.DiagnosticJson()},
      {"model_request", session.ActiveAgent().ModelRequest()}};
  json effective = reply.result["effective_config"];
  effective["capabilities"] = reply.result["capabilities"];
  // The writable roots are the whole of what the sandbox permits, so the deep
  // view is the one place they are printed in full.
  effective["sandbox"] = SandboxDiagnosticJson();
  const json& sources = effective["sources"];
  auto source = [&](const char* key, const std::string& fallback = "runtime") {
    return sources.is_object() ? JsonValue(sources, key, fallback) : fallback;
  };
  std::string model_source = source("UAGENT_MODEL", source("OPENROUTER_MODEL"));
  std::string credential_source =
      source("UAGENT_API_KEY", source("OPENROUTER_API_KEY"));
  effective["route"] = {
      {"base_url", RedactedUrl(session.ApiClient().base_url)},
      {"base_url_source", source("UAGENT_BASE_URL")},
      {"model", session.ApiClient().RequestModel()},
      {"model_source", std::move(model_source)},
      {"credentials", session.ApiClient().api_key.empty() ||
                              session.ApiClient().api_key == kPlaceholderApiKey
                          ? "<unset>"
                          : "<set>"},
      {"credential_source", std::move(credential_source)},
      {"context_window", session.ApiClient().ctx_window}};
  reply.Print("%seffective configuration%s\n%s\n", BOLD(), RST(),
              TerminalSafe(JsonDump(effective, 2)).c_str());
  reply.Print("%smodel request%s\n", BOLD(), RST());
  reply.Print("%s\n",
              TerminalSafe(JsonDump(reply.result["model_request"], 2)).c_str());
}

// The startup row is a snapshot; MCP refresh and config reloads change the
// set mid-session, so this is the live view.

// /context is the deep live-context view. /status answers the everyday
// questions in one screen and /debug-config explains provenance.
void HandleStatus(const AppSession& session, CommandReply& reply) {
  reply.result =
      DescribeSelf(SelfTopic::kStatus, "", DescriptionInputs(session));
  const json& status = reply.result;
  auto row = [&reply](const char* label, const std::string& value) {
    reply.Print("%s", KeyValueRow(label, TerminalSafe(value)).c_str());
  };
  reply.Print("%s\u00b5Agent %s%s\n", BOLD(), kVersion, RST());
  row("route", JsonValue(status, "route", std::string()));
  row("wire api", JsonValue(status, "wire_api", std::string()));
  row("endpoint", JsonValue(status, "base_url", std::string()));
  row("effort", JsonValue(status, "effort", std::string()));
  row("approval", JsonValue(status, "approval", std::string()));
  row("web search", JsonValue(status, "web_search", std::string()));
  row("sandbox", JsonValue(status["sandbox"], "summary", std::string()));
  row("memory", JsonValue(status, "memory", false) ? "on" : "off");
  row("tools", std::to_string(JsonValue(status, "tools", int64_t{0})));
  int64_t window = JsonValue(status, "context_window", int64_t{0});
  row("context",
      window > 0 ? FmtCount(window) : std::string("provider default"));
  double budget = JsonValue(status, "session_budget", 0.0);
  if (budget > 0) row("session budget", FmtCost(budget));
  const json& restart = status["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    std::string names;
    for (const json& key : restart) {
      names += (names.empty() ? "" : ", ") + key.get<std::string>();
    }
    row("restart needed", names);
  }
}

// The person edits instruction files directly here, so no approval stands
// between them and the file; an agent asks through uagent set_instructions.
void HandleInstructions(AppSession& session, const std::string& argument,
                        CommandReply& reply) {
  std::istringstream words(argument);
  std::string action, audience, scope;
  words >> action >> audience >> scope;
  const std::string cwd = CanonicalCwd();
  if (action == "clear") {
    Agent& agent = session.ActiveAgent();
    json shown = agent.SelfDirective({{"action", "show"}});
    json cleared = agent.SelfDirective(
        {{"action", "reset"}, {"revision", shown["item"]["revision"]}});
    reply.Note(Tone::kNeutral,
               cleared.contains("error")
                   ? TerminalSafe(JsonValue(cleared, "error", ""))
                   : "self-directive cleared");
    return;
  }
  if (action == "edit") {
    bool coordinator = false, project = false;
    if (!ParseInstructionTarget(audience, scope, coordinator, project)) {
      reply.Print("%s", "usage: /instructions edit sessions|coordinator "
                        "user|project\n");
      return;
    }
    const auto path = InstructionPath(coordinator, project, cwd);
    const std::string base = ReadInstructionFile(path);
    bool cancelled = false;
    const std::string text = ReadInteraction(
        {.kind = "editor", .prompt = path.string(), .initial = base},
        &cancelled);
    if (cancelled) {
      reply.Note(Tone::kNeutral, "instructions unchanged");
      return;
    }
    const std::string error =
        WriteInstructionFile(coordinator, project, cwd, text, base);
    reply.Note(Tone::kNeutral,
               error.empty() ? "saved; new and restarted sessions read it"
                             : TerminalSafe(error));
    return;
  }
  if (!action.empty()) {
    reply.Print("%s", "usage: /instructions [edit sessions|coordinator "
                      "user|project | clear]\n");
    return;
  }
  // The stack in the order a session reads it, after the built-in base.
  const json stack = InstructionFiles(cwd);
  for (const json& file : stack["files"]) {
    const std::string text = JsonValue(file, "text", "");
    reply.Print("%s%s · %s%s  %s\n%s\n\n", BOLD(),
                JsonValue(file, "scope", "") == "user" ? "yours" : "project",
                JsonValue(file, "audience", "") == "coordinator"
                    ? "coordinator"
                    : "every session",
                RST(), TerminalSafe(JsonValue(file, "path", "")).c_str(),
                text.empty() ? "(empty)" : TerminalSafe(text).c_str());
  }
  for (const json& also : stack["also_loaded"]) {
    reply.Print("also read: %s\n",
                TerminalSafe(also.get<std::string>()).c_str());
  }
  const json self = session.ActiveAgent().SelfDirective({{"action", "show"}});
  if (!JsonValue(self["item"], "text", "").empty()) {
    reply.Print("%sself-directive%s (this conversation, %s)\n%s\n", BOLD(),
                RST(), JsonValue(self["item"], "mode", "").c_str(),
                TerminalSafe(JsonValue(self["item"], "text", "")).c_str());
  }
}

void HandleDebugConfig(const AppSession& session, const std::string& argument,
                       CommandReply& reply) {
  reply.result =
      DescribeSelf(SelfTopic::kConfig, argument, DescriptionInputs(session));
  const json& described = reply.result;
  const json& settings = described["settings"];
  if (settings.empty()) {
    reply.Note(Tone::kError, "no setting named " + TerminalSafe(argument));
    return;
  }
  // Only settings the user actually influenced, unless one was named: the full
  // schema belongs in the generated reference, not in a terminal dump.
  bool named = !argument.empty();
  reply.Print("%sconfiguration%s\n", BOLD(), RST());
  for (const json& setting : settings) {
    std::string source = JsonValue(setting, "source", std::string("default"));
    if (!named && source == "default") continue;
    std::string name = JsonValue(setting, "name", std::string());
    reply.Print("  %s%s%s\n", BOLD(), TerminalSafe(name).c_str(), RST());
    auto row = [&reply](const char* label, const std::string& value) {
      reply.Print(
          "%s", KeyValueRow(label, TerminalSafe(value), kDetailIndent).c_str());
    };
    row("source", source);
    if (setting.contains("active")) row("active", JsonDump(setting["active"]));
    row("default", JsonDump(setting["default"]));
    row("takes effect", JsonValue(setting, "takes_effect", std::string()));
  }
  const json& restart = described["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    reply.Note(Tone::kWarn, "restart required for " +
                                std::to_string(restart.size()) +
                                " changed setting" +
                                (restart.size() == 1 ? "" : "s"));
  }
  reply.Note(Tone::kNeutral,
             "precedence: command line, process environment, trusted "
             "project config, user config, built-in default");
}

// The settings a layer changes, as text: what differs from its default and
// where it comes from, then how a change takes effect.
void HandleConfig(AppSession& session, const std::string& argument,
                  CommandReply& reply) {
  json request = {{"kind", "config"}};
  if (!argument.empty()) {
    std::istringstream input(argument);
    std::string scope, change;
    input >> scope;
    std::getline(input, change);
    change = Trim(change);
    const bool unset = change.starts_with("unset ");
    const size_t equal = change.find('=');
    if (unset) change = Trim(change.substr(6));
    request.update({{"scope", scope}});
    if (change == "reset") {
      request["operation"] = "reset";
    } else {
      request.update(
          {{"operation", "apply"},
           {"changes",
            json::array({{{"key", unset ? change : change.substr(0, equal)},
                          {"value", unset || equal == std::string::npos
                                        ? ""
                                        : change.substr(equal + 1)},
                          {"unset", unset}}})}});
    }
  }
  reply.result = SessionControl(session, request);
  const json& result = reply.result;
  if (result.contains("error")) {
    reply.Note(Tone::kError,
               "error: " + TerminalSafe(JsonValue(result, "error", "")));
    return;
  }
  bool restart = false;
  for (const json& effect : JsonValue(result, "effects", json::array())) {
    const std::string how = JsonValue(effect, "effect", "");
    restart |= how == "needs a restart";
    reply.Note(Tone::kNeutral,
               TerminalSafe(JsonValue(effect, "key", "")) + ": " + how);
  }
  if (restart) {
    reply.Note(Tone::kWarn,
               "/restart applies it here; new conversations have it already");
  }
  if (!argument.empty()) return;
  size_t changed = 0;
  for (const json& setting : JsonValue(result, "settings", json::array())) {
    const std::string source = JsonValue(setting, "source", "default");
    if (source == "default") continue;
    ++changed;
    const json& value = setting["value"];
    reply.Print("%s = %s%s · %s%s\n",
                TerminalSafe(JsonValue(setting, "name", "")).c_str(), DIM(),
                value.is_null()
                    ? "configured"
                    : TerminalSafe(value.is_string() ? value.get<std::string>()
                                                     : JsonDump(value))
                          .c_str(),
                source.c_str(), RST());
  }
  if (!changed) {
    reply.Note(Tone::kNeutral, "every setting is at its default");
  }
  reply.Note(Tone::kNeutral,
             "/config user|project KEY=VALUE, unset KEY, or reset "
             "(keeps secrets)");
}

// This repository's remembered actions, numbered so one can be forgotten,
// as in the web settings.
void HandlePermissionRules(const std::string& argument, CommandReply& reply) {
  json listed = PermissionRulesControl({{"action", "list"}});
  if (argument != "rules" && !listed.contains("error")) {
    const std::string which = Trim(argument.substr(6));
    int64_t index = 0;
    const json& rules = listed["rules"];
    if (which == "all") {
      listed = PermissionRulesControl({{"action", "clear"}});
    } else if (ParseInt64(which.c_str(), index) && index >= 1 &&
               index <= static_cast<int64_t>(rules.size())) {
      listed = PermissionRulesControl(
          {{"action", "delete"},
           {"key",
            JsonValue(rules[static_cast<size_t>(index - 1)], "key", "")}});
    } else {
      listed = {{"error", "usage: /permissions forget N|all"}};
    }
  }
  reply.result = listed;
  if (listed.contains("error")) {
    reply.Note(Tone::kError,
               "error: " + TerminalSafe(JsonValue(listed, "error", "")));
    return;
  }
  const json& rules = listed["rules"];
  if (rules.empty()) {
    reply.Note(Tone::kNeutral, "no remembered actions for this repository");
  }
  for (size_t i = 0; i < rules.size(); ++i) {
    reply.Print("%zu. %s %s· %s%s\n", i + 1,
                TerminalSafe(JsonValue(rules[i], "tool", "")).c_str(), DIM(),
                TerminalSafe(JsonValue(rules[i], "preview", "")).c_str(),
                RST());
  }
}

// The same overview as the web settings: each server by scope, its state,
// and why it is not running; retry, on and off act on one server.
void HandleMcp(AppSession& session, const std::string& argument,
               CommandReply& reply) {
  AppContext& app = session.context;
  if (!argument.empty()) {
    std::istringstream input(argument);
    std::string operation, name, extra;
    input >> operation >> name >> extra;
    if ((operation != "retry" && operation != "on" && operation != "off") ||
        name.empty() || !extra.empty()) {
      reply.Note(Tone::kError, "error: usage: /mcp [retry|on|off NAME]");
      return;
    }
    json done = SessionControl(
        session,
        {{"kind", "tools"},
         {"operation", operation == "retry" ? "mcp_restart" : "mcp_enable"},
         {"name", name},
         {"enabled", operation == "on"}});
    if (done.contains("error")) {
      reply.Note(Tone::kError,
                 "error: " + TerminalSafe(JsonValue(done, "error", "")));
      return;
    }
  }
  const json servers = McpStatus(app.runtime.mcp, app.tools);
  reply.result = {{"mcp", servers}};
  if (servers.empty()) {
    reply.Note(Tone::kNeutral, "no MCP servers (~/.mcp.json, ./.mcp.json)");
  }
  for (const json& server : servers) {
    const std::string state = JsonValue(server, "state", "");
    std::string detail =
        state == "ready"
            ? std::to_string(JsonValue(server, "tools", int64_t{0})) + " tools"
        : state == "failed" ? JsonValue(server, "error", "")
                            : state;
    if (JsonValue(server, "overrides", false)) detail += " · overrides global";
    reply.Print("%s %s %s· %s · %s%s\n", StatusMark(state).c_str(),
                TerminalSafe(JsonValue(server, "name", "")).c_str(), DIM(),
                JsonValue(server, "scope", "").c_str(),
                TerminalSafe(detail).c_str(), RST());
  }
}

void HandleTools(AppSession& session, const std::string& argument,
                 CommandReply& reply) {
  json request = {{"kind", "tools"}, {"operation", "catalog"}};
  if (!argument.empty()) {
    std::istringstream input(argument);
    std::string operation, value, extra;
    input >> operation >> value >> extra;
    if (!extra.empty() ||
        (operation != "reset" && operation != "profile" && operation != "on" &&
         operation != "off") ||
        ((operation == "reset") != value.empty())) {
      reply.Note(Tone::kError,
                 "error: usage: /tools [on|off NAME|profile NAME|reset]");
      return;
    }
    request["operation"] =
        operation == "on" || operation == "off" ? "set" : operation;
    if (operation == "profile") request["profile"] = value;
    if (operation == "on" || operation == "off") {
      request["name"] = value;
      request["active"] = operation == "on";
    }
  }
  reply.result = SessionControl(session, request);
  const json& result = reply.result;
  if (result.contains("error")) {
    reply.Note(Tone::kError,
               "error: " + TerminalSafe(JsonValue(result, "error", "")));
    return;
  }
  reply.Note(
      Tone::kNeutral,
      std::to_string(JsonValue(result, "active", int64_t{0})) + "/" +
          std::to_string(JsonValue(result, "available", int64_t{0})) +
          " tools · " +
          TerminalSafe(JsonValue(result, "profile", "default")) + " · " +
          std::to_string(JsonValue(result, "schema_bytes", int64_t{0})) +
          " serialized schema bytes");
  if (const json* tools = JsonArray(result, "tools")) {
    for (const json& tool : *tools) {
      reply.Print("%s %s%-14s %s · %s%s\n",
                  StatusMark(JsonValue(tool, "active", false) ? "ready"
                                                              : "disabled")
                      .c_str(),
                  DIM(),
                  TerminalSafe(JsonValue(tool, "name", "")).c_str(),
                  TerminalSafe(JsonValue(tool, "category", "")).c_str(),
                  TerminalSafe(JsonValue(tool, "description", "")).c_str(),
                  RST());
    }
  }
}

// The child agents the subagent tool reports, joined with what the
// supervisor knows about the ones still running. The id is the join key: it is
// what the spawn stamped on the job, and it is what the human types back.
json AgentsJson(const AppSession& session) {
  const ProcessSupervisor& processes = session.Runtime().processes;
  std::vector<SubagentView> live = processes.SubagentViews();
  json rows = json::array();
  for (json& record : AgentSummaries(processes)) {
    const std::string id = JsonValue(record, "id", std::string());
    for (const SubagentView& view : live) {
      if (view.source_id != id) continue;
      record["elapsed_ms"] =
          std::chrono::duration_cast<std::chrono::milliseconds>(view.elapsed)
              .count();
      if (!view.tail.empty()) record["progress"] = view.tail;
      break;
    }
    rows.push_back(std::move(record));
  }
  return rows;
}

SelfDescriptionInputs DescriptionInputs(const AppSession& session) {
  return {session.context.config_manager, session.Runtime().config,
          session.ApiClient(), session.context.tools, &session.ActiveAgent()};
}

}  // namespace uagent
